#!/usr/bin/env python3
"""Syntax-check source files with the Build Helper's own compiler flags, without building or touching dist\\ objects:

    python check_unit.py runtime/ui/menu.c [more files ...]      (paths relative to recomp/)

Uses the fetched zig in dist\\smm2-dx_data\\work (run build-helper.cmd once first)."""
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path

spec = importlib.util.spec_from_file_location('build', Path(__file__).with_name('build.py'))
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)
cfg = json.loads((b.RECOMP / 'games' / 'smm2' / 'game.json').read_text(encoding='utf-8'))
units = b.get_units(cfg, 'dev', 'check', [b.RECOMP / 'tools' / 'gen.py'])
by_src = {Path(u.src).resolve(): u for u in units}
rc = 0
for arg in sys.argv[1:]:
    src = (b.RECOMP / arg).resolve()
    u = by_src.get(src)
    if u is None:
        print(f'{arg}: not a unit of the build', file=sys.stderr)
        rc = 1
        continue
    drv = 'c++' if u.kind == 'cxx' else 'cc'
    flags = [f for f in u.flags if f != '-g']
    with tempfile.TemporaryDirectory() as tmp:    # (compiled, not -fsyntax-only: zig mishandles that with its cache)
        r = subprocess.run([str(b.zig()), drv, '-target', b.TARGET] + flags + ['-c', b.fwd(src), '-o', b.fwd(Path(tmp) / 'x.o')],
                           cwd=str(b.RECOMP), capture_output=True, text=True, encoding='utf-8', errors='replace')
    text = (r.stderr + r.stdout).strip()
    print(f'{arg}: {"ok" if r.returncode == 0 else "FAILED"}')
    if text:
        print(text)
    rc |= r.returncode != 0
sys.exit(rc)
