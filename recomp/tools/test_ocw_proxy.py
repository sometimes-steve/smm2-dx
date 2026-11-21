#!/usr/bin/env python3
"""Tests of the early course downloads (runtime/net/ocwproxy.c and nextap.c, issue #3): builds them with the Build Helper's zig into small
programs. The proxy is run in front of a fake OpenCourse World that serves /level_data/<id> slowly, and the tests check what the game would
get and what the server would see; the stream reader is fed synthetic WebSocket / PRUDP frames the way the server sends them.

    python recomp/tools/test_ocw_proxy.py        (run build-helper.cmd once first: it fetches zig)
"""
import os
import warnings
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

RECOMP = Path(__file__).resolve().parents[1]
ZIG = RECOMP.parent / 'dist' / 'smm2-dx_data' / 'work' / 'zig' / 'zig.exe'
BODY_LEN = 377073 - 220  # (the course file is 377,073 bytes with its head: here the head is about 220)

PROXY_HARNESS = r'''
#define OCWPROXY_TEST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ocwproxy.h"
/* usage: harness <upstream port>; prints the listen port, then reads "prefetch <id>" lines from stdin until "quit" */
int main(int argc, char **argv) {
    uint16_t port = ocwproxy_test_start(0x7f000001, (uint16_t)atoi(argv[1]), "ocw.test");
    printf("%u\n", port);
    fflush(stdout);
    char line[128];
    while (fgets(line, sizeof line, stdin)) {
        unsigned id;
        if (sscanf(line, "prefetch %u", &id) == 1) ocwproxy_prefetch(id);
        else if (!strncmp(line, "quit", 4)) break;
    }
    return 0;
}
'''

