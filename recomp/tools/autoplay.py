#!/usr/bin/env python3
"""A closed-loop driver for the game, for automated tests (Windows). It starts the game in its quiet test mode and steers it through a command
file (RECOMP_CTL, read once per frame by runtime/hle/input.c): buttons are pressed, screenshots are saved by the game itself, and the log is read
for what happened. Nothing is sent to the desktop: no key presses, no window focus, no screen capture. The game ignores the keyboard, mouse and
controllers of whoever is at the computer while it runs, and its window opens without taking the focus (or not at all with --hidden), so a test can
run while the machine is in use.

    python recomp/tools/autoplay.py launch [--hidden] [--to-course-world]   start the game (stops one it started before)
    python recomp/tools/autoplay.py do "down a wait:1.5 l+r"                press buttons one after another (see below)
    python recomp/tools/autoplay.py shot name                               save the game's picture as <shots dir>/name.png and print the path
    python recomp/tools/autoplay.py wait REGEX [--timeout 60]               wait for a log line that matches
    python recomp/tools/autoplay.py live [--timeout 60]                     wait until a course is being played
    python recomp/tools/autoplay.py skips N                                 in a live Endless course: hold Minus N times, time each wait
    python recomp/tools/autoplay.py log                                     print the log from where the last command left off
    python recomp/tools/autoplay.py mark text                               put a line in the log, to find the place again
    python recomp/tools/autoplay.py quit                                    end the game

Steps for `do`: a button name presses it for 0.15 s (a b x y l r zl zr plus minus up down left right; `l+r` presses two together),
`hold:<buttons>:<s>` holds it, `wait:<s>` waits. A test written in Python can `import autoplay` and use Game.
"""
import argparse
import os
import re
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DIST = ROOT / 'dist'
LOGDIR = DIST / 'smm2-dx_log'
LOG = LOGDIR / 'smm2-dx.log'
WORK = LOGDIR / 'autoplay'
CTL = WORK / 'ctl.txt'
SHOTS = WORK / 'shots'
MARK = WORK / 'log-offset'
PIDFILE = WORK / 'pid'
EXE = DIST / 'smm2-dx-windows-x64.exe'

BUTTONS = {'a', 'b', 'x', 'y', 'l', 'r', 'zl', 'zr', 'plus', 'minus', 'up', 'down', 'left', 'right', 'lstick', 'rstick'}


