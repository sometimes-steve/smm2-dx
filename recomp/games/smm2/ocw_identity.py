"""Finds out whose OpenCourse World account the config belongs to, so the game knows its player before it first asks for its Mii.

OCW knows a player by the token in the handshake and gives every player the same console account id (0xCAFE); asking it for
that user returns the player's own profile, maker code included. This logs in the way the game does (the token rides in the
WebSocket handshake, as games/smm2/mods/ocw.c does), asks for that one record, and writes the maker code to ocw-maker.txt in
the data folder, in the format ocw.c reads: "<fingerprint> <AAA-BBB-CCC>". The fingerprint names the account (domain and token)
without keeping the token, exactly as ocw.c computes it, so a cache of another account is never used.

    ocw_identity.py --config <ocw-config.json> --out <data folder>/ocw-maker.txt

Nothing here is needed to play: any failure is silent (exit 1) and the game then learns the code from its own first profile
request, as before. The token is only ever sent to the domain in the config, and never printed or logged.
"""
import argparse
import json
import os
import re
import sys

SHARED_ACCOUNT_ID = 0xCAFE   # runtime/net/netcfg.c OCW_NSA_ID
TOKEN_LEN = 22
DOMAIN_MAX = 29
MASK = (1 << 64) - 1
PRIME = 0x100000001B3


def fingerprint(domain, token):
    """64-bit FNV-1a of the domain and the token (ocw.c: self_fingerprint)."""
    h = 0xCBF29CE484222325
    for b in domain.encode():
        h = ((h ^ b) * PRIME) & MASK
    h = (h * PRIME) & MASK
    for b in token.encode():
        h = ((h ^ b) * PRIME) & MASK
    return '%016x' % h


def read_config(path):
    with open(path, 'rb') as f:
        raw = f.read(4096)
    c = json.loads(raw)
    domain, token = c.get('domain'), c.get('token')
    if not isinstance(domain, str) or not isinstance(token, str):
        raise ValueError('no domain or token')
    if not domain or len(domain) > DOMAIN_MAX or len(token) != TOKEN_LEN or any(ord(ch) <= 32 or ord(ch) == 127 for ch in token):
        raise ValueError('domain or token not usable')
    return domain, token


def cached(out, fp):
    try:
        with open(out, 'r', encoding='ascii') as f:
            parts = f.readline().split()
        return len(parts) == 2 and parts[0] == fp and re.fullmatch(r'[A-Z0-9]{3}-[A-Z0-9]{3}-[A-Z0-9]{3}', parts[1]) is not None
    except OSError:
        return False


async def own_maker_code(domain, token):
    import contextlib
    import anyio
    from anynet import http, websocket
    from nintendo.nex import authentication, backend, datastore_smm2 as datastore, prudp, settings

    original_request = http.HTTPClient.request
    original_transport = prudp.connect_transport_socket
    original_key = websocket.secrets.token_urlsafe

    async def request(client, req, **kwargs):
        if req.headers.get('Upgrade', '').lower() == 'websocket':
            req.headers.pop('Sec-WebSocket-Protocol', None)
            req.headers['NEX'] = token
            req.headers['Mod-Version'] = '0.2'
            req.headers['Game-Version'] = '3.0.3'
        return await original_request(client, req, **kwargs)

    def transport(options, host, port, context):
        # the auth and the secure channel both stay on the one host the config names
        return websocket.connect('wss://%s:443/' % domain, context, protocols=['NEX'], disconnect_timeout=0)

    http.HTTPClient.request = request
    prudp.connect_transport_socket = transport
    # OCW's console mod sends this literal key, and the server routes that handshake differently from a standard one
    websocket.secrets.token_urlsafe = lambda *a, **k: 'NEX'
    try:
        options = settings.load('switch')
        options.configure('fdf6617f', 40605, 60)
        auth = authentication.AuthenticationInfo()
        auth.token = ''
        auth.ngs_version = 4
        auth.token_type = 2
        with anyio.fail_after(20):
            async with backend.connect(options, domain, 443) as be:
                async with be.login(str(SHARED_ACCOUNT_ID), auth_info=auth) as client:
                    store = datastore.DataStoreClientSMM2(client)
                    param = datastore.GetUsersParam()
                    param.pids = [SHARED_ACCOUNT_ID]
                    param.option = 0x7FF
                    users = (await store.get_users(param)).users
        if len(users) != 1:
            return None
        code = users[0].code
        return code if isinstance(code, str) and re.fullmatch(r'[A-Z0-9]{9}', code) else None
    finally:
        http.HTTPClient.request = original_request
        prudp.connect_transport_socket = original_transport
        websocket.secrets.token_urlsafe = original_key


def main():
    ap = argparse.ArgumentParser(description="Learn the player's own OCW maker code from the config's token")
    ap.add_argument('--config', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    try:
        domain, token = read_config(a.config)
    except (OSError, ValueError):
        return 1
    fp = fingerprint(domain, token)
    if cached(a.out, fp):
        return 0   # (the common case: nothing to do, no network)
    import logging
    logging.disable(logging.CRITICAL)
    import anyio
    try:
        code = anyio.run(own_maker_code, domain, token)
    except BaseException:   # (never a traceback: it could carry the token)
        return 1
    if not code:
        return 1
    maker = '%s-%s-%s' % (code[0:3], code[3:6], code[6:9])
    tmp = a.out + '.tmp'
    with open(tmp, 'w', encoding='ascii') as f:
        f.write('%s %s\n' % (fp, maker))
    os.replace(tmp, a.out)
    print('ocw_identity: the player is %s' % maker)
    return 0


if __name__ == '__main__':
    sys.exit(main())
