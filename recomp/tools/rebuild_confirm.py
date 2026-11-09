#!/usr/bin/env python3
"""Rebuild everything from clean and confirm the result.

  python recomp/tools/rebuild_confirm.py              clean build, then the checks below
  python recomp/tools/rebuild_confirm.py --gen        also recompile the game's code (--gen: long, needs the game's files set up)
  python recomp/tools/rebuild_confirm.py --install    afterwards copy the new runtime into the launcher's build folder (old one kept as .bak)
  python recomp/tools/rebuild_confirm.py --install --target "<path to smm2-dx_data>"

Checks (exit code 0 only when every one passes):
  1. build-helper.cmd --clean exits 0
  2. the compiler rebuilt every file ("compiling N of M" with N = M: nothing was reused from an earlier build)
  3. the runtime exe and the launcher exe exist and are newer than this script's start
  4. text your uncommitted source changes added (string literals in the git diff) is in the runtime exe: the binary
     really contains your edits, not an older build
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # repo root (this file is in recomp/tools)
GREEN, RED, YELLOW, RESET = "\033[32m", "\033[31m", "\033[33m", "\033[0m"
failures = []


def color(text, c):
    return f"{c}{text}{RESET}" if sys.stdout.isatty() else text


def ok(msg):
    print(color("  PASS  " + msg, GREEN))


def fail(msg):
    print(color("  FAIL  " + msg, RED))
    failures.append(msg)


def git(*args):
    r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", check=True)
    return r.stdout


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description="Clean rebuild, then confirm the result.")
    ap.add_argument("--gen", action="store_true", help="also recompile the game's code (long)")
    ap.add_argument("--install", action="store_true",
                    help="copy the new runtime into the launcher's build folder (old one kept as .bak)")
    ap.add_argument("--target", default="", help="path to smm2-dx_data for --install")
    ap.add_argument("--version", default="", help="the version the app shows (default: git describe); e.g. v0.4.0+test")
    a = ap.parse_args()

    os.chdir(ROOT)
    start = time.time()

    print("== source state")
    head = git("rev-parse", "--short", "HEAD").strip()
    dirty = [l for l in git("status", "--short").splitlines() if l.strip()]
    print(f"  HEAD {head}, {len(dirty)} changed file(s)")
    for d in dirty:
        print("    " + d)

    print("== clean build")
    bargs = ["--clean", "--no-banner", "--show-warnings"]
    if a.gen:
        bargs.append("--gen")
    if a.version:
        bargs += ["--version", a.version]
    log = ROOT / "rebuild-confirm.build.log"
    # (the helper asks "Launch smm2-dx now? [Y/n]" at the end: with stdin closed it ends there)
    p = subprocess.run([str(ROOT / "build-helper.cmd"), *bargs], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    out = p.stdout
    log.write_text(out, encoding="utf-8")
    print("  full output: " + str(log))
    if p.returncode == 0:
        ok("build-helper exited 0")
    else:
        fail(f"build-helper exited {p.returncode}")

    ms = re.findall(r"compiling (\d+) of (\d+) files", out)
    if ms:
        n, t = map(int, ms[-1])
        if n == t:
            ok(f"all {t} files were compiled (nothing reused)")
        else:
            fail(f"only {n} of {t} files were compiled")
    else:
        fail("no 'compiling N of M files' line in the output")
    warn = [l for l in out.splitlines() if re.search(r"warning:|error:", l)]
    if warn:
        print(color(f"  note  {len(warn)} compiler warning/error line(s): see the log", YELLOW))

    print("== outputs")
    rt = ROOT / "dist" / "smm2-dx_data" / "smm2-dx-runtime.exe"
    ln = ROOT / "dist" / "smm2-dx-windows-x64.exe"
    for f in (rt, ln):
        if not f.exists():
            fail("missing: " + str(f))
            continue
        st = f.stat()
        h = sha256(f)[:16]
        if st.st_mtime >= start:
            built = datetime.fromtimestamp(st.st_mtime).strftime("%H:%M:%S")
            ok(f"{f.name}  {st.st_size} bytes  sha256 {h}...  built {built}")
        else:
            fail(f"{f.name} was not rewritten by this build ({datetime.fromtimestamp(st.st_mtime)})")

    print("== your changes are in the binary")
    if rt.exists():
        data = rt.read_bytes()
        lits = []
        diff = git("diff", "-U0", "--", "recomp/*.c", "recomp/*.h")
        for line in diff.splitlines():
            if not re.match(r"^\+[^+]", line):
                continue
            code = re.sub(r"//.*$", "", line)  # (a comment is not in the binary)
            # string literals paired left to right; only plain ones (no format codes or escapes: the binary spells those differently)
            for m in re.finditer(r'"(?:[^"\\]|\\.)*"', code):
                v = m.group(0)[1:-1]
                if len(v) >= 14 and not re.search(r"[%\\]", v):
                    lits.append(v)
        lits = list(dict.fromkeys(lits))
        if not lits:
            print("  (no new string literals in the uncommitted changes to look for)")
        else:
            miss = 0
            for l in lits:
                if l.encode("latin-1", errors="replace") not in data:
                    miss += 1
                    fail(f'not in the runtime: "{l}"')
            if not miss:
                ok(f"all {len(lits)} new string(s) from your changes are in the runtime exe")

    if a.install and not failures:
        print("== install")
        target = Path(a.target) if a.target else ROOT.parents[2] / "builds" / "v0.4.0+base" / "smm2-dx_data"
        dst = target / "smm2-dx-runtime.exe"
        try:
            if dst.exists():
                shutil.copy2(dst, str(dst) + ".bak")
            shutil.copy2(rt, dst)
            if sha256(rt) == sha256(dst):
                ok(f"copied to {dst} (identical; old one kept as .bak)")
            else:
                fail("the copy differs from the build")
        except OSError as e:
            fail(f"could not copy (is the game running?): {e}")

    print()
    if failures:
        print(color(f"REBUILD NOT CONFIRMED: {len(failures)} check(s) failed", RED))
        return 1
    print(color("REBUILD CONFIRMED", GREEN))
    return 0


if __name__ == "__main__":
    sys.exit(main())
