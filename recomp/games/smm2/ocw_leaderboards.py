"""Finds out which of OpenCourse World's in-game leaderboards the player is on, over NEX like the game does.

The game's leaderboards are DataStore calls (protocol 0x73) that return a run of players with their ranks, from any offset and of
any length (up to about 500 at once). The boards go to 999. This logs in as ocw_identity.py does, asks for the player's own
record (the maker code to look for, and the player's figure for each board), and finds the player on each:

  - the top 100 of every board first, so a place in it shows within seconds;
  - then, for a board ordered by a figure the player has (points, clears, an Endless high score), a binary search for where
    that figure would sit, one entry per call, and a look through the entries around it;
  - for a board whose figure is not known (First Clear, World Record) or does not fit the order, a walk of the whole board.

A call costs the server about a hundredth of a second per entry plus a second to answer, and a connection that is asked for
several at once is dropped, so it is one call at a time. The result goes to a file the game reads, written after every board:

    ocw_leaderboards.py --config <ocw-config.json> --out <data folder>/ocw-boards.txt

The file is "<fingerprint> <unix time>" and then one line per board, "<board> <rank> <value>": the rank is a number, or - when
the player is not on the board, or ? when it could not be read, or ~ while it is still being looked through (a file written
part-way). The value is the player's own figure for the board where there is one. The fingerprint names the account (domain and
token) without keeping the token, as ocw_identity.py does, so another account's file is never shown.

Nothing here is needed to play: any failure is silent (exit 1). The token is only ever sent to the domain in the config, and
never printed or logged.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ocw_identity import SHARED_ACCOUNT_ID, fingerprint, read_config  # noqa: E402

DEPTH = 999         # the boards go to rank 999
PAGE = 100
SESSIONS = 6        # the server now and then drops a connection: go on from where it was, on a new one
CALL_WAIT = 30      # seconds for one call

# (board id, method, variant or None, the player's figure: (field, key) or None)
# Method 58 is the weekly Maker Points; 50 the all-time ones; 57 is Clears / First Clear / World Record by its variant byte;
# 51 is the Endless Challenge by difficulty. Their parameters are the game's own, found by capturing it (the constant words
# are the board's id as the game sends it). The weekly board is ordered by the points of the week, which the player's record does
# not hold: all-time points of 0 mean none this week, and anything else is searched without them. First Clear and World Record
# are ordered by counts that the record does not hold either (the caller may tell how many the player has: a player with none is
# not on the board).
NO_ORDER = ('weekly',)
BOARDS = [
    ('weekly',  58, None, ('maker_stats', 1)),
    ('alltime', 50, None, ('maker_stats', 1)),
    ('clears',  57, 0,    ('play_stats', 2)),
    ('first',   57, 1,    None),
    ('record',  57, 2,    None),
    ('endless0', 51, 0,   ('endless_challenge_high_scores', 0)),
    ('endless1', 51, 1,   ('endless_challenge_high_scores', 1)),
    ('endless2', 51, 2,   ('endless_challenge_high_scores', 2)),
    ('endless3', 51, 3,   ('endless_challenge_high_scores', 3)),
]
MAGIC = {50: '27620000', 57: '65620000', 51: '2d620000'}
TAIL = '040000000001020300080000000000000064000000'


def params(method, variant, offset, size):
    """The game's own request for `size` entries of a board from `offset`."""
    off, n = offset.to_bytes(4, 'little'), size.to_bytes(4, 'little')
    if method == 58:
        b = bytearray.fromhex('0015000000256600000008000000000000006400000000000000')
        b[14:18], b[18:22] = off, n
        return bytes(b)
    head = '0019000000' + MAGIC[50] if method == 50 else '001a000000%02x%s' % (variant, MAGIC[method])
    b = bytearray.fromhex(head + TAIL)
    b[-8:-4], b[-4:] = off, n
    return bytes(b)