TAP_HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nextap.h"
/* reads "feed <hex>" lines: each chunk goes to the tap; every RMC response it reports is printed as "cb <proto> <method> <call> <params hex>" */
static void cb(void *u, unsigned proto, unsigned method, uint32_t call, const uint8_t *p, size_t n) {
    (void)u;
    printf("cb %u %u %u ", proto, method, call);
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}
int main(void) {
    static char line[1 << 20];
    NexTap t;
    memset(&t, 0, sizeof t);
    while (fgets(line, sizeof line, stdin)) {
        if (strncmp(line, "feed ", 5)) continue;
        size_t n = strlen(line + 5);
        while (n && (line[5 + n - 1] == '\n' || line[5 + n - 1] == '\r')) n--;
        uint8_t *b = malloc(n / 2 + 1);
        for (size_t i = 0; i + 1 < n; i += 2) { unsigned v; sscanf(line + 5 + i, "%2x", &v); b[i / 2] = (uint8_t)v; }
        nextap_feed(&t, b, n / 2, cb, NULL);
        printf("state %d\n", t.state);
        free(b);
    }
    nextap_free(&t);
    return 0;
}
'''


def build(tmp, name, src, extra):
    c = tmp / f'{name}.c'
    c.write_text(src)
    exe = tmp / f'{name}.exe'
    r = subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-Wall', '-Wno-unused-function', '-DOCWPROXY_TEST', f'-I{RECOMP / "runtime" / "net"}', str(c)] +
                       [str(RECOMP / 'runtime' / 'net' / f) for f in extra] + ['-lws2_32', '-o', str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return exe


class FakeServer:
    """The OpenCourse World end: /level_data/<id> comes slowly in pieces; the requests it gets are kept."""

    def __init__(self):
        self.requests = []          # (monotonic time, request line)
        self.mode = {}              # id -> 'ok' (default), '404', 'chunked', 'cut' (the connection ends half-way)
        self.delay = 0.0            # seconds before the body, and the time it takes to send it
        self.needs_game_header = False  # answer chunked unless the request has the game's own header
        self.lock = threading.Lock()
        self.sock = socket.socket()
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]
        threading.Thread(target=self.accept, daemon=True).start()

    def accept(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self.handle, args=(c,), daemon=True).start()

    @staticmethod
    def body(i):
        return bytes((i * 31 + k) & 0xff for k in range(BODY_LEN))

    def handle(self, c):
        try:
            buf = b''
            while True:
                while b'\r\n\r\n' not in buf:
                    d = c.recv(65536)
                    if not d:
                        return
                    buf += d
                head, buf = buf.split(b'\r\n\r\n', 1)
                line = head.split(b'\r\n')[0].decode()
                with self.lock:
                    self.requests.append((time.monotonic(), line))
                parts = line.split(' ')
                length = 0
                hdrs = {}
                for h in head.split(b'\r\n')[1:]:
                    k, _, v = h.partition(b':')
                    hdrs[k.strip().lower()] = v.strip()
                    if h.lower().startswith(b'content-length:'):
                        length = int(h.split(b':')[1])
                while len(buf) < length:
                    buf += c.recv(65536)
                payload, buf = buf[:length], buf[length:]
                if parts[1].startswith('/level_data/'):
                    i = int(parts[1].rsplit('/', 1)[1])
                    mode = self.mode.get(i, 'ok')
                    if self.needs_game_header and b'x-game' not in hdrs:
                        mode = 'forbidden'  # (a request that does not look like the game's is turned away)
                    if mode == '404':
                        c.sendall(b'HTTP/1.1 404 Not Found\r\nContent-Length: 4\r\n\r\nnone')
                        continue
                    if mode == 'forbidden':
                        c.sendall(b'HTTP/1.1 403 Forbidden\r\nContent-Length: 4\r\n\r\nnope')
                        continue
                    if mode == 'badchunks':
                        c.sendall(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\noops\r\n')
                        continue
                    if mode in ('chunked', 'chunked-slow'):  # (as the real server, behind Caddy, answers every course)
                        data = self.body(i)
                        c.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nVia: 1.1 Caddy\r\nTransfer-Encoding: chunked\r\n\r\n')
                        for k in range(0, len(data), 20000):
                            piece = data[k:k + 20000]
                            c.sendall(b'%x\r\n' % len(piece) + piece + b'\r\n')
                            if mode == 'chunked-slow':
                                time.sleep(self.delay / 20)
                        c.sendall(b'0\r\n\r\n')
                        continue
                    time.sleep(self.delay / 2)
                    data = self.body(i)
                    c.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n' % len(data))
                    pieces = 10
                    step = len(data) // pieces + 1
                    for k in range(0, len(data), step):
                        if mode == 'cut' and k >= len(data) // 2:
                            c.close()
                            return
                        c.sendall(data[k:k + step])
                        time.sleep(self.delay / 2 / pieces)
                else:
                    out = b'echo:' + parts[0].encode() + b':' + payload
                    c.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n' % len(out) + out)
        except OSError:
            pass
        finally:
            c.close()

    def seen(self, path):
        with self.lock:
            return [t for t, l in self.requests if l.split(' ')[1] == path]


def http_get(port, path, timeout=20, conn=None, extra=''):
    """One request on a connection; returns (status line, body, the connection)."""
    c = conn or socket.create_connection(('127.0.0.1', port), timeout=timeout)
    c.sendall(f'GET {path} HTTP/1.1\r\nHost: ocw.test\r\nAccept: */*\r\n{extra}\r\n'.encode())
    buf = b''
    while b'\r\n\r\n' not in buf:
        d = c.recv(65536)
        if not d:
            break
        buf += d
    head, _, rest = buf.partition(b'\r\n\r\n')
    n = 0
    for h in head.split(b'\r\n')[1:]:
        if h.lower().startswith(b'content-length:'):
            n = int(h.split(b':')[1])
    while len(rest) < n:
        d = c.recv(65536)
        if not d:
            break
        rest += d
    return head.split(b'\r\n')[0].decode(), rest, c


def http_get_raw_chunked(port, path, timeout=20):
    """One request; the response exactly as it came, up to the end of a chunked body."""
    c = socket.create_connection(('127.0.0.1', port), timeout=timeout)
    c.sendall(f'GET {path} HTTP/1.1\r\nHost: ocw.test\r\nAccept: */*\r\n\r\n'.encode())
    buf = b''
    while not buf.endswith(b'0\r\n\r\n'):
        d = c.recv(65536)
        if not d:
            break
        buf += d
    c.close()
    return buf


def dechunk(b):
    out = b''
    while b:
        line, _, b = b.partition(b'\r\n')
        n = int(line, 16)
        if n == 0:
            return out
        out += b[:n]
        b = b[n + 2:]
    raise AssertionError('no last chunk')


class Proxy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not ZIG.exists():
            raise unittest.SkipTest(f'{ZIG} is missing: run build-helper.cmd once')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = build(Path(cls.tmp.name), 'proxy', PROXY_HARNESS, ['ocwproxy.c'])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.server = FakeServer()
        self.p = subprocess.Popen([str(self.exe), str(self.server.port)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.port = int(self.p.stdout.readline())
        self.assertGreater(self.port, 0)

    def tearDown(self):
        try:
            self.p.stdin.write('quit\n')
            self.p.stdin.flush()
        except OSError:
            pass
        self.p.wait(timeout=10)
        self.server.sock.close()

    def prefetch(self, i):
        self.p.stdin.write(f'prefetch {i}\n')
        self.p.stdin.flush()

    def wait_seen(self, path, n=1, timeout=5):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if len(self.server.seen(path)) >= n:
                return
            time.sleep(0.02)
        self.fail(f'the server never saw {path}')

    def test_a_request_with_no_early_download_goes_on_untouched(self):
        st, body, _ = http_get(self.port, '/level_data/77')
        self.assertEqual(st, 'HTTP/1.1 200 OK')
        self.assertEqual(body, FakeServer.body(77))
        self.assertEqual(len(self.server.seen('/level_data/77')), 1)

    def test_other_requests_pass_through_with_their_body(self):
        c = socket.create_connection(('127.0.0.1', self.port), timeout=10)
        c.sendall(b'POST /post_replay/5 HTTP/1.1\r\nHost: ocw.test\r\nContent-Length: 11\r\n\r\nhello world')
        got = b''
        while b'echo:POST:hello world' not in got:
            d = c.recv(4096)
            self.assertTrue(d)
            got += d

    def test_an_early_download_is_what_the_game_gets_and_the_server_is_asked_once(self):
        self.prefetch(123)
        self.wait_seen('/level_data/123')
        st, body, _ = http_get(self.port, '/level_data/123')
        self.assertEqual(st, 'HTTP/1.1 200 OK')
        self.assertEqual(body, FakeServer.body(123))
        time.sleep(0.2)
        self.assertEqual(len(self.server.seen('/level_data/123')), 1, 'the game\'s own request must not reach the server')

    def test_asking_while_it_is_still_arriving_waits_for_the_same_download(self):
        self.server.delay = 1.6
        self.prefetch(5)
        self.wait_seen('/level_data/5')
        t0 = time.monotonic()
        time.sleep(0.6)  # (the game asks a little after)
        st, body, _ = http_get(self.port, '/level_data/5')
        took = time.monotonic() - t0
        self.assertEqual(body, FakeServer.body(5))
        self.assertLess(took, 2.4, 'it should end when the early download does, not a whole download later')
        self.assertEqual(len(self.server.seen('/level_data/5')), 1)

    def test_it_starts_to_arrive_before_the_early_download_is_done(self):
        self.server.delay = 2.0
        self.prefetch(6)
        self.wait_seen('/level_data/6')
        c = socket.create_connection(('127.0.0.1', self.port), timeout=10)
        t0 = time.monotonic()
        c.sendall(b'GET /level_data/6 HTTP/1.1\r\nHost: ocw.test\r\n\r\n')
        first = c.recv(4096)
        self.assertTrue(first.startswith(b'HTTP/1.1 200'))
        self.assertLess(time.monotonic() - t0, 1.6, 'the head should come as soon as the server sends it')

    def test_a_failed_early_download_leaves_the_game_to_ask_the_server(self):
        self.server.mode[9] = '404'
        self.prefetch(9)
        self.wait_seen('/level_data/9')
        time.sleep(0.3)
        st, body, _ = http_get(self.port, '/level_data/9')
        self.assertEqual(st, 'HTTP/1.1 404 Not Found')
        self.assertEqual(len(self.server.seen('/level_data/9')), 2, 'the game\'s own request goes to the server')

    def test_a_chunked_answer_is_followed_and_sent_on_as_it_came(self):
        self.server.mode[10] = 'chunked'
        self.prefetch(10)
        self.wait_seen('/level_data/10')
        time.sleep(0.6)
        raw = http_get_raw_chunked(self.port, '/level_data/10')
        head, _, rest = raw.partition(b'\r\n\r\n')
        self.assertIn(b'Transfer-Encoding: chunked', head)
        self.assertEqual(dechunk(rest), FakeServer.body(10))
        time.sleep(0.2)
        self.assertEqual(len(self.server.seen('/level_data/10')), 1, "the game's own request must not reach the server")

    def test_a_chunked_answer_still_arriving_is_passed_on_while_it_comes(self):
        self.server.mode[15] = 'chunked-slow'
        self.server.delay = 2.0
        self.prefetch(15)
        self.wait_seen('/level_data/15')
        time.sleep(0.5)
        t0 = time.monotonic()
        raw = http_get_raw_chunked(self.port, '/level_data/15')
        self.assertEqual(dechunk(raw.partition(b'\r\n\r\n')[2]), FakeServer.body(15))
        self.assertLess(time.monotonic() - t0, 2.0, 'it ends with the early download, not a whole download after the game asked')
        self.assertEqual(len(self.server.seen('/level_data/15')), 1)

    def test_bad_chunk_framing_leaves_the_game_to_ask_the_server(self):
        self.server.mode[16] = 'badchunks'
        self.prefetch(16)
        self.wait_seen('/level_data/16')
        time.sleep(0.4)
        self.server.mode[16] = 'ok'
        st, body, _ = http_get(self.port, '/level_data/16')
        self.assertEqual(body, FakeServer.body(16))
        self.assertEqual(len(self.server.seen('/level_data/16')), 2)

    def test_a_download_that_breaks_off_early_is_asked_again_by_the_game(self):
        self.server.mode[11] = 'cut'
        self.prefetch(11)
        self.wait_seen('/level_data/11')
        time.sleep(0.6)
        self.server.mode[11] = 'ok'
        st, body, _ = http_get(self.port, '/level_data/11')
        self.assertEqual(body, FakeServer.body(11))
        self.assertEqual(len(self.server.seen('/level_data/11')), 2)

    def test_early_downloads_repeat_the_headers_of_the_games_own_request(self):
        self.server.needs_game_header = True
        self.prefetch(40)  # (before the game has asked for anything: a plain request, which this server turns away)
        self.wait_seen('/level_data/40')
        time.sleep(0.3)
        st, body, _ = http_get(self.port, '/level_data/40', extra='X-Game: 1\r\n')  # (the game's own: goes through, and is learned)
        self.assertEqual(body, FakeServer.body(40))
        self.prefetch(41)
        self.wait_seen('/level_data/41')
        time.sleep(0.5)
        st, body, _ = http_get(self.port, '/level_data/41', extra='X-Game: 1\r\n')
        self.assertEqual(body, FakeServer.body(41))
        self.assertEqual(len(self.server.seen('/level_data/41')), 1, "the early download had the game's headers: the game's own request is not needed")

    def test_the_same_id_is_fetched_once(self):
        self.server.delay = 0.4
        for _ in range(3):
            self.prefetch(12)
        self.wait_seen('/level_data/12')
        time.sleep(0.8)
        self.assertEqual(len(self.server.seen('/level_data/12')), 1)

    def test_a_game_connection_can_ask_again_after_an_early_download(self):
        self.prefetch(13)
        self.wait_seen('/level_data/13')
        st, body, conn = http_get(self.port, '/level_data/13')
        self.assertEqual(body, FakeServer.body(13))
        st, body, _ = http_get(self.port, '/level_data/14', conn=conn)  # (not early: the rest of the connection goes to the server)
        self.assertEqual(st, 'HTTP/1.1 200 OK')
        self.assertEqual(body, FakeServer.body(14))

    def test_several_early_downloads_at_once(self):
        for i in (21, 22, 23):
            self.prefetch(i)
        for i in (21, 22, 23):
            self.wait_seen(f'/level_data/{i}')
        for i in (23, 21, 22):
            st, body, _ = http_get(self.port, f'/level_data/{i}')
            self.assertEqual(body, FakeServer.body(i))
        for i in (21, 22, 23):
            self.assertEqual(len(self.server.seen(f'/level_data/{i}')), 1)

    def test_a_closed_server_does_not_hang_the_game(self):
        self.server.sock.close()
        t0 = time.monotonic()
        try:
            c = socket.create_connection(('127.0.0.1', self.port), timeout=12)
            c.sendall(b'GET /level_data/31 HTTP/1.1\r\nHost: ocw.test\r\n\r\n')
            d = c.recv(100)
            self.assertEqual(d, b'')
        except OSError:
            pass
        self.assertLess(time.monotonic() - t0, 11)


def ws(payload, op=0x82):
    n = len(payload)
    if n < 126:
        h = bytes([op, n])
    else:
        h = bytes([op, 126]) + struct.pack('>H', n)
    return h + payload


def prudp(payload, frag=0, ack=False, typ=2, opt=b''):
    """A PRUDP Lite packet: 0x80, option length, payload size, ..., fragment id at 7, type and flags at 8."""
    h = bytearray(12)
    h[0] = 0x80
    h[1] = len(opt)
    h[2:4] = struct.pack('<H', len(payload))
    h[7] = frag
    h[8] = typ | (0x10 if ack else 0)
    return bytes(h) + opt + payload


def rmc_response(method, params, call=7, proto=0x73, ok=1):
    body = bytes([proto, ok]) + struct.pack('<II', call, method | 0x8000) + params
    return struct.pack('<I', len(body)) + body


UPGRADE = b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n'


class Tap(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not ZIG.exists():
            raise unittest.SkipTest(f'{ZIG} is missing: run build-helper.cmd once')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = build(Path(cls.tmp.name), 'tap', TAP_HARNESS, ['nextap.c'])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_chunks(self, chunks):
        inp = ''.join(f'feed {c.hex()}\n' for c in chunks)
        r = subprocess.run([str(self.exe)], input=inp, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        cbs = [l.split() for l in r.stdout.splitlines() if l.startswith('cb ')]
        state = [int(l.split()[1]) for l in r.stdout.splitlines() if l.startswith('state ')][-1]
        return [(int(c[1]), int(c[2]), int(c[3]), bytes.fromhex(c[4]) if len(c) > 4 else b'') for c in cbs], state

    def test_a_response_in_one_packet(self):
        params = bytes(9) + struct.pack('<Q', 1000012889) + b'rest'
        out, st = self.run_chunks([UPGRADE + ws(prudp(rmc_response(79, params, call=25)))])
        self.assertEqual(out, [(0x73, 79, 25, params)])
        self.assertEqual(st, 1)

    def test_the_stream_in_single_bytes(self):
        params = b'x' * 40
        data = UPGRADE + ws(prudp(rmc_response(79, params)))
        out, _ = self.run_chunks([bytes([b]) for b in data])
        self.assertEqual(out, [(0x73, 79, 7, params)])

    def test_a_response_split_into_fragments(self):
        params = bytes(range(200)) * 3
        msg = rmc_response(48, params)
        a, b, c = msg[:250], msg[250:500], msg[500:]
        data = UPGRADE + ws(prudp(a, frag=1)) + ws(prudp(b, frag=2)) + ws(prudp(c, frag=0))
        out, _ = self.run_chunks([data[:100], data[100:400], data[400:]])
        self.assertEqual(out, [(0x73, 48, 7, params)])

    def test_several_responses_and_noise_between(self):
        data = UPGRADE
        data += ws(prudp(b'\x00' * 4, ack=True, typ=2))                       # an acknowledgement: no payload
        data += ws(b'', op=0x89)                                              # a ping
        data += ws(prudp(rmc_response(79, b'one', call=1)))
        data += ws(prudp(rmc_response(25, b'two', call=2)))
        data += ws(prudp(rmc_response(48, b'', call=3, ok=0)))                # an error answer: not reported
        out, _ = self.run_chunks([data])
        self.assertEqual(out, [(0x73, 79, 1, b'one'), (0x73, 25, 2, b'two')])

    def test_a_request_from_the_server_side_is_not_a_response(self):
        body = bytes([0x73 | 0x80]) + struct.pack('<II', 1, 79) + b'zz'
        out, _ = self.run_chunks([UPGRADE + ws(prudp(struct.pack('<I', len(body)) + body))])
        self.assertEqual(out, [])

    def test_a_stream_that_is_not_a_websocket_is_left_alone(self):
        out, st = self.run_chunks([b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n', ws(prudp(rmc_response(79, b'x')))])
        self.assertEqual(out, [])
        self.assertEqual(st, -1)

    def test_options_in_the_packet_header(self):
        out, _ = self.run_chunks([UPGRADE + ws(prudp(rmc_response(79, b'abc'), opt=b'\x01\x02\x03\x04'))])
        self.assertEqual(out, [(0x73, 79, 7, b'abc')])

    def test_a_long_frame_header(self):
        params = b'q' * 400
        out, _ = self.run_chunks([UPGRADE + ws(prudp(rmc_response(79, params)))])
        self.assertEqual(out, [(0x73, 79, 7, params)])


if __name__ == '__main__':
    warnings.simplefilter('ignore', ResourceWarning)  # (the tests leave their client sockets to be closed at exit)
    sys.exit(unittest.main(verbosity=2, warnings='ignore').result.wasSuccessful() is False)
