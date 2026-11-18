#!/usr/bin/env python3
"""Tests of the support report's redaction (runtime/support_redact.c, issue #55): builds it with the Build Helper's zig into a small
program, feeds it synthetic secrets, personal paths and ordinary log lines, and checks what comes out.

    python recomp/tools/test_support_report.py        (run build-helper.cmd once first: it fetches zig)
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

RECOMP = Path(__file__).resolve().parents[1]
ZIG = RECOMP.parent / 'dist' / 'smm2-dx_data' / 'work' / 'zig' / 'zig.exe'

HARNESS = r'''
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "support_redact.h"
/* usage: harness <cap> [find=with ...]; the text comes on stdin */
int main(int argc, char **argv) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    size_t cap = (size_t)atol(argv[1]);
    RedactLiteral lits[32];
    int n = 0;
    for (int i = 2; i < argc && n < 32; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) continue;
        *eq = 0;
        lits[n].find = argv[i], lits[n].with = eq + 1, n++;
    }
    static char in[1 << 20];
    size_t len = fread(in, 1, sizeof in, stdin);
    char *out = malloc(cap);
    size_t w = support_redact(in, len, out, cap, lits, n);
    fwrite(out, 1, w, stdout);
    return 0;
}
'''


class Redact(unittest.TestCase):
    exe = None

    @classmethod
    def setUpClass(cls):
        if not ZIG.exists():
            raise unittest.SkipTest(f'{ZIG} is missing: run build-helper.cmd once')
        cls.tmp = tempfile.TemporaryDirectory()
        t = Path(cls.tmp.name)
        (t / 'h.c').write_text(HARNESS)
        cls.exe = t / 'h.exe'
        r = subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-Wall', f'-I{RECOMP / "runtime"}', str(t / 'h.c'),
                            str(RECOMP / 'runtime' / 'support_redact.c'), '-o', str(cls.exe)], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_text(self, text, *lits, cap=1 << 20):
        r = subprocess.run([str(self.exe), str(cap), *lits], input=text.encode(), capture_output=True)
        self.assertEqual(r.returncode, 0)
        return r.stdout.decode()

    def test_ordinary_log_lines_are_unchanged(self):
        for line in ('[vk] gpu 0: NVIDIA GeForce RTX 5080 Laptop GPU (api 1.4.351)\n',
                     '[vk] t=6.3 frame 2054 335.3 fps max 8ms level  (190441 draws, 0 skipped)\n',
                     'loaded main  at 7000000000 size 2e74000 syms 947\n',
                     '[am] IApplicationFunctions cmd 50 (unhandled, ok)\n',
                     'Windows 10.0.26300.0, WebView2 157.0.4341.60, 127.0.0.1 and 0.0.0.0\n',
                     'build id 0123456789abcdef0123456789abcdef01234567 shader 5f3a9b0c1d2e4f60718293a4b5c6d7e8\n',
                     "[loader] sdk: _ZN2nn2os5MutexD2Ev bound to an earlier module's\n",
                     '[ssl] host: handshake done (TLSv1.2, ECDHE-ECDSA-AES256-GCM-SHA384-CHACHA20)\n'):
            self.assertEqual(self.run_text(line), line)

    def test_personal_paths_and_names(self):
        text = r'C:\Users\Alice\Desktop\smm2-dx\dist and c:/users/alice/Desktop and ALICE-PC' + '\n'
        out = self.run_text(text, r'C:\Users\Alice=%USERPROFILE%', 'C:/Users/Alice=%USERPROFILE%', 'ALICE-PC=<computer>', 'Alice=<user>')  # (longer first)
        self.assertNotIn('lice', out.replace('<user>', '').replace('smm2-dx', ''))
        self.assertIn('%USERPROFILE%', out)
        self.assertIn('<computer>', out)

    def test_known_secrets_are_taken_out_by_value(self):
        out = self.run_text('GET https://my.private.example/api?x=1 with Zq7Kp0Lm9Xv3Rt5Yb2Nc8Hd4 ok\n',
                            'my.private.example=<ocw domain>', 'Zq7Kp0Lm9Xv3Rt5Yb2Nc8Hd4=<ocw token>')
        self.assertNotIn('private.example', out)
        self.assertNotIn('Zq7Kp0', out)
        self.assertIn('<ocw domain>', out)
        self.assertIn('<ocw token>', out)

    def test_secret_words_hide_their_value(self):
        for text, leak in (('token=abc123SECRET\n', 'abc123SECRET'), ('Token: s3cr3tvalue\n', 's3cr3tvalue'),
                           ('"token": "hunter2value"\n', 'hunter2value'), ('password = p4ssw0rd!\n', 'p4ssw0rd'),
                           ('Authorization: Bearer eyJhbGciOiJIUzI1NiJ9abc\n', 'eyJhbGci'),
                           ('Authorization: Basic dXNlcjpwYXNz\n', 'dXNlcjpw'), ('api_key=ABCD-EFGH\n', 'ABCD-EFGH'),
                           ('Cookie: session=abc\n', 'session=abc')):
            out = self.run_text(text)
            self.assertNotIn(leak, out, out)
            self.assertIn('<redacted>', out)

    def test_words_that_only_contain_a_secret_word_stay(self):
        text = 'tokens loaded, token_count 5, mytoken ok\n'
        self.assertEqual(self.run_text(text), text)

    def test_emails_addresses_ids(self):
        out = self.run_text('mail jane.doe+x@example.co.uk now; host 192.168.1.20 and 8.8.8.8; maker YNG-5P9-CC8 course 4XK-QJF-9WG\n')
        self.assertNotIn('jane', out)
        self.assertNotIn('example.co', out)
        self.assertNotIn('192.168', out)
        self.assertNotIn('8.8.8.8', out)
        self.assertNotIn('YNG-5P9', out)
        self.assertNotIn('4XK-QJF', out)
        for label in ('<email>', '<ip>', '<id>'):
            self.assertIn(label, out)

    def test_long_token_like_strings(self):
        tok = 'aB3dE5fG7hJ9kL1mN3pQ5rS7tU9vW1xY3zA5'
        out = self.run_text(f'got {tok} done\n')
        self.assertNotIn(tok, out)
        self.assertIn('<token>', out)
        path = '/some/very/long/path/with/many/parts/and/files123/abc456\n'
        self.assertEqual(self.run_text(path), path)

    def test_output_is_cut_at_a_line_and_terminated(self):
        text = ''.join(f'line {i} of the log\n' for i in range(50))
        out = self.run_text(text, cap=100)
        self.assertLess(len(out), 100)
        self.assertTrue(out.endswith('\n'))
        self.assertTrue(text.startswith(out))

    def test_empty_and_no_literals(self):
        self.assertEqual(self.run_text(''), '')


if __name__ == '__main__':
    sys.exit(unittest.main(verbosity=2).result.wasSuccessful() is False)
