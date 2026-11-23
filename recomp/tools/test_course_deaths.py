#!/usr/bin/env python3
"""Test of the personal death counter (issue #33) in the running game, through the autoplay driver (no desktop input, no focus: see autoplay.py).
It starts the game hidden, gets into a course (the one the game resumes in, or the Endless Challenge), waits 15 s with nothing pressed (no death may be
counted), runs Mario about until he dies, and checks that the log says a death was counted for the course being played, that the number in the log is one more than the file
course-deaths.txt had, and that the file now says so too. The game runs on a copy of the settings file. It plays the player's own course (an
Endless run loses a life, as dying there does). Mario is not steered: if he does not die within 150 s, the test is skipped, not failed.

    python recomp/tools/test_course_deaths.py        (needs the game set up and signed in to OpenCourse World; takes about two minutes)
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

FILE = autoplay.DIST / 'smm2-dx_data' / 'course-deaths.txt'


def deaths(course):
    for ln in FILE.read_text().splitlines() if FILE.exists() else []:
        a = ln.split()
        if len(a) == 2 and a[0] == str(course):
            return int(a[1])
    return 0


class CourseDeaths(unittest.TestCase):
    def test_a_death_is_counted(self):
        g = autoplay.Game()
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        ini = tmp / 'smm2.ini'  # (so the test never writes the player's own settings)
        src = autoplay.DIST / 'smm2-dx_data' / 'smm2.ini'
        ini.write_text(src.read_text() if src.exists() else '')
        g.launch(hidden=True, to_course_world=True, replace=True, extra_env={'LARECOMP_SETTINGS': str(ini)})
        try:
            try:  # the game picks up where it was left: in a course
                g.wait(r'\[mod\] course playing: [1-9]', 25)
            except SystemExit:
                g.enter_endless()
            ids = re.findall(r'\[mod\] course playing: ([1-9]\d*)', autoplay.LOG.read_text(errors='replace'))
            self.assertTrue(ids, 'no course was played')
            course = int(ids[-1])
            self.assertGreater(course, 0)
            before = deaths(course)
            g.log()
            time.sleep(15)  # (nothing pressed: a course starting is no death)
            self.assertNotIn('[mod] course death', g.log())
            self.assertEqual(deaths(course), before)
            end = time.time() + 150
            died = None
            while time.time() < end and not died:  # run right and jump until Mario dies
                g.send('pad RIGHT+Y')
                time.sleep(1.0)
                g.send('pad RIGHT+Y+B')
                time.sleep(1.0)
                m = re.search(r'\[mod\] course death: course (\d+), (\d+) in all', g.log())
                died = m
            g.send('pad -')
            if not died:  # (a course Mario can run through: nothing to count)
                self.skipTest('Mario did not die in 150 s')
            self.assertEqual(int(died.group(1)), course)
            self.assertEqual(int(died.group(2)), before + 1)
            self.assertEqual(deaths(course), before + 1)
        finally:
            g.stop()


if __name__ == '__main__':
    unittest.main()
