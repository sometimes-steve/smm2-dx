#!/usr/bin/env python3
"""Test of the Endless skip counter (issue #32) in the running game, through the autoplay driver (no desktop input, no focus: see autoplay.py).
It starts the game hidden, opens the Endless Challenge (Easy), plays into a course (the log says an Endless course is played, which the
Show Endless skips overlay depends on), taps Minus (no skip), skips twice by holding it, and checks the log's `[mod] endless skip` lines and
endless-skips.txt. It uses the player's own Endless run (New Game if there is none to continue), so it can change that run: nothing here loses a
life or a clear on purpose (a course the player dies in meanwhile is the course's). The game runs on a copy of the settings file.

    python recomp/tools/test_endless_skips.py        (needs the game set up and signed in to OpenCourse World; takes about two minutes)
"""
import re
import shutil
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import autoplay  # noqa: E402

COUNTS = autoplay.DIST / 'smm2-dx_data' / 'endless-skips.txt'


def total():
    m = re.search(r'total_skips = (\d+)', COUNTS.read_text()) if COUNTS.exists() else None
    return int(m.group(1)) if m else 0


class EndlessSkips(unittest.TestCase):
    def test_skips_are_counted(self):
        g = autoplay.Game()
        before = total()
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        ini = tmp / 'smm2.ini'  # (so the test never writes the player's own settings)
        src = autoplay.DIST / 'smm2-dx_data' / 'smm2.ini'
        ini.write_text(src.read_text() if src.exists() else '')
        g.launch(hidden=True, to_course_world=True, replace=True, extra_env={'LARECOMP_SETTINGS': str(ini)})
        try:
            g.do('right down a wait:4 a wait:12 a')  # Endless Challenge, New Game or Continue, the map, Play
            g.live('endless-live', timeout=180, min_bytes=100000)
            g.wait(r'\[mod\] endless course: playing', 60)
            time.sleep(5)
            g.log()
            g.hold('minus', 0.4)  # a tap is no skip
            time.sleep(3)
            self.assertNotIn('[mod] endless skip', g.log())
            for n in (1, 2):
                g.hold('minus', 2.5)
                line = g.wait(r'\[mod\] endless skip: ', 30)
                self.assertIn(f'{n} this session', line)
                g.live('endless-live', timeout=180, min_bytes=100000)  # the next course, played
                time.sleep(6)
                self.assertNotIn('[mod] endless skip', g.log(), 'a skip was counted twice')
            self.assertEqual(total(), before + 2)
        finally:
            g.stop()


if __name__ == '__main__':
    unittest.main()
