#!/usr/bin/env python3
"""Packages dist/ into a release zip that contains none of the player's own data.

Usage:  python recomp/tools/package_release.py v0.1.0
Output: dist/release/smm2-dx-windows-x64.zip
"""
import argparse
import fnmatch
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # repo root (this file is in recomp/tools)

# Left out: the player's extracted game, firmware-derived data, Nintendo fonts, saves, logs, caches, their paths.
# Only these top-level folders of smm2-dx_data are skipped: a bare name match would also drop every nested folder
# of that name (zig/lib/std/Build/Cache!).
# (mods-generated: files made at run time from the player's own game files, e.g. the patched font chain; mods: the mods they installed)
SKIP_DIRS = ("extracted", "cache", "work", "logs", "sysdata", "save", "mods-generated", "mods")
# (smm2.ini: their settings; ocw-maker.txt: their maker code; ocw-config.json: their token; .cjk-checked: setup's note that it
# has looked for the Chinese and Korean fonts in the player's firmware, which a fresh install must not start with)
SKIP_FILES = ("launcher.ini", "smm2.ini", "ocw-maker.txt", "ocw-boards.txt", "ocw-courses.bin", "ocw-config.json", ".cjk-checked", "playtime.txt", "character.txt", "history.txt", "*.bfttf", "*.log")

# Safety net: refuse to ship anything that looks like game or key material.
BAD_EXT = {".nca", ".nsp", ".xci", ".nso", ".bfttf", ".keys"}
BAD_NAMES = {"prod.keys", "title.keys", "launcher.ini", "smm2.ini", "ocw-maker.txt", "ocw-boards.txt", "ocw-config.json", ".cjk-checked", "playtime.txt", "character.txt", "history.txt"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("version", help="release version, e.g. v0.1.0")
    ap.parse_args()

    dist = ROOT / "dist"
    exe = dist / "smm2-dx-windows-x64.exe"
    data = dist / "smm2-dx_data"
    if not exe.exists() or not data.exists():
        sys.exit(f"Build first: {exe} or {data} is missing.")

    out = dist / "release"
    zip_path = out / "smm2-dx-windows-x64.zip"
    skip_dirs = {str(data / d).lower() for d in SKIP_DIRS}

    def ignore(directory, names):
        skipped = []
        for n in names:
            p = Path(directory) / n
            if p.is_dir():
                if str(p).lower() in skip_dirs:
                    skipped.append(n)
            elif any(fnmatch.fnmatch(n.lower(), pat) for pat in SKIP_FILES):
                skipped.append(n)
        return skipped

    with tempfile.TemporaryDirectory(prefix="smm2-dx-release-") as tmp:
        stage = Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        shutil.copy2(exe, stage / exe.name)
        shutil.copytree(data, stage / "smm2-dx_data", ignore=ignore)

        bad = [p for p in stage.rglob("*")
               if p.is_file() and (p.suffix.lower() in BAD_EXT or p.name.lower() in BAD_NAMES)]
        if bad:
            sys.exit("Refusing to package:\n" + "\n".join(str(p) for p in bad))

        zip_path.unlink(missing_ok=True)
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
            for p in sorted(stage.rglob("*")):
                z.write(p, p.relative_to(stage).as_posix())

    print(f"{zip_path} ({zip_path.stat().st_size / 1048576:.1f} MB)")


if __name__ == "__main__":
    main()
