#!/usr/bin/env python3
"""smm2-dx Build Helper: builds smm2-dx for Windows (the launcher exe and the smm2-dx_data folder) in <repo>\\dist.

Run it through build-helper.cmd, which fetches a Python of its own first. Needs only Windows 10 or later (the curl
and tar that come with it). The first run downloads the compiler (zig), the libraries (SDL3, shaderc, the Vulkan
headers), Python and hactool, each pinned to a sha256, into dist\\smm2-dx_data\\work; a piece that is missing or broken is
fetched again. Nothing is installed on the machine. The game's files are never downloaded: you supply them in the
setup screen.

Options (a leading - or -- both work; PowerShell-style names are accepted too):
  --clean         delete the compiled objects first (the fetched tools are kept)
  --jobs N        compiles at once (default: one per logical processor)
  --setup         from your own dump: lay out the game files in private\\ (see games/smm2/game.json; --dump-root to change)
  --gen           recompile the game's code ahead of time into generated\\ and build the game library beside the runtime
  --run           start the app when the build is done
  --no-banner     skip the title banner
  --show-warnings print compiler warnings
  --version TEXT  the version the app shows (default: git describe, else dev)
"""
import argparse
import concurrent.futures as cf
import ctypes
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

if sys.platform != 'win32':
    sys.exit('build.py builds the Windows app and runs on Windows only (Linux and macOS: see the Makefile).')

RECOMP = Path(__file__).resolve().parents[1]
OUT = (RECOMP.parent / 'dist').resolve()
DX_NAME = 'smm2-dx_data'
DX = OUT / DX_NAME
WORK = DX / 'work'                      # objects, and the fetched compiler, libraries and tools
DL = WORK / 'dl'                        # downloads, kept so a fetch isn't repeated
SYSTEM32 = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32'
SYS_TAR = str(SYSTEM32 / 'tar.exe')     # bsdtar: zip, tar.gz and tar.zst (not Git's GNU tar)
TARGET = 'x86_64-windows-gnu'

ZIG_VER = '0.17.0'
ZIG_SHA = 'b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428'
ZIG_DIR = WORK / 'zig'                          # the full compiler (builds the exe)
ZIG_SLIM_DIR = WORK / 'zig-x86_64-windows'      # what ships: only what compiling C for Windows needs
DEPS_DIR = WORK / 'deps-windows-x86_64'
PY_DIR = WORK / 'python-windows-x86_64'
HACTOOL_DIR = WORK / 'hactool-windows-x86_64'
PY_IMPORTS = 'import threading, subprocess, json, hashlib, capstone, unicorn, lz4.frame'
GEN_FLAGS = ('-O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime '
             '-D_FILE_OFFSET_BITS=64 -mcpu=x86_64_v2')
DLLS = ('SDL3.dll', 'libiconv-2.dll', 'libshaderc_shared.dll')

# zig's cache (compiler_rt, libc++) lives in work\ too: nothing is installed on the machine, and a stale entry in a
# global cache (a killed build can leave one: "failed to link with LLD: FileNotFound") can't break the build
os.environ['ZIG_GLOBAL_CACHE_DIR'] = str(WORK / 'zig-cache')

SHOW_WARNINGS = False
T0 = time.time()
LIVE = sys.stdout.isatty()


class BuildError(Exception):
    pass


# ------------------------------------------------------------------ what the build says
# The picture (tools/banner.txt) is pinned on the left of the window and the log goes in a panel beside it that
# redraws as lines arrive, so the text scrolls around the picture. In a console without VT support, one that is too
# small, or when the output goes to a file, it falls back to plain lines.
# Everything the build says goes through step(), note(), out() and bar().
def _enable_vt():
    try:
        k = ctypes.windll.kernel32
        h = k.GetStdHandle(-11)
        m = ctypes.c_uint32()
        if k.GetConsoleMode(h, ctypes.byref(m)):
            if m.value & 4:
                return True
            return bool(k.SetConsoleMode(h, m.value | 4))
    except Exception:
        pass
    return False


COLOR = LIVE and _enable_vt()
if LIVE:
    try:
        sys.stdout.reconfigure(encoding='utf-8')    # (the picture is braille characters)
    except Exception:
        pass
ESC = '\033'
SPINNER = '|/-\\'
PANEL = None            # the picture-and-panel layout, when on: dict(left, width, top, rows, status_row, height, log, status)
SPIN = 0


def _c(code, text):
    return f'{ESC}[{code}m{text}{ESC}[0m' if COLOR else text