def leaf(e):
    while getattr(e, 'exceptions', None):
        e = e.exceptions[0]
    return '%s %s' % (type(e).__name__, str(e)[:100])


def patch_strings(streams):
    """Names on the server can be cut in the middle of a character: decode them leniently instead of failing the page."""
    def string(self):
        length = self.u16()
        if length == 0:
            return ''
        return self.read(length).decode('utf8', 'replace')[:-1]
    streams.StreamIn.string = string


def value_of(user, spec):
    return int((getattr(user, spec[0], None) or {}).get(spec[1], 0))


async def scan(domain, token, write, known_zero):
    import anyio
    from anynet import http, websocket
    from nintendo.nex import authentication, backend, datastore_smm2 as datastore, prudp, settings, streams

    original_request = http.HTTPClient.request
    original_transport = prudp.connect_transport_socket
    original_key = websocket.secrets.token_urlsafe
    original_string = streams.StreamIn.string

    async def request(client, req, **kwargs):
        if req.headers.get('Upgrade', '').lower() == 'websocket':
            req.headers.pop('Sec-WebSocket-Protocol', None)
            req.headers['NEX'] = token
            req.headers['Mod-Version'] = '0.2'
            req.headers['Game-Version'] = '3.0.3'
        return await original_request(client, req, **kwargs)

    def transport(options, host, port, context):
        return websocket.connect('wss://%s:443/' % domain, context, protocols=['NEX'], disconnect_timeout=0)

    http.HTTPClient.request = request
    prudp.connect_transport_socket = transport
    websocket.secrets.token_urlsafe = lambda *a, **k: 'NEX'
    patch_strings(streams)
    result = {}
    try:
        options = settings.load('switch')
        options.configure('fdf6617f', 40605, 60)
        options['prudp.ping_timeout'] = 3600   # (the server does not answer the library's pings, and the connection is then closed)
        auth = authentication.AuthenticationInfo()
        auth.token = ''
        auth.ngs_version = 4
        auth.token_type = 2

        # what has been found so far, kept across connections: board -> 'wait', the place, '-' (not on it) or '?'
        state = {b[0]: 'wait' for b in BOARDS}
        first_done = set()      # boards whose top 100 has been looked at
        monotone = {}           # board -> its order by the player's figure holds (checked on the top 100)
        figures = {}

        def snapshot():
            for board, _m, _v, _s in BOARDS:
                result[board] = ('~' if state[board] == 'wait' else state[board], figures.get(board, 0))
            if write is not None:
                write(result, final=all(v != 'wait' for v in state.values()))

        async def run(store):
            param = datastore.GetUsersParam()
            param.pids = [SHARED_ACCOUNT_ID]
            param.option = 0x7FF
            users = (await store.get_users(param)).users
            if len(users) != 1:
                return
            me = users[0]
            for board, _m, _v, spec in BOARDS:
                figures[board] = value_of(me, spec) if spec else 0
            for board in known_zero:   # (the player has none of what the board counts)
                state[board] = '-'
            for board, _m, _v, spec in BOARDS:   # (no figure at all: not on a board of figures)
                if spec and figures[board] == 0:
                    state[board] = '-'
            figures['weekly'] = 0

            async def fetch(method, variant, offset, size):
                with anyio.fail_after(CALL_WAIT):
                    data = await store.client.request(store.PROTOCOL_ID, method, params(method, variant, offset, size))
                st = streams.StreamIn(data, store.settings)
                found = st.list(datastore.UserInfo)
                ranks = st.list(st.u32) if st.size() - st.tell() >= 4 else []
                return found, [ranks[i] if i < len(ranks) else offset + i + 1 for i in range(len(found))]

            def place(found, ranks):
                for u, r in zip(found, ranks):
                    if u.code == me.code:
                        return r
                return None

            # the top 100 of every board
            for board, method, variant, spec in BOARDS:
                if state[board] != 'wait' or board in first_done:
                    continue
                found, ranks = await fetch(method, variant, 0, PAGE)
                first_done.add(board)
                r = place(found, ranks)
                if r is not None:
                    state[board] = str(r)
                elif len(found) < PAGE:
                    state[board] = '-'
                elif spec and board not in NO_ORDER:
                    vals = [value_of(u, spec) for u in found]
                    monotone[board] = all(vals[i] >= vals[i + 1] for i in range(len(vals) - 1)) and vals[0] != vals[-1]
                snapshot()

            # then the rest of each board that the player was not found on
            for board, method, variant, spec in BOARDS:
                if state[board] != 'wait':
                    continue
                fig = figures.get(board, 0)
                start = PAGE
                if spec and board not in NO_ORDER and monotone.get(board) and fig > 0:
                    # the first entry whose figure is not above the player's, found by halving
                    lo, hi = PAGE, DEPTH
                    while lo < hi:
                        mid = (lo + hi) // 2
                        found, ranks = await fetch(method, variant, mid, 1)
                        if not found or value_of(found[0], spec) <= fig:
                            hi = mid
                        else:
                            lo = mid + 1
                    if lo >= DEPTH:
                        state[board] = '-'
                        snapshot()
                        continue
                    start = max(PAGE, lo - 20)   # (a little before: the figure may have moved since the board was made)
                    stop_below = fig
                else:
                    stop_below = None            # (no figure to go by: the whole board)
                offset, answer = start, '-'
                while offset < DEPTH:
                    found, ranks = await fetch(method, variant, offset, min(PAGE, DEPTH - offset))
                    r = place(found, ranks)
                    if r is not None:
                        answer = str(r)
                        break
                    if len(found) < min(PAGE, DEPTH - offset):
                        break
                    if stop_below is not None and value_of(found[-1], spec) < stop_below:
                        break
                    offset += len(found)
                state[board] = answer
                snapshot()

        for attempt in range(SESSIONS):
            try:
                with anyio.fail_after(CALL_WAIT * 40):
                    async with backend.connect(options, domain, 443) as be:
                        async with be.login(str(SHARED_ACCOUNT_ID), auth_info=auth) as client:
                            await run(datastore.DataStoreClientSMM2(client))
            except BaseException as e:   # (never a traceback: it could carry the token)
                if os.environ.get('OCW_DEBUG'):
                    print('session', attempt, leaf(e))
            if all(v != 'wait' for v in state.values()):
                break
        for board in state:
            if state[board] == 'wait':
                state[board] = '?'
        if figures:
            snapshot()
        return result
    finally:
        http.HTTPClient.request = original_request
        prudp.connect_transport_socket = original_transport
        websocket.secrets.token_urlsafe = original_key
        streams.StreamIn.string = original_string


