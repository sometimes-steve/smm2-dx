#!/usr/bin/env python3
"""Tests of the text prompts' UTF-8 / UTF-16 handling (runtime/text_utf.c, issue #53): builds it with the Build Helper's zig into a small
program and checks what the game receives and what the prompt shows against Python's own codecs: Japanese, Chinese, Korean, emoji,
broken input, and the limits (characters, never bytes; never inside a character or a surrogate pair).

    python recomp/tools/test_text_utf.py        (run build-helper.cmd once first: it fetches zig)
"""
import random
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
#include "text_utf.h"
/* usage: harness <mode> <n>; bytes on stdin, bytes on stdout
 *   16to8 <cap>   stdin UTF-16LE (NUL-terminated or not)  -> UTF-8, into a cap-byte buffer
 *   8to16 <cap>   stdin UTF-8                             -> UTF-16LE, into a cap-unit buffer (the NUL is written too)
 *   clip <max>    stdin UTF-8                             -> UTF-8 cut after max characters, then one byte: the count
 *   chars         stdin UTF-8                             -> the character count, as one byte */
int main(int argc, char **argv) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    static char in[1 << 16];
    size_t len = fread(in, 1, sizeof in - 4, stdin);
    in[len] = 0;
    const char *mode = argv[1];
    long n = argc > 2 ? atol(argv[2]) : 0;
    if (!strcmp(mode, "16to8")) {
        char *out = malloc((size_t)n + 16);
        memset(out, 0x55, (size_t)n + 16);  // (a write past cap shows)
        text_utf16_to_utf8((const uint16_t *)in, (uint32_t)(len / 2), out, (size_t)n);
        size_t w = n ? strlen(out) : 0;
        for (size_t i = (size_t)n; i < (size_t)n + 16; i++) if ((unsigned char)out[i] != 0x55) return 3;
        fwrite(out, 1, w, stdout);
    } else if (!strcmp(mode, "8to16")) {
        uint16_t *out = malloc(((size_t)n + 8) * 2);
        for (long i = 0; i < n + 8; i++) out[i] = 0x5555;
        size_t w = text_utf8_to_utf16(in, out, (size_t)n);
        for (long i = n; i < n + 8; i++) if (out[i] != 0x5555) return 3;
        if (n && out[w] != 0) return 4;  // (terminated)
        fwrite(out, 2, w, stdout);
    } else if (!strcmp(mode, "clip")) {
        int k = text_utf8_clip(in, (int)n);
        fwrite(in, 1, strlen(in), stdout);
        fputc(k, stdout);
    } else if (!strcmp(mode, "chars")) {
        fputc(text_utf8_chars(in), stdout);
    }
    return 0;
}
'''

SCRIPTS = {
    'japanese': 'こんにちは、マリオ！',
    'hiragana + kanji': '日本語の入力テスト',
    'simplified': '你好，马力欧',
    'traditional': '你好，瑪利歐製作者',
    'korean': '안녕하세요 마리오',
    'emoji (astral)': '🍄⭐🔥',
    'mixed': 'Mario マリオ 馬 마 🍄!',
    'latin accents': 'Café déjà vu ñ',
    'ascii': 'Hello, world!',
}


class TextUtf(unittest.TestCase):
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
                            str(RECOMP / 'runtime' / 'text_utf.c'), '-o', str(cls.exe)], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_h(self, data, *args):
        r = subprocess.run([str(self.exe), *map(str, args)], input=data, capture_output=True)
        self.assertEqual(r.returncode, 0, f'harness exit {r.returncode} for {args} (3: wrote past the buffer, 4: not terminated)')
        return r.stdout

    def to8(self, units_bytes, cap=400):
        return self.run_h(units_bytes, '16to8', cap)

    def to16(self, text_bytes, cap=400):
        return self.run_h(text_bytes, '8to16', cap)

    def clip(self, text, max_chars):
        out = self.run_h(text.encode('utf-8'), 'clip', max_chars)
        return out[:-1].decode('utf-8'), out[-1]

    # --- what the game hands us (UTF-16) and what we hand back ---

    def test_scripts_round_trip_both_ways(self):
        for name, s in SCRIPTS.items():
            with self.subTest(name):
                self.assertEqual(self.to8(s.encode('utf-16-le')), s.encode('utf-8'))
                self.assertEqual(self.to16(s.encode('utf-8')), s.encode('utf-16-le'))

    def test_nul_ends_the_game_text(self):
        self.assertEqual(self.to8('ab\0cd'.encode('utf-16-le')), b'ab')

    def test_empty(self):
        self.assertEqual(self.to8(b''), b'')
        self.assertEqual(self.to16(b''), b'')

    def test_random_text_matches_python(self):
        rnd = random.Random(53)
        pools = [range(0x20, 0x7f), range(0x3041, 0x3097), range(0x4e00, 0x4e80), range(0xac00, 0xac80), range(0x1f300, 0x1f340), range(0xa0, 0x180)]
        for _ in range(300):
            s = ''.join(chr(rnd.choice(rnd.choice(pools))) for _ in range(rnd.randint(1, 60)))
            self.assertEqual(self.to8(s.encode('utf-16-le')), s.encode('utf-8'), s)
            self.assertEqual(self.to16(s.encode('utf-8')), s.encode('utf-16-le'), s)

    # --- the buffer the game gave us is never overrun and never ends inside a character ---

    def test_utf16_output_never_ends_inside_a_surrogate_pair(self):
        for s in ('🍄' * 10, 'a🍄' * 10, 'あ🍄' * 7):
            for cap in range(1, 25):  # (units, the NUL included)
                with self.subTest(s=s, cap=cap):
                    out = self.to16(s.encode('utf-8'), cap)
                    self.assertLessEqual(len(out) // 2, max(cap - 1, 0))
                    out.decode('utf-16-le')  # (strict: a cut pair raises)

    def test_utf8_output_never_ends_inside_a_character(self):
        for name, s in SCRIPTS.items():
            for cap in range(1, 40):
                with self.subTest(name, cap=cap):
                    out = self.to8(s.encode('utf-16-le'), cap)
                    self.assertLess(len(out), cap)
                    self.assertTrue(s.encode('utf-8').startswith(out))
                    out.decode('utf-8')

    def test_zero_capacity_is_harmless(self):
        self.assertEqual(self.to16('あ'.encode('utf-8'), 0), b'')
        self.assertEqual(self.run_h('あ'.encode('utf-16-le'), '16to8', 0), b'')

    # --- broken input: a stable stand-in, no crash, no junk characters (the Save As course-name junk of #44's era) ---

    def test_lone_surrogates_become_the_replacement_character(self):
        for units in ([0xd83c], [0xdf44], [0x61, 0xd83c, 0x62], [0xdf44, 0xd83c]):
            with self.subTest(units=units):
                raw = b''.join(u.to_bytes(2, 'little') for u in units)
                out = self.to8(raw)
                text = out.decode('utf-8')  # (strict)
                self.assertIn('�', text)
                self.assertEqual(text.replace('�', '').encode('utf-16-le', 'surrogatepass'),
                                 b''.join(u.to_bytes(2, 'little') for u in units if not 0xd800 <= u < 0xe000))

    def test_invalid_utf8_becomes_the_replacement_character(self):
        for bad in (b'\x80', b'a\xc3', b'\xe3\x81', b'\xf0\x9f\x8d', b'\xff\xfe', b'\xed\xa0\x80', b'\xf4\x90\x80\x80', b'ok\xe3\x81\x82\x80!'):
            with self.subTest(bad=bad):
                out = self.to16(bad)
                text = out.decode('utf-16-le')  # (strict: no lone surrogates out)
                self.assertIn('�', text)
                self.assertEqual(len(out) % 2, 0)

    # --- limits: characters, not bytes and not UTF-16 units ---

    def test_clip_counts_characters(self):
        for name, s in SCRIPTS.items():
            for limit in (0, 1, 3, 8, 100):
                with self.subTest(name, limit=limit):
                    out, kept = self.clip(s, limit)
                    self.assertEqual(out, s[:limit])
                    self.assertEqual(kept, min(limit, len(s)))

    def test_clip_never_splits_a_character(self):
        s = 'マ' * 5 + '🍄' * 5
        for limit in range(0, 12):
            out, kept = self.clip(s, limit)
            self.assertEqual(len(out), kept)
            out.encode('utf-8')

    def test_character_count(self):
        for name, s in SCRIPTS.items():
            with self.subTest(name):
                self.assertEqual(self.run_h(s.encode('utf-8'), 'chars')[0], len(s))

    def test_the_prompt_limit_holds_after_the_trip_to_the_game(self):
        # a 32-character name typed in Japanese is 96 bytes of UTF-8 but 32 units of UTF-16: the game must get all 32, and no 33rd
        s = 'あ' * 40
        out, kept = self.clip(s, 32)
        self.assertEqual(kept, 32)
        self.assertEqual(len(self.to16(out.encode('utf-8'))) // 2, 32)
        # 🍄 is two UTF-16 units: the game's count (characters) and ours agree on 16 of them, it is the buffer that must not cut one
        out, kept = self.clip('🍄' * 40, 16)
        self.assertEqual(len(self.to16(out.encode('utf-8'), 33)) // 2, 32)


if __name__ == '__main__':
    sys.exit(unittest.main(verbosity=2).result.wasSuccessful() is False)