def get_art_lines(max_rows, max_cols):
    """The picture: braille characters are a 2x4 dot grid each, so it can be shrunk to fit a small window."""
    path = RECOMP / 'tools' / 'banner.txt'
    if not path.exists():
        return []
    lines = path.read_text(encoding='utf-8').rstrip().splitlines()
    n_rows = len(lines)
    n_cols = max((len(ln) for ln in lines), default=0)
    s = 1
    while s < 8 and (-(-n_rows // s) > max_rows or -(-n_cols // s) > max_cols):
        s += 1
    if s == 1:
        return lines
    px_h, px_w = n_rows * 4, n_cols * 2
    bm = [[False] * px_w for _ in range(px_h)]
    dx = (0, 0, 0, 1, 1, 1, 0, 1)       # the dot for each bit of a braille character: x
    dy = (0, 1, 2, 0, 1, 2, 3, 3)       #                                              y
    for r, ln in enumerate(lines):
        for c, ch in enumerate(ln):
            bits = ord(ch) - 0x2800
            if bits <= 0 or bits > 255:
                continue
            for b in range(8):
                if bits & (1 << b):
                    bm[r * 4 + dy[b]][c * 2 + dx[b]] = True
    px_h2, px_w2 = -(-px_h // s), -(-px_w // s)
    need = 1 if s <= 2 else max(1, int(s * s * 0.3))   # (thin lines survive a small shrink; larger ones need weight)
    bm2 = [[False] * px_w2 for _ in range(px_h2)]
    for y in range(px_h2):
        for x in range(px_w2):
            n = 0
            for j in range(s):
                for i in range(s):
                    yy, xx = y * s + j, x * s + i
                    if yy < px_h and xx < px_w and bm[yy][xx]:
                        n += 1
            if n >= need:
                bm2[y][x] = True
    out_lines = []
    for r in range(-(-px_h2 // 4)):
        row = []
        for c in range(-(-px_w2 // 2)):
            bits = 0
            for b in range(8):
                yy, xx = r * 4 + dy[b], c * 2 + dx[b]
                if yy < px_h2 and xx < px_w2 and bm2[yy][xx]:
                    bits |= 1 << b
            row.append(chr(0x2800 + bits))
        out_lines.append(''.join(row))
    return out_lines


def start_panel(version, jobs):
    global PANEL
    if not COLOR:
        return False
    try:
        W, H = shutil.get_terminal_size()
        if W < 90 or H < 22:
            return False
        art = get_art_lines(H - 2, int(W * 0.42))
        if not art:
            return False
        left = max(len(ln) for ln in art) + 3
        width = W - left - 1
        if width < 44:
            return False
    except Exception:
        return False
    sb = [f'{ESC}[2J{ESC}[H{ESC}[?25l']
    for i, ln in enumerate(art):
        sb.append(f'{ESC}[{i + 1};1H{ESC}[35m{ln}{ESC}[0m')
    PANEL = dict(left=left, width=width, top=6, rows=H - 8, status_row=H - 1, height=H, log=[], status='')
    sb.append(f'{ESC}[1;{left}H{ESC}[33msmm2-dx{ESC}[97m  Build Helper{ESC}[0m')
    sb.append(f'{ESC}[2;{left}H{ESC}[90m{version}  -  {jobs} jobs{ESC}[0m')
    sb.append(f'{ESC}[3;{left}H{ESC}[33mby sometimes_steve{ESC}[0m')
    sb.append(f'{ESC}[4;{left}H{ESC}[35m{"-" * min(width, 36)}{ESC}[0m')
    sys.stdout.write(''.join(sb))
    sys.stdout.flush()
    return True


def stop_panel():
    global PANEL
    if PANEL is None:
        return
    h = PANEL['height']
    PANEL = None
    sys.stdout.write(f'{ESC}[?25h{ESC}[{h};1H\n')
    sys.stdout.flush()


def _update_status():
    p = PANEL
    t = p['status'][:p['width']]
    sys.stdout.write(f'{ESC}[{p["status_row"]};{p["left"]}H{ESC}[36m{t.ljust(p["width"])}{ESC}[0m')
    sys.stdout.flush()


def _update_panel():
    p = PANEL
    n = len(p['log'])
    first = max(0, n - p['rows'])
    sb = []
    for i in range(p['rows']):
        idx = first + i
        text, color = p['log'][idx] if idx < n else (' ' * p['width'], 90)
        sb.append(f'{ESC}[{p["top"] + i};{p["left"]}H{ESC}[{color}m{text.ljust(p["width"])}{ESC}[0m')
    sys.stdout.write(''.join(sb))
    _update_status()


def _add_log(text, color=90, indent=''):
    """Adds text to the panel's log, wrapped to its width."""
    w = PANEL['width']
    for line in text.splitlines() or ['']:
        line = line.rstrip()
        pre = indent
        while True:
            room = max(10, w - len(pre))
            if len(line) <= room:
                PANEL['log'].append((pre + line, color))
                break
            PANEL['log'].append((pre + line[:room], color))
            line = line[room:]
            pre = indent + '  '
    _update_panel()


def step(text):
    el = int(time.time() - T0)
    stamp = f'[{el // 60:02d}:{el % 60:02d}] '
    if PANEL:
        _add_log(stamp + text, 36)
        return
    print(_c(35, '[smm2-dx ' + stamp[1:]) + _c(36, text), flush=True)


def note(text):
    if PANEL:
        _add_log(text, 90, '  ')
        return
    print(_c(90, '    ' + text), flush=True)


def out(text, color=90):
    """Output of a program, or any other text: color is an SGR code."""
    if PANEL:
        _add_log(text, color, '  ')
        return
    for line in text.splitlines():
        print(_c(color, line), flush=True)


def bar(done, total, text):
    """One line that redraws itself: [#######-------------] 35%  text"""
    pct = int(100 * done / total) if total > 0 else 0
    fill = pct // 5
    line = f'[{"#" * fill}{"-" * (20 - fill)}] {pct:3d}%  {text}'
    if PANEL:
        PANEL['status'] = line
        _update_status()
    elif LIVE:
        sys.stdout.write('\r    ' + _c(36, line) + '   ')
        sys.stdout.flush()


def busy(label, started):
    """A spinner with the time a step has taken: for steps that print nothing while they work."""
    global SPIN
    if not LIVE:
        return
    SPIN += 1
    line = f'[{SPINNER[SPIN % 4]}] {label}  {time.time() - started:,.0f}s'
    if PANEL:
        PANEL['status'] = line
        _update_status()
    else:
        sys.stdout.write('\r    ' + _c(36, line) + '   ')
        sys.stdout.flush()


def clear_bar():
    """The bar goes away when a step ends."""
    if PANEL:
        PANEL['status'] = ''
        _update_status()
    elif LIVE:
        sys.stdout.write('\n')
        sys.stdout.flush()


def banner(version, jobs, no_art=False):
    try:
        ctypes.windll.kernel32.SetConsoleTitleW(f'smm2-dx Build Helper {version}')
    except Exception:
        pass
    if not no_art and start_panel(version, jobs):
        return
    # plain layout: the title block (and, in a console, the picture shrunk to fit above it)
    print()
    if not no_art and LIVE:
        try:
            W, H = shutil.get_terminal_size()
            for ln in get_art_lines(max(8, H - 8), W - 2):
                print(_c(35, ln))
            print()
        except Exception:
            pass
    print(_c(35, '  ===================================='))
    print('   ' + _c(33, 'smm2-dx  ') + _c(97, f'Build Helper  ({version}, {jobs} jobs)'))
    print(_c(33, '   by sometimes_steve'))
    print(_c(35, '  ===================================='))
    print(flush=True)


# ------------------------------------------------------------------ helpers
def fwd(p):
    """Forward slashes: what the compiler's command line (and its @response files) take without escaping."""
    return str(p).replace('\\', '/')


def run(exe, args=(), cwd=None, quiet=False):
    """Runs a program and waits; raises with its output when it fails. Returns its output."""
    p = subprocess.Popen([str(exe)] + [str(a) for a in args], cwd=str(cwd or RECOMP), stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, encoding='utf-8', errors='replace')
    started = time.time()
    label = Path(str(exe)).stem
    shown = False
    while True:
        try:
            so, se = p.communicate(timeout=0.2)
            break
        except subprocess.TimeoutExpired:
            if time.time() - started >= 1.5:
                busy(f'working ({label})', started)
                shown = True
    if shown:
        clear_bar()
    text = so + se
    if p.returncode != 0:
        raise BuildError(f'{Path(str(exe)).name} failed (exit {p.returncode}):\n{text}')
    if not quiet and text.strip():
        out(text.rstrip())
    return text


def run_parallel(jobs, max_jobs, what):
    """Each job: (name, exe, args, cwd). Raises after all have finished if any failed."""
    if not jobs:
        return
    total, done, failed = len(jobs), 0, []
    step_n, shown = max(1, total // 10), 0

    def one(j):
        name, exe, args, cwd = j
        r = subprocess.run([str(exe)] + [str(a) for a in args], cwd=str(cwd or RECOMP), capture_output=True,
                           text=True, encoding='utf-8', errors='replace')
        return name, r.returncode, (r.stderr + r.stdout).strip()

    with cf.ThreadPoolExecutor(max_workers=max(1, max_jobs)) as ex:
        for fut in cf.as_completed([ex.submit(one, j) for j in jobs]):
            name, code, text = fut.result()
            done += 1
            if code != 0:
                failed.append((name, text))
            elif text and SHOW_WARNINGS:
                out(f'{name}:\n{text}', 33)
            if LIVE:
                bar(done, total, f'{what} {done}/{total}')
            elif done - shown >= step_n or done == total:
                shown = done
                note(f'{what} {done}/{total}')
    if LIVE:
        clear_bar()
    if failed:
        stop_panel()    # (the compiler's messages are long: they go below the layout)
        for name, text in failed:
            out(f'FAILED {name}\n{text}', 31)
        raise BuildError(f'{len(failed)} of {total} failed ({what})')


def move_retry(src, dst):
    """A file or folder that was just written may be held for a moment by antivirus: move it again a few times."""
    for i in range(1, 21):
        try:
            if Path(dst).exists():
                remove_tree(dst) if Path(dst).is_dir() else Path(dst).unlink()
            shutil.move(str(src), str(dst))
            return
        except OSError:
            if i >= 20:
                raise
            time.sleep(0.5)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def remove_tree(path):
    """Removes a file or folder; a junction or link inside is unlinked, never followed."""
    p = Path(path)
    if not (p.exists() or p.is_symlink()):
        return
    if p.is_symlink() or os.path.isjunction(p):
        os.rmdir(p) if p.is_dir() else p.unlink()
        return
    if p.is_file():
        p.unlink()
        return

    def onexc(func, fpath, exc):
        os.chmod(fpath, 0o700)
        func(fpath)
    shutil.rmtree(p, onexc=onexc)


def copy_tree(src, dst, exclude_dirs=()):
    shutil.copytree(src, dst, dirs_exist_ok=True, ignore=shutil.ignore_patterns(*exclude_dirs) if exclude_dirs else None)


def write_text(path, text, crlf=False):
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    if crlf:
        text = text.replace('\r\n', '\n').replace('\n', '\r\n')
    p.write_bytes(text.encode('utf-8'))


def mtime(path):
    try:
        return os.path.getmtime(path)
    except OSError:
        return 0.0


def newest(files):
    return max([mtime(f) for f in files] or [0.0])


def files_in(directory, pattern):
    """The files of a recomp-relative folder matching a pattern, sorted by name."""
    d = RECOMP / directory
    return sorted(glob.glob(str(d / pattern)), key=lambda s: os.path.basename(s))


def glob_rel(pattern):
    return files_in(os.path.dirname(pattern), os.path.basename(pattern))


def expand(archive, dest, strip=0):
    Path(dest).mkdir(parents=True, exist_ok=True)
    args = ['-xf', archive, '-C', dest]
    if strip:
        args.append(f'--strip-components={strip}')
    note(f'unpacking {Path(archive).name}')
    run(SYS_TAR, args, quiet=True)


def get_verified(url, file, sha):
    """Downloads a file and checks it against its pinned sha256; kept in work/dl so it is fetched once."""
    file = Path(file)
    file.parent.mkdir(parents=True, exist_ok=True)
    if file.exists() and sha256_of(file) == sha:
        return
    file.unlink(missing_ok=True)
    if not url.startswith('https://'):
        raise BuildError(f'refusing to download over a non-https URL: {url}')
    note(f'downloading {file.name}')
    part = Path(str(file) + '.part')
    last = None
    for attempt in range(1, 4):
        if attempt > 1:
            note(f'retrying ({attempt} of 3)')
            time.sleep(3)
        try:
            pos = part.stat().st_size if part.exists() else 0
            req = urllib.request.Request(url, headers={'Range': f'bytes={pos}-'} if pos else {})
            with urllib.request.urlopen(req, timeout=60) as r:
                if pos and r.status != 206:
                    pos = 0     # (the server can't continue a download: start again)
                total = pos + int(r.headers.get('Content-Length') or 0)
                done = pos
                with open(part, 'ab' if pos else 'wb') as f:
                    while True:
                        block = r.read(1 << 20)
                        if not block:
                            break
                        f.write(block)
                        done += len(block)
                        bar(done, total, f'{done / 1048576:,.0f} / {total / 1048576:,.0f} MB  {file.name}'
                            if total > done or total else f'{done / 1048576:,.0f} MB  {file.name}')
            if LIVE:
                clear_bar()
            last = None
            break
        except urllib.error.HTTPError as e:
            if LIVE:
                clear_bar()
            if e.code == 416:    # (the partial file was already whole)
                last = None
                break
            last = e
            part.unlink(missing_ok=True)
        except Exception as e:   # (a dropped connection: the partial file is continued on the next try)
            if LIVE:
                clear_bar()
            last = e
            note(str(e))
    if last is not None:
        raise BuildError(f'download failed ({last}): {url}\nDownload it by hand and save it as {file} (see the README)')
    move_retry(part, file)
    got = sha256_of(file)
    if got != sha:
        file.unlink()
        raise BuildError(f'{file.name}: sha256 {got}, expected {sha}')


# ------------------------------------------------------------------ toolchain
def zig():
    return ZIG_DIR / 'zig.exe'


def py():
    return PY_DIR / 'python.exe'


def install_zig():
    if (ZIG_DIR / 'lib').exists() and zig().exists():
        return
    step(f'zig {ZIG_VER}')
    name = f'zig-x86_64-windows-{ZIG_VER}'
    archive = DL / 'zig-dl' / f'{name}.zip'
    get_verified(f'https://ziglang.org/download/{ZIG_VER}/{name}.zip', archive, ZIG_SHA)
    tmp = DL / 'zig-unpack'
    remove_tree(tmp)
    expand(archive, tmp)
    remove_tree(ZIG_DIR)
    ZIG_DIR.parent.mkdir(parents=True, exist_ok=True)
    move_retry(tmp / name, ZIG_DIR)
    remove_tree(tmp)
    if not zig().exists():
        raise BuildError('zig did not unpack')


def install_zig_slim():
    """The copy that ships: the game code is compiled on the player's machine for Windows, so only the Windows C
    headers and libc sources stay (no C++ runtimes, sanitizers or docs). lib\\std stays: zig cc builds compiler_rt."""
    marker = ZIG_SLIM_DIR / '.slim'
    if marker.exists():
        return
    install_zig()
    step('zig (the copy that ships)')
    remove_tree(ZIG_SLIM_DIR)
    copy_tree(ZIG_DIR, ZIG_SLIM_DIR)
    lib = ZIG_SLIM_DIR / 'lib'
    for d in (lib / 'libc' / 'include').iterdir():
        if d.is_dir() and not any(k in d.name for k in ('windows', 'mingw')):
            remove_tree(d)
    for d in (lib / 'libc').iterdir():
        if d.name not in ('include', 'mingw'):
            remove_tree(d)
    for d in ('libcxx', 'libcxxabi', 'libtsan', 'docs'):
        remove_tree(lib / d)
    remove_tree(ZIG_SLIM_DIR / 'doc')
    write_text(marker, 'slim')


def install_deps():
    """SDL3, libiconv, shaderc, the Vulkan headers and loader, VMA: MSYS2's UCRT64 packages, pinned by sha256."""
    if (DEPS_DIR / 'include' / 'SDL3').exists() and (DEPS_DIR / 'bin' / 'libshaderc_shared.dll').exists() \
            and (DEPS_DIR / 'lib' / 'SDL3.lib').exists():
        return
    step('SDL3, shaderc and the Vulkan headers')
    repo = 'https://repo.msys2.org/mingw/ucrt64'
    pkgs = [
        ('f9f8e2fe9858868e7653b24c973bcc8c19dea1096afc8c905bf1cd1a87960442', 'sdl3-3.4.18-1'),
        ('9a500f38c2b91808741c62fae746b3e9110b33a1ecf5c30fa0c66dbedddf7e16', 'libiconv-1.19-1'),
        ('a4ad271f76acd2307ed67738770b30d6486bd3c7acff63ba83dd13e56ce9ab82', 'shaderc-2026.4-1'),
        ('be602b2657cace531316e78b1eda4087e8f7f84ebb877f08fba198a2db06406a', 'vulkan-headers-1~1.4.363.0-1'),
        ('065076fe13d39ab97eea4740596426d5ae741b5044e48e1c9495d04835c92dbe', 'vulkan-loader-1~1.4.363.0-1'),
        ('44c81162337915afecda3185f66fd98268a54454580b2717080c9dfef57fe781', 'vulkan-memory-allocator-3.4.0-1')]
    x = DL / 'msys2-unpack'
    remove_tree(x)
    for sha, name in pkgs:
        f = f'mingw-w64-ucrt-x86_64-{name}-any.pkg.tar.zst'
        get_verified(f'{repo}/{f}', DL / 'msys2' / f, sha)
        expand(DL / 'msys2' / f, x)
    u = x / 'ucrt64'
    remove_tree(DEPS_DIR)
    for d in ('include', 'lib', 'bin'):
        (DEPS_DIR / d).mkdir(parents=True, exist_ok=True)
    for d in ('SDL3', 'vulkan', 'vk_video', 'shaderc'):
        shutil.copytree(u / 'include' / d, DEPS_DIR / 'include' / d)
    shutil.copy(u / 'include' / 'vma' / 'vk_mem_alloc.h', DEPS_DIR / 'include')
    for lib in ('libSDL3.dll.a', 'libvulkan-1.dll.a', 'libshaderc_shared.dll.a'):
        shutil.copy(u / 'lib' / lib, DEPS_DIR / 'lib')
    # (zig looks for these names)
    shutil.copy(u / 'lib' / 'libSDL3.dll.a', DEPS_DIR / 'lib' / 'SDL3.lib')
    shutil.copy(u / 'lib' / 'libvulkan-1.dll.a', DEPS_DIR / 'lib' / 'vulkan-1.lib')
    shutil.copy(u / 'lib' / 'libshaderc_shared.dll.a', DEPS_DIR / 'lib' / 'shaderc_shared.lib')
    for b in DLLS:
        shutil.copy(u / 'bin' / b, DEPS_DIR / 'bin')
    remove_tree(x)


def test_python():
    if not (PY_DIR / 'README-LICENSES.txt').exists():
        return False
    try:
        run(py(), ['-c', PY_IMPORTS], quiet=True)
        return True
    except (BuildError, OSError):
        return False


def _inside_py_dir():
    try:
        Path(sys.executable).resolve().relative_to(PY_DIR.resolve())
        return True
    except ValueError:
        return False


def install_python():
    """Python 3.12 for the recompiler and the setup scripts (python-build-standalone) plus capstone, unicorn, lz4.
    build-helper.cmd fetches and unpacks the interpreter itself (this script runs on it); what is left is its packages."""
    if test_python():
        return
    rel, ver = '20260924', '3.12.14'
    name = f'cpython-{ver}+{rel}-x86_64-pc-windows-msvc-install_only_stripped.tar.gz'
    sha = 'c5bf8edfe858c1df9891be498b5bbc8761d383df5b9790658b088fea4870433a'
    if not _inside_py_dir():
        step('Python 3.12 with capstone, unicorn and lz4')
        get_verified(f'https://github.com/astral-sh/python-build-standalone/releases/download/{rel}/'
                     f'{name.replace("+", "%2B")}', DL / name, sha)
        remove_tree(PY_DIR)
        expand(DL / name, PY_DIR, 1)
    else:
        step('capstone, unicorn and lz4 for the fetched Python')
    interp = sys.executable if _inside_py_dir() else str(py())
    note('installing capstone, unicorn and lz4 (pip)')
    # the generator's packages: this platform's wheels (hash-checked), unpacked into site-packages
    wheels = DL / 'wheels-win_amd64'
    run(interp, ['-m', 'pip', 'download', '-q', '--disable-pip-version-check', '--only-binary=:all:', '--no-deps',
                 '--platform', 'win_amd64', '--python-version', '3.12', '--implementation', 'cp', '--require-hashes',
                 '-r', RECOMP / 'tools' / 'requirements-gen.txt', '-d', wheels], quiet=True)
    import zipfile
    site = PY_DIR / 'Lib' / 'site-packages'
    for whl in sorted(wheels.glob('*.whl')):
        with zipfile.ZipFile(whl) as z:
            z.extractall(site)
    note('trimming what the scripts do not need')
    # what running the scripts doesn't need: GUI toolkits, tests, the package installer, headers, docs
    lib = PY_DIR / 'Lib'
    for d in ('test', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip', 'lib2to3', 'pydoc_data'):
        remove_tree(lib / d)
    for p in site.glob('pip*'):
        remove_tree(p)
    for d in ('include', 'share', 'tcl', 'Scripts', 'libs'):
        remove_tree(PY_DIR / d)
    for pat in ('tcl*', 'tk*', '_tkinter*'):
        for p in (PY_DIR / 'DLLs').glob(pat):
            p.unlink()
    for p in list(PY_DIR.rglob('__pycache__')):
        remove_tree(p)
    for p in list(PY_DIR.rglob('*.a')):
        p.unlink()
    for d in ('capstone', 'unicorn'):
        remove_tree(site / d / 'include')
    write_text(PY_DIR / 'README-LICENSES.txt',
               f'Python {ver} (python-build-standalone {rel}, x86_64-pc-windows-msvc): '
               'https://github.com/astral-sh/python-build-standalone\n'
               'Python: PSF License (LICENSE.txt in this folder or in the Python distribution); the components it includes\n'
               'keep their own licenses (python-build-standalone documents them).\n'
               'Packages for the game code generator: capstone 5.0.9 (BSD-3-Clause), unicorn 2.1.4 (GPL-2.0, unmodified\n'
               'library, run as a separate program\'s module), lz4 4.4.5 (BSD-3-Clause); their licenses are in '
               'site-packages/*.dist-info.\n')
    if not test_python():
        raise BuildError('the fetched Python does not import capstone, unicorn and lz4')


def install_hactool():
    """hactool 1.4.0 (SciresM, ISC; its mbedtls: Apache-2.0), built from the release's source with zig cc."""
    if (HACTOOL_DIR / 'hactool.exe').exists():
        return
    install_zig()
    install_python()
    step('hactool 1.4.0')
    ver = '1.4.0'
    tgz = DL / f'hactool-{ver}.tar.gz'
    get_verified(f'https://github.com/SciresM/hactool/archive/refs/tags/{ver}.tar.gz', tgz,
                 '1373c98ef8c58da42cb1e81b361283955caad0e1b8b5ad389b0ee49b7a21da43')
    src = WORK / f'hactool-src-{ver}'
    remove_tree(src)
    expand(tgz, src, 1)
    run(py(), [RECOMP / 'tools' / 'hactool_patch.py', src], quiet=True)
    remove_tree(HACTOOL_DIR)
    obj_dir = HACTOOL_DIR / 'obj'
    obj_dir.mkdir(parents=True, exist_ok=True)
    cflags = ['-target', TARGET, '-O2', '-std=gnu11', '-w', f'-I{fwd(src)}/mbedtls/include',
              '-D_BSD_SOURCE', '-D_POSIX_SOURCE', '-D_POSIX_C_SOURCE=200112L', '-D_DEFAULT_SOURCE',
              '-D__USE_MINGW_ANSI_STDIO=1', '-D_FILE_OFFSET_BITS=64']
    jobs, objs = [], []
    for c in sorted((src / 'mbedtls' / 'library').glob('*.c')) + sorted(src.glob('*.c')):
        if c.name == 'net_sockets.c':
            continue    # (no networking: Windows would need ws2_32)
        o = obj_dir / f'{c.parent.name}_{c.stem}.o'
        objs.append(o)
        jobs.append((c.name, zig(), ['cc'] + cflags + ['-c', c, '-o', o], None))
    run_parallel(jobs, os.cpu_count() or 1, 'hactool')
    run(zig(), ['cc', '-target', TARGET, '-o', HACTOOL_DIR / 'hactool.exe'] + objs + ['-s', '-ladvapi32'], quiet=True)
    remove_tree(obj_dir)
    for pat in ('*.lib', '*.pdb'):   # (zig leaves an import library next to a Windows exe)
        for p in HACTOOL_DIR.rglob(pat):
            p.unlink()
    write_text(HACTOOL_DIR / 'LICENSE.txt', (src / 'LICENSE').read_text(encoding='utf-8', errors='replace') +
               f"\nhactool {ver}: https://github.com/SciresM/hactool (built from the release's source).\n"
               'Its bundled mbedtls is Apache-2.0: see mbedtls-LICENSE.txt.\n')
    shutil.copy(src / 'mbedtls' / 'apache-2.0.txt', HACTOOL_DIR / 'mbedtls-LICENSE.txt')


# ------------------------------------------------------------------ compiling
def get_version():
    try:
        r = subprocess.run(['git', '-C', str(RECOMP), 'describe', '--tags', '--match', 'v[0-9]*'],
                           capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    except OSError:
        pass
    return 'dev'


def get_lib_id(files, text):
    """sha256 of the files' bytes, in order, then a line of text: what the game code library is built from. A
    library with another id is never loaded (runtime/gamelib.c)."""
    h = hashlib.sha256()
    for f in files:
        h.update(Path(f).read_bytes())
    h.update((text + '\n').encode('utf-8'))
    return h.hexdigest()[:16]


def read_flag_store():
    store = {}
    f = WORK / '.flags'
    if f.exists():
        for line in f.read_text(encoding='utf-8').splitlines():
            k, sep, v = line.partition('\t')
            if sep and k:
                store[k] = v
    return store


def save_flag_store(store):
    write_text(WORK / '.flags', ''.join(f'{k}\t{store[k]}\n' for k in sorted(store)))


def game_defs(cfg):
    return [f'-DGAME_TITLE_ID={cfg["TitleId"]}ull', f'-DGAME_ID="{cfg["GameId"]}"', f'-DGAME_TITLE="{cfg["Title"]}"',
            f'-DGAME_BUNDLE_ID="{cfg["BundleId"]}"', f'-DGAME_DISPLAY_VERSION="{cfg["DisplayVersion"]}"']


class Unit:
    def __init__(self, src, obj, kind, flags, dep, group):
        self.src, self.obj, self.kind, self.flags, self.dep, self.group = str(src), str(obj), kind, flags, dep, group


def get_units(cfg, version, lib_id, lib_srcs):
    B = WORK
    deps_inc = '-I' + fwd(DEPS_DIR / 'include')
    os_flags = ['-D_FILE_OFFSET_BITS=64', '-D_POSIX_THREAD_SAFE_FUNCTIONS', '-D_CRT_RAND_S', '-DWIN32_LEAN_AND_MEAN',
                '-DNOMINMAX']
    gdefs = game_defs(cfg)
    rt_flags = ['-O2', '-g', '-fno-strict-aliasing', '-Wall', '-Wno-unused-function', '-Iruntime', '-Iruntime/nvn',
                '-Ithird_party', '-DCIMGUI_NO_EXPORT', deps_inc] + os_flags + gdefs
    # (the ImGui backends are built with plain C names, so C code can call them: runtime/ui/ig.h)
    cxx_flags = ['-O2', '-g', '-std=c++17', '-Ithird_party', '-Ithird_party/imgui', '-Ithird_party/imgui/backends',
                 '-Iruntime', '-Iruntime/nvn', '-DCIMGUI_NO_EXPORT', '-DIMGUI_IMPL_API=extern "C"', deps_inc] + gdefs
    mbed_flags = ['-Ithird_party/mbedtls/include', '-Iruntime/net', '-DMBEDTLS_USER_CONFIG_FILE="mbedtls_user_config.h"']
    lib_flags = [f'-DGAMELIB_ID="{lib_id}"']
    build_flags = lib_flags + [
        '-DGAMELIB_CFLAGS="' + ' '.join(f for f in GEN_FLAGS.split(' ') if f != '-Iruntime') + '"',
        f'-DGAMEBUILD_HLE="{cfg["Hle"]}"', f'-DGAMEBUILD_PATCHES="{cfg["Patches"]}"']

    # headers every runtime object depends on (and the game's own, for its glue)
    hdrs = []
    for d in ('runtime', 'runtime/platform', 'runtime/nvn', 'runtime/nvn/maxwell', 'runtime/nvn/vulkan', 'runtime/ui',
              'runtime/net'):
        hdrs += files_in(d, '*.h')
    hdrs.append(str(RECOMP / 'games' / cfg['GameId'] / 'game.json'))
    rt_dep = newest(hdrs)
    game_hdrs = [str(p) for p in (RECOMP / 'games' / cfg['GameId']).rglob('*.h')]
    game_dep = newest(hdrs + game_hdrs)
    lib_dep = newest(lib_srcs)
    mbed_dep = newest([RECOMP / 'runtime' / 'net' / 'mbedtls_user_config.h'])
    smaa_inc = B / 'inc' / 'smaa_hlsl.inc'

    units = []

    def rel(p):
        return fwd(Path(p).relative_to(RECOMP))

    # runtime C, in the Makefile's order
    rt_src = []
    for d in ('runtime', 'runtime/hle', 'runtime/services', 'runtime/nvn', 'runtime/nvn/maxwell'):
        rt_src += files_in(d, '*.c')
    rt_src.append(str(RECOMP / 'runtime' / 'platform' / 'windows.c'))
    rt_src += files_in('runtime/net', '*.c')
    for g in cfg['SourceGlobs']:
        rt_src += glob_rel(g)
    rt_src.append(str(RECOMP / 'runtime' / 'platform' / 'windows_exports.c'))
    if not cfg.get('Movies'):
        rt_src = [s for s in rt_src if rel(s) != 'runtime/hle/movie.c']
    for s in rt_src:
        r = rel(s)
        obj = B / 'rt' / (r[len('runtime/'):] if r.startswith('runtime/') else r)
        obj = obj.with_suffix('.o')
        extra, dep = [], rt_dep
        if r.startswith('games/'):
            dep = game_dep
        if r in ('runtime/services/ssl.c', 'runtime/net/http.c', 'runtime/net/roots.c'):
            extra += mbed_flags
            dep = max(dep, mbed_dep)
        if r == 'runtime/services/am.c':  # (the General tab's "Show recomp version": the title screen's Ver. box)
            extra += [f'-DRECOMP_VERSION="{version}"']
        if r == 'runtime/nvn/smaa_data.c':
            extra += [f'-I{fwd(B / "inc")}', '-Ithird_party/smaa/Textures']
            dep = max(dep, mtime(smaa_inc))
        if r == 'runtime/gamelib.c':
            extra += lib_flags
            dep = max(dep, lib_dep)
        if r == 'runtime/gamebuild.c':
            extra += build_flags
            dep = max(dep, lib_dep)
        units.append(Unit(s, obj, 'cc', rt_flags + extra, dep, 'rt'))
    # the Vulkan loader's forwarders (generated)
    units.append(Unit(B / 'vk_shim.c', B / 'rt' / 'vk_shim.o', 'cc', rt_flags + ['-Wno-unused-parameter'], 0.0, 'rt'))

    # Mbed TLS (the game's TLS client)
    for s in files_in('third_party/mbedtls/library', '*.c'):
        units.append(Unit(s, B / 'mbedtls' / (Path(s).stem + '.o'), 'cc', ['-O2', '-g'] + mbed_flags + os_flags,
                          mbed_dep, 'mbedtls'))
    # the Vulkan backend
    for s in files_in('runtime/nvn/vulkan', '*.c'):
        units.append(Unit(s, B / 'rt' / 'nvn' / 'vulkan' / (Path(s).stem + '.o'), 'cc', rt_flags, rt_dep, 'backend'))
    for s in files_in('runtime/nvn/vulkan', '*.cpp'):
        units.append(Unit(s, B / 'rt' / 'nvn' / 'vulkan' / (Path(s).stem + '.o'), 'cxx', cxx_flags + os_flags, rt_dep,
                          'backend'))
    # the setup screen and the overlay menu
    for s in files_in('runtime/ui', '*.c'):
        n = Path(s).stem
        extra = [f'-DRECOMP_VERSION="{version}"'] if n in ('launcher', 'menu') else []
        units.append(Unit(s, B / 'rt' / 'ui' / f'{n}.o', 'cc', rt_flags + extra, rt_dep, 'ui'))
    for s in files_in('runtime/ui', '*.cpp'):
        n = Path(s).stem
        extra = ['-x', 'c++'] + os_flags
        if n in ('launcher', 'menu'):
            extra.append(f'-DRECOMP_VERSION="{version}"')
        units.append(Unit(s, B / 'rt' / 'ui' / f'{n}.o', 'cxx', cxx_flags + extra, rt_dep, 'ui'))
    # Dear ImGui
    units.append(Unit(RECOMP / 'third_party' / 'cimgui' / 'cimgui.cpp', B / 'imgui' / 'cimgui.o', 'cxx', cxx_flags, 0.0,
                      'imgui'))
    for n in ('imgui', 'imgui_demo', 'imgui_draw', 'imgui_tables', 'imgui_widgets'):
        units.append(Unit(RECOMP / 'third_party' / 'imgui' / f'{n}.cpp', B / 'imgui' / f'{n}.o', 'cxx', cxx_flags, 0.0,
                          'imgui'))
    for n in ('imgui_impl_sdl3', 'imgui_impl_vulkan'):
        units.append(Unit(RECOMP / 'third_party' / 'imgui' / 'backends' / f'{n}.cpp', B / 'imgui' / f'{n}.o', 'cxx',
                          cxx_flags, 0.0, 'imgui'))
    return units


def build_smaa_include():
    """SMAA (third_party/smaa, MIT): the reference shader as a C string for runtime/nvn/smaa_data.c."""
    src = RECOMP / 'third_party' / 'smaa' / 'SMAA.hlsl'
    dst = WORK / 'inc' / 'smaa_hlsl.inc'
    if dst.exists() and mtime(dst) > mtime(src):
        return
    data = bytes(b for b in src.read_bytes() if b != 13)
    text = ''.join('?' if b >= 128 else chr(b) for b in data)
    lines = text.split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    write_text(dst, '\n'.join('"' + ln.replace('\\', '\\\\').replace('"', '\\"') + '\\n"' for ln in lines) + '\n')


def build_vk_shim():
    """Windows: the Vulkan loader's entry points, forwarded to vulkan-1.dll loaded at run time."""
    dst = WORK / 'vk_shim.c'
    # a forwarder for every Vulkan function the runtime's sources and the ImGui Vulkan backend mention
    scan = [RECOMP / 'runtime', RECOMP / 'third_party' / 'imgui' / 'backends' / 'imgui_impl_vulkan.cpp']
    srcs = [RECOMP / 'tools' / 'gen_vk_shim.py']
    for s in scan:
        srcs += [x for x in s.rglob('*') if x.suffix in ('.c', '.cpp', '.h')] if s.is_dir() else [s]
    if dst.exists() and mtime(dst) > newest(srcs):
        return
    run(py(), [srcs[0], DEPS_DIR / 'include', dst] + scan, quiet=True)


def build_resource():
    res = WORK / 'windows.res'
    inputs = [RECOMP / 'runtime' / 'platform' / 'windows.rc', RECOMP / 'branding' / 'app-icon.ico']
    if res.exists() and mtime(res) > newest(inputs):
        return
    run(zig(), ['rc', '-fo', res, '-i', 'branding', 'runtime/platform/windows.rc'], quiet=True)


def compile_units(units, jobs):
    store = read_flag_store()
    todo, job_list = [], []
    for u in units:
        h = hashlib.sha1((u.kind + ' ' + ' '.join(u.flags)).encode('utf-8')).hexdigest()
        key = str(Path(u.obj).relative_to(WORK))
        stale = not os.path.exists(u.obj)
        if not stale:
            ot = mtime(u.obj)
            if mtime(u.src) > ot or u.dep > ot or store.get(key) != h:
                stale = True
        if stale:
            Path(u.obj).parent.mkdir(parents=True, exist_ok=True)
            drv = 'c++' if u.kind == 'cxx' else 'cc'
            job_list.append((Path(u.src).name, zig(),
                             [drv, '-target', TARGET] + u.flags + ['-c', fwd(u.src), '-o', fwd(u.obj)], RECOMP))
            todo.append((key, h))
    if not job_list:
        note('objects are up to date')
        return
    step(f'compiling {len(job_list)} of {len(units)} files')
    run_parallel(job_list, jobs, 'compiled')
    for key, h in todo:
        store[key] = h
    save_flag_store(store)


def _rsp_line(s):
    return f'"{s}"' if any(c.isspace() for c in s) else s


def link_runtime(units, runtime_exe, imp_lib):
    res = WORK / 'windows.res'
    objs = [u.obj for u in units]
    stale = not runtime_exe.exists() or not imp_lib.exists()
    if not stale:
        t = mtime(runtime_exe)
        stale = any(mtime(o) > t for o in objs + [str(res)])
    if not stale:
        note(f'{runtime_exe.name} is up to date')
        return
    step(f'linking {runtime_exe.name}')
    # the Makefile's order: runtime, Mbed TLS, the Vulkan backend, the setup screen, ImGui (then the icon and the shim)
    order = [u.obj for g in ('rt', 'mbedtls', 'backend', 'ui', 'imgui') for u in units if u.group == g]
    ld = ['-Wl,--stack,16777216', f'-L{fwd(DEPS_DIR / "lib")}', '-lSDL3', '-lwinmm', '-lole32', '-lshell32',
          '-lcomdlg32', '-luuid', '-lshaderc_shared', '-lws2_32', '-lbcrypt', '-Wl,--export-all-symbols',
          f'-Wl,--out-implib,{fwd(imp_lib)}']
    link_dir = WORK / 'link'
    link_dir.mkdir(parents=True, exist_ok=True)
    runtime_exe.parent.mkdir(parents=True, exist_ok=True)
    tmp = link_dir / runtime_exe.name
    rsp = link_dir / 'link.rsp'
    # (a response file: ~400 objects don't fit on a Windows command line)
    lines = ['-target', TARGET, '-o', fwd(tmp)] + [fwd(o) for o in order] + [fwd(res)] + ld
    write_text(rsp, '\n'.join(_rsp_line(x) for x in lines) + '\n')
    run(zig(), ['c++', f'@{fwd(rsp)}'], RECOMP, quiet=True)
    move_retry(tmp, runtime_exe)
    # the import library the game code DLL links against: the exe's name and what it exports
    # (the names the exe exports are the -export: directives of runtime/platform/windows_exports.c)
    exports = (RECOMP / 'runtime' / 'platform' / 'windows_exports.c').read_text(encoding='utf-8')
    names = [f'{n} DATA' if kind else n for n, kind in re.findall(r'-export:(\w+)(,data)?', exports)]
    write_text(WORK / 'exports.def', f'LIBRARY {runtime_exe.name}\nEXPORTS\n' + '\n'.join(names) + '\n')
    run(zig(), ['dlltool', '-m', 'i386:x86-64', '-d', WORK / 'exports.def', '-l', imp_lib], quiet=True)


def build_launcher(stub_exe):
    """The exe players start: a small launcher that runs the runtime in smm2-dx_data (runtime/platform/launcher_stub.c)."""
    src = RECOMP / 'runtime' / 'platform' / 'launcher_stub.c'
    res = WORK / 'windows.res'
    if stub_exe.exists() and mtime(stub_exe) > newest([src, res]):
        return
    step(f'linking {stub_exe.name}')
    stub_exe.parent.mkdir(parents=True, exist_ok=True)
    run(zig(), ['cc', '-target', TARGET, '-O2', '-s', '-Wl,--subsystem,windows', '-o', stub_exe, src, res], RECOMP,
        quiet=True)


def copy_runtime_dlls():
    DX.mkdir(parents=True, exist_ok=True)
    for d in DLLS:
        s, t = DEPS_DIR / 'bin' / d, DX / d
        if not t.exists() or mtime(t) < mtime(s):
            shutil.copy2(s, t)


# ------------------------------------------------------------------ the dump-based pipeline
def dump_setup(cfg, dump_root):
    """From your own dump: the game files laid out for the generator."""
    dump_root = dump_root or cfg['DumpRoot']
    if dump_root.startswith('~'):
        dump_root = str(Path(os.environ['USERPROFILE']) / dump_root[1:].lstrip('/\\'))
    step(f'setting up the game files from {dump_root}')
    g = cfg['GameId']
    run(py(), [RECOMP / 'games' / g / 'setup.py',
               '--base-romfs', Path(dump_root) / cfg['DumpBaseRomfs'],
               '--update-exefs', Path(dump_root) / cfg['DumpUpdateExefs'],
               '--update-romfs', Path(dump_root) / cfg['DumpUpdateRomfs'],
               '--out', RECOMP / 'private' / g / 'extracted'], quiet=True)


def dump_gen(cfg, lib_id):
    """Recompiles every module to C (AArch64 -> C, on this machine, from your own files)."""
    g = cfg['GameId']
    files = RECOMP / 'private' / g / 'extracted'
    gen = RECOMP / 'generated' / f'{g}-c'
    origin = 0x7000000000
    step('recompiling the game code (sdk, main)')
    tools = RECOMP / 'tools'
    run(py(), ['gen.py', 'sdk', files / 'exefs' / 'sdk', f'0x{origin + 0x4000000:x}', gen / 'sdk',
               '--hle', RECOMP / cfg['Hle'], '--simd', 'c'], tools, quiet=True)
    a = ['gen.py', 'main', files / 'exefs' / 'main', '0x7000000000', gen / 'main']
    if (RECOMP / cfg['Patches']).exists():
        a += ['--patches', RECOMP / cfg['Patches']]
    run(py(), a + ['--simd', 'c'], tools, quiet=True)
    for p in gen.glob('.gen-*'):
        p.unlink()
    write_text(gen / f'.gen-{lib_id}', '')


def build_game_lib(cfg, lib_id, imp_lib, lib_file, jobs):
    g = cfg['GameId']
    gen = RECOMP / 'generated' / f'{g}-c'
    srcs = sorted((gen / 'main').glob('*.c')) + sorted((gen / 'sdk').glob('*.c'))
    if not srcs:
        return
    if not (gen / f'.gen-{lib_id}').exists():
        raise BuildError(f'the generated game code in {gen} is older than the generator and patches '
                         f'(library id {lib_id}): run build-helper.cmd --gen first')
    obj_dir = WORK / 'obj'
    gen_flags = GEN_FLAGS.split(' ')
    cpu_h = RECOMP / 'runtime' / 'cpu.h'
    job_list, objs = [], []
    for s in srcs:
        o = obj_dir / s.parent.name / f'{s.stem}.o'
        objs.append(o)
        if o.exists() and mtime(o) > newest([s, cpu_h]):
            continue
        o.parent.mkdir(parents=True, exist_ok=True)
        job_list.append((s.name, zig(), ['cc', '-target', TARGET] + gen_flags + ['-c', fwd(s), '-o', fwd(o)], RECOMP))
    entry = obj_dir / 'tools' / 'gamelib_entry.o'
    objs.append(entry)
    entry.parent.mkdir(parents=True, exist_ok=True)
    job_list.append(('gamelib_entry.c', zig(), ['cc', '-target', TARGET] + gen_flags + [f'-DGAMELIB_ID="{lib_id}"'] +
                     game_defs(cfg) + ['-c', 'tools/gamelib_entry.c', '-o', fwd(entry)], RECOMP))
    step(f'compiling the game code ({len(job_list)} files)')
    run_parallel(job_list, jobs, 'compiled')
    tmp = Path(str(lib_file) + '.tmp')
    rsp = WORK / 'link' / 'gamelib.rsp'
    lines = ['-target', TARGET, '-shared', fwd(imp_lib), '-o', fwd(tmp)] + [fwd(o) for o in objs]
    write_text(rsp, '\n'.join(_rsp_line(x) for x in lines))
    run(zig(), ['cc', f'@{fwd(rsp)}'], RECOMP, quiet=True)
    move_retry(tmp, lib_file)
    write_text(str(lib_file) + '.id', lib_id + '\n')


# ------------------------------------------------------------------ staging
THIRD_PARTY = """Third-party components in this package and their licenses (the project's own code: see the source repository)

- Dear ImGui (the setup screen and the overlay menu): MIT, licenses/LICENSE-imgui.txt
- SMAA (built in): MIT-style, licenses/LICENSE-smaa.txt
- Mbed TLS 3.6 (built in: the game's TLS client): Apache-2.0, licenses/Apache-2.0.txt
- Zstandard decoder (built in: the shader cache): BSD-3-Clause, licenses/LICENSE-zstd.txt
- bcdec (built in: BC texture decoding where the GPU lacks it): MIT / Unlicense, licenses/LICENSE-bcdec.txt
- zig (zig/, the compiler the game code is built with): MIT, zig/LICENSE; its clang / lld: Apache-2.0 with
  LLVM exception (licenses/Apache-2.0.txt)
- Python 3.12 (python/, python-build-standalone): PSF License and its components' own: python/README-LICENSES.txt
- capstone (BSD-3-Clause), unicorn (GPL-2.0; source: https://github.com/unicorn-engine/unicorn) and lz4
  (BSD-3-Clause), the game code generator's Python packages: python/.../site-packages/*.dist-info
- hactool (hactool/): ISC, hactool/LICENSE.txt; its mbedtls: Apache-2.0, hactool/mbedtls-LICENSE.txt
- SDL3 (SDL3.dll): zlib license, https://github.com/libsdl-org/SDL/blob/main/LICENSE.txt
- shaderc, glslang, SPIRV-Tools (libshaderc_shared.dll): Apache-2.0 / BSD-3-Clause, licenses/Apache-2.0.txt
- libiconv (libiconv-2.dll, used by SDL3): LGPL-2.1, an unmodified MSYS2 build (source: https://www.gnu.org/software/libiconv/)
"""

PLAYER_README = """Super Mario Maker 2 (recompiled) {version} (windows-x64)

Super Mario Maker 2 v3.0.3, recompiled for your computer from YOUR OWN game. Nothing of the game is in this
download: you bring your dump, and the setup screen prepares everything from it, on your machine.

1. Start smm2-dx-windows-x64.exe (keep it together with its smm2-dx_data folder).
2. The setup screen asks for:
   - your Super Mario Maker 2 with the 3.0.3 update: either the decrypted game (the folders with its extracted
     ExeFS and RomFS, e.g. an emulator's "Extract RomFS / ExeFS" of the updated game), or the game's NSP / XCI
     file and its 3.0.3 update's NSP, which are decrypted with your prod.keys;
   - your console's prod.keys and firmware (the folder of its system .nca files, e.g. Ryujinx's
     bis/system/Contents/registered): the game's fonts come from there. They are read where they are, never copied;
     key values are never shown or logged.
3. Press "Set up and play". The first time it checks your files against the known 3.0.3 version, extracts them,
   copies the fonts and recompiles the game code (a few minutes to half an hour), then the game starts.
   Later starts go straight to the game. The overlay menu (F1 / Esc) > Mods > Game files brings the setup back.

Your saves, settings and the set-up game files are in the smm2-dx_data folder beside the program (keep it somewhere
you can write to, e.g. Documents - not Program Files). The logs are in smm2-dx_log, also beside it.

Keys: WASD stick, arrows d-pad, K/J/L/I = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter = +, Backspace = -, F1 menu.
Controllers work through SDL.

Windows 10 / 11, 64-bit, a CPU with x86-64-v2 (AVX2 used when available) and a graphics driver with Vulkan 1.3.

Help, status and the source: https://github.com/sometimes-steve/smm2-dx - the setup screen tells you when a new
release is out (it only links to it; nothing is downloaded by itself).
"""


def copy_support(dest, cfg):
    """Puts what the runtime looks for into dest: python/, tools/ (the recompiler and the game's setup scripts),
    gamelib-src/ (what the game code is compiled against), zig/, hactool/ and licenses/."""
    dest = Path(dest)
    g = cfg['GameId']
    for d in ('python', 'zig', 'hactool', 'tools', 'gamelib-src', 'licenses'):
        remove_tree(dest / d)
    for d in (f'tools/games/{g}', 'gamelib-src/mods', 'licenses'):
        (dest / d).mkdir(parents=True, exist_ok=True)

    install_python()
    note('copying python')
    copy_tree(PY_DIR, dest / 'python', ('__pycache__',))

    # the recompiler and the setup scripts
    for f in ('gen.py', 'lifter.py', 'analyze.py', 'nso.py', 'passes.py', 'patches.py', 'simd_c.py'):
        shutil.copy(RECOMP / 'tools' / f, dest / 'tools')
    for f in ('setup.py', 'sysdata.py', 'mii_stub.py', 'mini_head.json', 'BIZUDPGothic-Regular.ttf',
              'BIZUDPGothic-OFL.txt', 'versions.py'):
        shutil.copy(RECOMP / 'games' / g / f, dest / 'tools' / 'games' / g)
    # what the game code is compiled against (the same files the library id hashes)
    for f in ('cpu.h', 'simd.h', 'simd_fp.h', 'gamelib.h', 'game.h'):
        shutil.copy(RECOMP / 'runtime' / f, dest / 'gamelib-src')
    shutil.copy(RECOMP / 'tools' / 'gamelib_entry.c', dest / 'gamelib-src')
    shutil.copy(RECOMP / cfg['Hle'], dest / 'gamelib-src' / 'hle_symbols.txt')
    shutil.copy(RECOMP / cfg['Patches'], dest / 'gamelib-src' / 'patches.py')
    # every mod patch file the game's patches.py loads: the same files the library id hashes
    for p in glob_rel(cfg['PatchDeps']):
        shutil.copy(p, dest / 'gamelib-src' / 'mods')

    install_zig_slim()
    note('copying zig')
    copy_tree(ZIG_SLIM_DIR, dest / 'zig')
    (dest / 'zig' / '.slim').unlink(missing_ok=True)
    install_hactool()
    copy_tree(HACTOOL_DIR, dest / 'hactool')

    lic = dest / 'licenses'
    shutil.copy(RECOMP / 'third_party' / 'imgui' / 'LICENSE.txt', lic / 'LICENSE-imgui.txt')
    if (RECOMP / 'third_party' / 'smaa' / 'LICENSE.txt').exists():
        shutil.copy(RECOMP / 'third_party' / 'smaa' / 'LICENSE.txt', lic / 'LICENSE-smaa.txt')
    shutil.copy(RECOMP / 'third_party' / 'zstd' / 'LICENSE', lic / 'LICENSE-zstd.txt')
    shutil.copy(RECOMP / 'third_party' / 'bcdec' / 'LICENSE', lic / 'LICENSE-bcdec.txt')
    shutil.copy(dest / 'hactool' / 'mbedtls-LICENSE.txt', lic / 'Apache-2.0.txt')
    write_text(dest / 'THIRD-PARTY.txt', THIRD_PARTY, crlf=True)


def write_player_readme(path, version):
    write_text(path, PLAYER_README.format(version=version), crlf=True)


def update_staging(cfg, version, sources):
    """Stages the build's support files beside the runtime, again when what they are made of changes or any is missing."""
    stamp = WORK / '.staged'
    need = ['python/python.exe', 'zig/zig.exe', 'hactool/hactool.exe', 'tools/gen.py', 'README.txt']
    ok = stamp.exists() and all((DX / n).exists() for n in need)
    if ok:
        t = mtime(stamp)
        if newest(list(sources) + [RECOMP / 'tools' / 'build.py']) > t:
            ok = False
        else:   # (the staged Python must still import what the generator needs)
            try:
                run(DX / 'python' / 'python.exe', ['-c', PY_IMPORTS], quiet=True)
            except (BuildError, OSError):
                ok = False
    if ok:
        note('smm2-dx_data is staged')
        return
    step('staging python, zig, hactool and the recompiler into smm2-dx_data')
    copy_support(DX, cfg)
    write_player_readme(DX / 'README.txt', version)
    write_text(stamp, 'staged')


# ------------------------------------------------------------------ main
def parse_args(argv):
    p = argparse.ArgumentParser(description='smm2-dx Build Helper (Windows)', allow_abbrev=False)

    def flag(*names, **kw):
        p.add_argument(*names, **kw)
    flag('--clean', '-Clean', action='store_true')
    flag('--jobs', '-Jobs', type=int, default=0)
    flag('--setup', '-Setup', action='store_true')
    flag('--gen', '-Gen', action='store_true')
    flag('--run', '-Run', action='store_true')
    flag('--show-warnings', '-ShowWarnings', action='store_true')
    flag('--no-banner', '-NoBanner', action='store_true')
    flag('--dump-root', '-DumpRoot', default='')
    flag('--game', '-Game', default='smm2')
    flag('--version', '-Version', default='')
    return p.parse_args(argv)


def main(argv):
    global SHOW_WARNINGS
    a = parse_args(argv)
    SHOW_WARNINGS = a.show_warnings
    cfg = json.loads((RECOMP / 'games' / a.game / 'game.json').read_text(encoding='utf-8'))
    jobs = a.jobs if a.jobs >= 1 else (os.cpu_count() or 1)
    version = a.version or get_version()
    banner(version, jobs, a.no_banner)
    stub_exe = OUT / f'{cfg["GameId"]}-dx-windows-x64.exe'      # the exe players start
    runtime_exe = DX / f'{cfg["GameId"]}-dx-runtime.exe'           # the runtime, beside its DLLs
    imp_lib = DX / f'{cfg["GameId"]}-dx.dll.a'                     # what the game code library links against
    game_lib = DX / f'{cfg["GameId"]}game.dll'

    if a.clean:
        step('cleaning')
        for d in ('rt', 'imgui', 'mbedtls', 'inc', 'link', 'obj'):
            remove_tree(WORK / d)
        for f in ('vk_shim.c', 'windows.res', 'exports.def', '.flags', '.staged'):
            (WORK / f).unlink(missing_ok=True)

    # the toolchain
    install_zig()
    install_deps()
    install_python()

    # what the game code library is built from: the generator, the headers it is compiled against, the flags
    lib_srcs = [RECOMP / f for f in ('tools/gen.py', 'tools/lifter.py', 'tools/analyze.py', 'tools/nso.py',
                                     'tools/passes.py', 'tools/patches.py', 'tools/simd_c.py',
                                     'tools/gamelib_entry.c', cfg['Hle'])]
    if (RECOMP / cfg['Patches']).exists():
        lib_srcs.append(RECOMP / cfg['Patches'])
    lib_srcs += [Path(p) for p in glob_rel(cfg['PatchDeps'])]
    lib_srcs += [RECOMP / f for f in ('runtime/cpu.h', 'runtime/simd.h', 'runtime/simd_fp.h', 'runtime/gamelib.h')]
    lib_id = get_lib_id(lib_srcs, GEN_FLAGS)

    if a.setup:
        dump_setup(cfg, a.dump_root)
    if a.gen:
        dump_gen(cfg, lib_id)

    step(f'building smm2-dx {version}')
    build_smaa_include()
    build_vk_shim()
    build_resource()
    units = get_units(cfg, version, lib_id, lib_srcs)
    compile_units(units, jobs)
    link_runtime(units, runtime_exe, imp_lib)
    build_launcher(stub_exe)
    copy_runtime_dlls()
    build_game_lib(cfg, lib_id, imp_lib, game_lib, jobs)
    g = RECOMP / 'games' / 'smm2'
    update_staging(cfg, version, lib_srcs + [g / f for f in ('setup.py', 'sysdata.py', 'mii_stub.py', 'mini_head.json',
                                                            'BIZUDPGothic-Regular.ttf', 'BIZUDPGothic-OFL.txt',
                                                            'versions.py')])

    stop_panel()
    print(_c(32, f'Done in {time.time() - T0:,.0f}s: {stub_exe}'))
    print()
    print(_c(33, 'If you enjoy my work, consider buying me a coffee! https://buymeacoffee.com/sometimes_steve'))
    run_now = a.run
    if not run_now and sys.stdin.isatty():
        print()
        try:
            run_now = not input('Launch smm2-dx now? [Y/n] ').strip().lower().startswith('n')
        except EOFError:
            run_now = False
    if run_now:
        subprocess.Popen([str(stub_exe)], cwd=str(stub_exe.parent),
                         creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP)


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except BuildError as e:
        stop_panel()
        print(_c(31, str(e)), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        stop_panel()
        sys.exit(130)