class Game:
    """The running game: buttons, pictures and the log."""

    def __init__(self):
        WORK.mkdir(parents=True, exist_ok=True)
        SHOTS.mkdir(parents=True, exist_ok=True)

    # ---- starting and stopping
    def launch(self, hidden=False, extra_env=None, to_course_world=False, timeout=300, replace=False):
        if self.running() and not (self.ours() or replace):
            raise SystemExit('a game is already running that this driver did not start (it may be being played): close it, or pass --replace')
        self.stop()
        CTL.write_text('')
        MARK.write_text('0')
        env = dict(os.environ)
        env['RECOMP_CTL'] = str(CTL)  # (this also makes the run ignore the user's input and not take the focus)
        if hidden:
            env['LARECOMP_HIDDEN'] = '1'
        env.update(extra_env or {})
        p = subprocess.Popen([str(EXE)], cwd=str(DIST), env=env, creationflags=0x00000008 | 0x00000200,  # DETACHED_PROCESS | NEW_PROCESS_GROUP
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        PIDFILE.write_text(str(p.pid))
        # the game starts a log of its own (the old one is still there until it does): wait for this run's own first line
        tag = f'run-{p.pid}-{int(time.time())}'
        self.send('mark ' + tag)
        MARK.write_text('0')
        self.wait(r'\[ctl\] mark ' + tag, 180)
        if to_course_world:
            self.wait(r'skip mode select: sequence done|Course World open', timeout)
            time.sleep(4)  # (the list is drawn)
        return p.pid

    def ours(self):
        """Is the game that is running the one this driver started? (its launcher exe has the pid we noted; the game itself is its child)"""
        if not PIDFILE.exists():
            return False
        pid = PIDFILE.read_text().strip()
        r = subprocess.run(['tasklist', '/FI', f'PID eq {pid}', '/NH'], capture_output=True, text=True)
        if pid in r.stdout:
            return True
        # (the launcher hands over to the game and ends: then the game's own command line has the control file in its environment, which cannot be
        # read from here; the control file having been written to lately is the sign)
        return CTL.exists() and time.time() - CTL.stat().st_mtime < 6 * 3600 and self.running()

    def stop(self):
        """End the game this driver started."""
        if self.running() and CTL.exists():
            self.send('quit')
            end = time.time() + 5
            while time.time() < end and self.running():
                time.sleep(0.2)
        if self.running() and self.ours():
            subprocess.run(['taskkill', '/F', '/IM', 'smm2-dx-runtime.exe'], capture_output=True)
            subprocess.run(['taskkill', '/F', '/IM', 'smm2-dx-windows-x64.exe'], capture_output=True)
        time.sleep(1)

    def running(self):
        r = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq smm2-dx-runtime.exe', '/NH'], capture_output=True, text=True)
        return 'smm2-dx-runtime.exe' in r.stdout

    # ---- commands
    def send(self, line):
        with open(CTL, 'a', newline='\n') as f:
            f.write(line + '\n')

    def hold(self, buttons, seconds=0.15):
        self.send('pad ' + '+'.join(b.upper() for b in buttons.split('+')))
        time.sleep(seconds)
        self.send('pad -')
        time.sleep(0.15)

    def do(self, steps):
        for st in steps.split():
            if st.startswith('wait:'):
                time.sleep(float(st[5:]))
            elif st.startswith('hold:'):
                _, b, s = st.split(':')
                self.hold(b.lower(), float(s))
            else:
                for part in st.lower().split('+'):
                    if part not in BUTTONS:
                        raise SystemExit(f'not a button: {part}')
                self.hold(st.lower())

    def shot(self, name, timeout=10):
        path = SHOTS / (name if name.endswith('.png') else name + '.png')
        path.unlink(missing_ok=True)
        self.send(f'shot {path}')
        end = time.time() + timeout
        while time.time() < end:
            if path.exists() and path.stat().st_size > 0:
                time.sleep(0.1)
                return path
            time.sleep(0.1)
        raise SystemExit(f'the game saved no picture in {timeout:.0f} s: is it running and presenting frames?')

    def live(self, name='live-probe', timeout=60, min_bytes=120000):
        """Wait until a course is being played: its picture is big (a PNG of 120 KB or more: busy levels are about 800 KB, plain ones 170 KB; the intro card is 70 KB and a fade 5 KB).
        Two big pictures in a row, so a fade-in does not count. Returns the picture's path."""
        end = time.time() + timeout
        big = 0
        while time.time() < end:
            p = self.shot(name)
            big = big + 1 if p.stat().st_size >= min_bytes else 0
            if big >= 2:
                return p
            time.sleep(0.15)
        raise SystemExit(f'no course went live in {timeout:.0f} s')

    def skips(self, n, hold=2.5):
        """Hold Minus in a live Endless course n times and time each wait: from the press until the black loading screen is gone and the next
        course's intro card is up (the card is about 70 KB as a PNG, the loading screen 5 KB). The card lasts a few seconds, then the course
        is played, so the next press waits for that. Returns the seconds for each."""
        out = []
        for i in range(n):
            if i == 0:
                self.live('skip-live', timeout=120, min_bytes=50000)
                time.sleep(1.0)
            self.mark(f'skip-{i}')
            t0 = time.time()
            self.hold('minus', hold)
            while True:  # (the picture goes dark while the next course loads)
                p = self.shot('skip-probe')
                if p.stat().st_size < 40000:
                    break
                if time.time() - t0 > 20:
                    raise SystemExit(f'skip {i}: the picture never went dark')
                time.sleep(0.1)
            ok = 0
            while ok < 2:  # (then something is drawn again: the card, or the course)
                if time.time() - t0 > 240:
                    raise SystemExit(f'skip {i}: the next course never appeared')
                ok = ok + 1 if self.shot('skip-probe').stat().st_size >= 50000 else 0
                time.sleep(0.1)
            out.append(round(time.time() - t0, 1))
            time.sleep(6.0)  # (the card, then the course is live)
        return out

    def mark(self, text):
        self.send('mark ' + text)

    def peek(self, addr, length=8):
        """The game's memory at a guest address (a bad address crashes the game), as bytes."""
        self.send(f'peek {addr:x} {length}')
        line = self.wait(r'\[ctl\] peek ', 10)
        return bytes.fromhex(line.rsplit(': ', 1)[1])

    def fnprof_start(self):
        """Start counting the game's function entries (see fnprof.c); fnprof_stop writes the counts."""
        self.send('fnprof start')
        self.wait(r'\[ctl\] fnprof started', 10)

    def fnprof_stop(self, name):
        """Stop counting and return {guest address: (entries, order of first entry)} since fnprof_start."""
        path = WORK / (name + '.fnprof')
        path.unlink(missing_ok=True)
        self.send(f'fnprof stop {path}')
        self.wait(r'\[ctl\] fnprof stopped', 30)
        return {int(p[0], 16): (int(p[1]), int(p[2])) for p in (ln.split() for ln in path.read_text().splitlines())}

    # ---- the log
    def _offset(self, size):
        off = int(MARK.read_text()) if MARK.exists() else 0
        return 0 if off > size else off  # (a new run's log is shorter)

    def log(self):
        if not LOG.exists():
            return ''
        b = LOG.read_bytes()
        off = self._offset(len(b))
        MARK.write_text(str(len(b)))
        return b[off:].decode('utf-8', 'replace')

    def wait(self, regex, timeout=60):
        """The first log line after the mark that matches; the mark moves to just after it."""
        rx = re.compile(regex)
        end = time.time() + timeout
        while time.time() < end:
            if LOG.exists():
                b = LOG.read_bytes()
                off = self._offset(len(b))
                pos = off
                for raw in b[off:].splitlines(keepends=True):
                    pos += len(raw)
                    line = raw.decode('utf-8', 'replace').rstrip('\r\n')
                    if raw.endswith(b'\n') and rx.search(line):
                        MARK.write_text(str(pos))
                        return line
            time.sleep(0.25)
        raise SystemExit(f'no log line matched {regex!r} in {timeout:.0f} s')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('launch')
    s.add_argument('--hidden', action='store_true', help='no window at all')
    s.add_argument('--to-course-world', action='store_true', help='wait until Skip mode select has opened Course World')
    s.add_argument('--replace', action='store_true', help='also end a game that is running which this driver did not start')
    s.add_argument('--env', action='append', default=[], metavar='NAME=VALUE', help='an environment variable for the game (repeatable)')
    s = sub.add_parser('do')
    s.add_argument('steps')
    s = sub.add_parser('shot')
    s.add_argument('name')
    s = sub.add_parser('wait')
    s.add_argument('regex')
    s.add_argument('--timeout', type=float, default=60)
    s = sub.add_parser('skips')
    s.add_argument('n', type=int)
    s = sub.add_parser('live')
    s.add_argument('--timeout', type=float, default=60)
    sub.add_parser('log')
    s = sub.add_parser('mark')
    s.add_argument('text')
    sub.add_parser('quit')
    a = ap.parse_args()
    g = Game()
    if a.cmd == 'launch':
        print('pid', g.launch(a.hidden, extra_env=dict(e.split('=', 1) for e in a.env), to_course_world=a.to_course_world, replace=a.replace))
    elif a.cmd == 'do':
        g.do(a.steps)
    elif a.cmd == 'shot':
        print(g.shot(a.name))
    elif a.cmd == 'wait':
        print(g.wait(a.regex, a.timeout))
    elif a.cmd == 'skips':
        print('seconds from the press of Minus to a playable course:', g.skips(a.n))
    elif a.cmd == 'live':
        print(g.live(timeout=a.timeout))
    elif a.cmd == 'log':
        print(g.log(), end='')
    elif a.cmd == 'mark':
        g.mark(a.text)
    elif a.cmd == 'quit':
        g.stop()


if __name__ == '__main__':
    main()