def main():
    ap = argparse.ArgumentParser(description="Find the player's place on OCW's in-game leaderboards")
    ap.add_argument('--config', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--first-clears', type=int)    # how many of each the player has, when the caller knows
    ap.add_argument('--world-records', type=int)
    a = ap.parse_args()
    known_zero = [b for b, n in (('first', a.first_clears), ('record', a.world_records)) if n == 0]
    try:
        domain, token = read_config(a.config)
    except (OSError, ValueError):
        return 1
    import logging
    logging.disable(logging.CRITICAL)
    import anyio

    def write(result, final):
        tmp = a.out + '.tmp'
        with open(tmp, 'w', encoding='ascii') as f:
            f.write('%s %d\n' % (fingerprint(domain, token), int(time.time())))
            for board, _m, _v, _s in BOARDS:
                rank, value = result.get(board, ('?', 0))
                f.write('%s %s %d\n' % (board, '-' if rank == '~' and final else rank, value))
        os.replace(tmp, a.out)

    try:
        result = anyio.run(scan, domain, token, write, known_zero)
    except BaseException:
        return 1
    if not result or all(v[0] == '?' for v in result.values()):
        return 1
    print('ocw_leaderboards: %d boards' % len(result))
    return 0


if __name__ == '__main__':
    sys.exit(main())
