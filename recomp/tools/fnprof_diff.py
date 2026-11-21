#!/usr/bin/env python3
"""What did a hitching frame run that an ordinary one did not? Reads the files the runtime writes with LARECOMP_FNPROF_LONG=<ms>
(fnprof-long-<frame>.txt, fnprof-norm.txt: guest function address and entry count per line), subtracts the ordinary frame's counts from
the hitch's, and names the functions with the game's own symbols.

    python recomp/tools/fnprof_diff.py dist/smm2-dx_data/fnprof-long-22710.txt [--norm FILE] [--top N] [--exefs DIR]
"""
import argparse
import bisect
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nso import Nso  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
MODS = [('main', 0x7000000000), ('sdk', 0x7004000000)]


def load_symbols(exefs):
    out = []
    for name, base in MODS:
        n = Nso(str(exefs / name))
        syms = sorted((s[3], s[0]) for s in n.syms if s[2] and (s[1] & 0xF) == 2)
        out.append((base, n.size, [a for a, _ in syms], [s for _, s in syms]))
    return out


def lookup(mods, addr):
    for base, size, addrs, names in mods:
        if base <= addr < base + size:
            off = addr - base
            i = bisect.bisect_right(addrs, off) - 1
            if i >= 0:
                return names[i] if off == addrs[i] else f'{names[i]}+{off - addrs[i]:#x}'
            return f'{name_of(base)}+{off:#x}'
    return '?'


def name_of(base):
    return next(n for n, b in MODS if b == base)


def demangle(names):
    try:
        r = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True, check=True)
        out = r.stdout.splitlines()
        return out if len(out) == len(names) else names
    except (OSError, subprocess.CalledProcessError):
        return names


def read(path):
    d = {}
    for line in Path(path).read_text().splitlines():
        a, c = line.split()
        d[int(a, 16)] = int(c)
    return d


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('long')
    ap.add_argument('--norm', help='the ordinary frame (default: fnprof-norm.txt next to it)')
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--exefs', default=str(ROOT / 'dist' / 'smm2-dx_data' / 'extracted' / 'exefs'))
    a = ap.parse_args()
    long = read(a.long)
    norm = read(a.norm or Path(a.long).with_name('fnprof-norm.txt'))
    diff = sorted(((c - norm.get(ad, 0), ad, c) for ad, c in long.items()), reverse=True)
    mods = load_symbols(Path(a.exefs))
    rows = diff[:a.top]
    names = demangle([lookup(mods, ad) for _, ad, _ in rows])
    print(f'{a.long}: {sum(long.values())} entries in {len(long)} functions; ordinary frame {sum(norm.values())} in {len(norm)}')
    print(f'{"extra":>9} {"in hitch":>9}  address        function')
    for (d, ad, c), nm in zip(rows, names):
        print(f'{d:>9} {c:>9}  {ad:#014x}  {nm[:120]}')


if __name__ == '__main__':
    main()
