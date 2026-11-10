"""Set up the system data the game reads: the button-glyph font, from the player's own firmware, and the Mii data.

    fonts.py --firmware <registered NCA folder> --keys <prod.keys> --data <data root> [--hactool PATH]

Writes <data root>/fonts/*.bfttf (the console's NintendoExtension font, nn::pl: the button glyphs; the game crashes
without it, while the Standard, Korean and Chinese shared fonts are not needed: runtime/hle/pl.c uses the bundled
BIZ UDPGothic, see below) and <data root>/sysdata/0100000000000802/ (the Mii face-part archive the game opens by data
id; runtime/services/fs.c serves it): the Mii archive is the console's own again. With --stub-mii it is instead made by
mii_stub.py, from nothing of the console's (a Mii with a rounded-box head), and written first, whatever the firmware is. The
game ran without the console's other system data archives (NgWord 0806 / 0823, TimeZone 080e), so they are not
extracted. The firmware is the NCA folder of a
system dump, e.g. Ryujinx's bis/system/Contents/registered. Keys are only handed to hactool and read in place;
hactool's output is never shown (it can echo key values), only whether each step worked. Lines for the launcher
(runtime/ui/launcher.cpp) start with '@', as setup.py's: "@progress <0..1>", "@error <code>\t<what>\t<what to do>".
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from setup import HACTOOL_CRASHES, masked  # noqa: E402  (setup.py: the crash codes, output without key values)
import mii_stub  # noqa: E402  (writes the Mii face-part archive)

FONT_TITLES = {
    '0100000000000810',  # NintendoExtension (button glyphs): the game stops with a crash if it has none (see pl.c)
}
DATA_TITLES = {'0100000000000802'}  # MiiModel: the console's Mii face parts (nn::mii renders the players' Miis)
# The shared fonts the game falls back to for characters the Standard stand-in lacks (a maker named with simplified
# Chinese characters showed one of them as a "?" box): 0812 Korean, 0813 Chinese (Traditional), 0814 Chinese (Simplified). Taken when
# the firmware has them; the game runs without (runtime/hle/pl.c loads each the first time the game asks for it). 0811 Standard
# (Latin + Japanese) is not taken: BIZ UDPGothic, bundled, stands in for it.
OPTIONAL_FONT_TITLES = {'0100000000000812', '0100000000000813', '0100000000000814'}
CJK_MARKER = 'fonts/.cjk-checked'  # written once the firmware has been looked through for them (found or not), so setup asks once
FONT_FILE = 'nintendo_ext_003.bfttf'   # what has to be in <data>/fonts afterwards


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, errors='replace')


def fail(code, what, hint):
    print(f'@error {code}\t{what}\t{hint}', flush=True)
    sys.exit(f'sysdata: {what}')


def nca_path(entry):
    # Ryujinx keeps each NCA as a folder <id>.nca/00; a plain dump has files
    return os.path.join(entry, '00') if os.path.isdir(entry) else entry


FAILS = []  # (exit code, output) of the NCAs hactool couldn't read, for the error
MII_MODEL = '0100000000000802'  # MiiModel: Mii face parts (nn::mii renders the players' Miis; SMM2 stops at its first Mii without it, issue #28)


UI_FONT_FILES = ('BIZUDPGothic-Regular.ttf', 'BIZUDPGothic-OFL.txt')  # the font that stands in for Standard, and its licence
UI_FONT_DIR = os.path.dirname(os.path.abspath(__file__))


def install_ui_font(data):
    """Copy the bundled Standard-font stand-in (BIZ UDPGothic, SIL Open Font License 1.1: the licence goes with it) to <data>/fonts/."""
    fonts = os.path.join(data, 'fonts')
    os.makedirs(fonts, exist_ok=True)
    for name in UI_FONT_FILES:
        src = os.path.join(UI_FONT_DIR, name)
        if not os.path.isfile(src):
            fail('UI_FONT', f'{name} (the font that comes with this program) was not found',
                 'Reinstall this program: the file is part of it.')
        shutil.copyfile(src, os.path.join(fonts, name))
    return len(UI_FONT_FILES)


def install_mii_face(data):
    """Write the Mii face-part archive (a rounded-box head, nothing else) to <data>/sysdata/<MII_MODEL>/, replacing what is there."""
    out = os.path.join(data, 'sysdata', MII_MODEL)
    if os.path.isdir(out):
        shutil.rmtree(out)
    return len(mii_stub.write(out))


def title_id(hactool, keys, nca):
    r = run([hactool, '-k', keys, '--disablekeywarns', '-t', 'nca', nca])
    for line in r.stdout.splitlines():
        if line.strip().startswith('Title ID:'):
            return line.split(':', 1)[1].strip().lower()
    FAILS.append((r.returncode, r.stdout + r.stderr))
    return None


def why_none():
    """What hactool said about the NCAs it couldn't read: a crash (no output, an exit code), or its last words."""
    if not FAILS:
        return ''
    if any('Invalid NCA header' in out for rc, out in FAILS):  # (masked() drops it: "Are keys correct?")
        return ' (prod.keys doesn\'t open them: the header key is missing or wrong)'
    silent = [rc for rc, out in FAILS if not out.strip()]
    if len(silent) == len(FAILS):
        code = silent[0] & 0xFFFFFFFF
        return f' (hactool stopped without a message: {HACTOOL_CRASHES.get(code, f"exit code {silent[0]} ({code:#x})")})'
    tail = [ln for rc, out in FAILS for ln in masked(out).splitlines() if ln.strip()][-2:]
    return f' (hactool: {" / ".join(tail)})' if tail else ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--firmware', required=True)
    ap.add_argument('--keys', required=True)
    ap.add_argument('--data', required=True)
    ap.add_argument('--hactool', default=shutil.which('hactool') or os.path.expanduser('~/smm-work/tools/bin/hactool'))
    ap.add_argument('--stub-mii', action='store_true', help="write mii_stub.py's cube-head Mii data instead of the console's")
    a = ap.parse_args()
    if a.stub_mii:
        print(f'sysdata: {install_mii_face(a.data)} Mii files (mini head)')
    print(f'sysdata: {install_ui_font(a.data)} bundled font files')
    if not a.hactool or not os.path.exists(a.hactool):
        fail('NO_HACTOOL', 'hactool was not found', 'It comes with this program: reinstall it, or pass --hactool.')
    if not os.path.isfile(a.keys):
        fail('NO_KEYS', 'prod.keys was not found', 'Choose your console\'s prod.keys (dumped with Lockpick_RCM).')
    if not os.path.isdir(a.firmware):
        fail('NO_FIRMWARE', 'The firmware folder was not found',
             'Choose the folder of your console\'s system NCAs, e.g. Ryujinx\'s bis/system/Contents/registered.')
    want = FONT_TITLES | OPTIONAL_FONT_TITLES | (set() if a.stub_mii else DATA_TITLES)
    found = {}
    ncas = sorted(n for n in os.listdir(a.firmware) if n.endswith('.nca'))
    if not ncas:
        fail('NO_FIRMWARE', 'The firmware folder has no NCA files',
             'Choose the folder of your console\'s system NCAs, e.g. Ryujinx\'s bis/system/Contents/registered '
             '(after installing your firmware dump in Ryujinx), or the registered folder of a NAND dump.')
    for i, name in enumerate(ncas):
        print(f'@progress {0.8 * i / len(ncas):.3f}', flush=True)
        p = nca_path(os.path.join(a.firmware, name))
        if not os.path.isfile(p) or os.path.getsize(p) < 0x8000:  # (the font archives are larger: skip tiny NCAs quickly)
            continue
        tid = title_id(a.hactool, a.keys, p)
        if tid in want:
            found[tid] = p
        if len(found) == len(want):
            break
    fonts = os.path.join(a.data, 'fonts')
    os.makedirs(fonts, exist_ok=True)
    nf = 0
    empty = {}
    for tid, p in sorted(found.items()):
        if tid in DATA_TITLES:
            out = os.path.join(a.data, 'sysdata', tid)
            if os.path.isdir(out):
                shutil.rmtree(out)
            os.makedirs(out)
            r = run([a.hactool, '-k', a.keys, '--disablekeywarns', '-t', 'nca', '--romfsdir', out, p])
            if not os.listdir(out):  # (read but nothing came out: the keys open the header, not the contents)
                shutil.rmtree(out)
                empty[tid] = ' / '.join([ln for ln in masked(r.stdout + r.stderr).splitlines() if ln.strip()][-2:])
            continue
        with tempfile.TemporaryDirectory() as tmp:
            run([a.hactool, '-k', a.keys, '--disablekeywarns', '-t', 'nca', '--romfsdir', tmp, p])
            for f in os.listdir(tmp):
                if f.endswith('.bfttf'):
                    shutil.copy(os.path.join(tmp, f), os.path.join(fonts, f))
                    nf += 1
    missing = sorted(want - set(found))
    with open(os.path.join(a.data, CJK_MARKER), 'w') as marker:
        marker.write('the Chinese and Korean shared fonts were looked for in the firmware'+'\n')
    print(f'sysdata: {nf} font files'
          + (f' (not in this firmware: {", ".join(missing)})' if missing else ''))
    print('@progress 1', flush=True)
    if not found:
        why = why_none()
        print(f'sysdata: {len(FAILS)} NCAs unreadable{why}', flush=True)
        if 'without a message' in why:
            fail('HACTOOL_CRASH', 'The firmware couldn\'t be read' + why,
                 'Please report this with the setup log. hactool, which comes with this program, didn\'t run '
                 'properly on this computer.')
        fail('KEYS', 'No system titles could be read from the firmware' + why,
             'prod.keys must be your console\'s and hold the keys for that firmware version (dump them again with a '
             'current Lockpick_RCM after updating). A newer firmware than this program\'s hactool knows can also do this.')
    if not a.stub_mii and (MII_MODEL not in found or MII_MODEL in empty):
        why = f' (hactool: {empty[MII_MODEL]})' if empty.get(MII_MODEL) else ''
        fail('MII_DATA', "The console's Mii data (0100000000000802) couldn't be read from the firmware" + why,
             "The game needs it to draw Miis. Choose the complete folder of your console's system NCAs and the prod.keys "
             'dumped from the same console after its last update (a current Lockpick_RCM).')
    if FONT_FILE not in os.listdir(fonts):
        fail('FONTS_MISSING', 'The console\'s button glyph font was not found in the firmware',
             'Choose the folder of your console\'s system NCAs (with the shared font title 0100000000000810).')


if __name__ == '__main__':
    main()
