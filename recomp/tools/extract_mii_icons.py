#!/usr/bin/env python3
"""Unpacks the Mii part icons (hair, eyes, mouth ...) from the player's own firmware to PNG files.

Usage:  python recomp/tools/extract_mii_icons.py --firmware <registered NCA folder> --keys <prod.keys> --out <folder>
                                                [--hactool PATH] [--jobs N]

The Mii editor applet (title 0100000000001009) keeps one texture atlas, lyt/Texture/IconAll.bntx: a 78 x 66 picture for
every choice in the editor (IconHair000, IconEye08 ...). They are read from the player's firmware and written to
<out>/<name>.png, so nothing of Nintendo's is shipped with the program (the folder stays with the player's data).
Keys are only handed to hactool and read in place; its output is never shown (it can echo key values).

A BNTX texture (Switch): its header says the size and format; the pixels sit in the GPU's block-linear order and are put
back in rows here. Two formats are in this atlas: R8G8B8A8 and BC5 (two channels, 4 x 4 blocks of two BC4 each: masks, painted in one colour, see TINTS).
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from concurrent.futures import ThreadPoolExecutor

APPLET = '0100000000001009'  # miiEdit
ATLAS = os.path.join('lyt', 'Texture', 'IconAll.bntx')

FMT_RGBA8 = 0x0b01
FMT_BC5 = 0x1e01


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, errors='replace')


def nca_path(entry):
    return os.path.join(entry, '00') if os.path.isdir(entry) else entry  # (Ryujinx keeps each NCA as <id>.nca/00)


def find_applet(hactool, keys, firmware, jobs):
    """The NCA of the applet's program: the one whose Title ID is APPLET (and that holds its files, i.e. is not a .cnmt)."""
    def check(name):
        p = nca_path(os.path.join(firmware, name))
        if not os.path.isfile(p) or os.path.getsize(p) < 0x8000:
            return None
        out = run([hactool, '-k', keys, '--disablekeywarns', '-t', 'nca', p]).stdout
        tid = ct = ''
        for line in out.splitlines():
            line = line.strip()
            if line.startswith('Title ID:'):
                tid = line.split(':', 1)[1].strip().lower()
            elif line.startswith('Content Type:'):
                ct = line.split(':', 1)[1].strip()
        return p if tid == APPLET and ct == 'Program' else None
    names = sorted(n for n in os.listdir(firmware) if n.endswith('.nca') and not n.endswith('.cnmt.nca'))
    with ThreadPoolExecutor(jobs) as ex:
        for p in ex.map(check, names):
            if p:
                return p
    return None


# ---------------------------------------------------------------- BNTX

def blocklinear_offset(x, y, width_gobs, bpp, block_height):
    """Byte offset of the element at (x, y) of a block-linear surface (the Tegra X1's order: 64 x 8 byte GOBs in stacks of
    block_height)."""
    xb = x * bpp
    gob = ((y // (8 * block_height)) * 512 * block_height * width_gobs + (xb // 64) * 512 * block_height
           + ((y % (8 * block_height)) // 8) * 512)
    xb %= 64
    y %= 8
    return gob + ((xb % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((xb % 32) // 16) * 32 + (y % 2) * 16 + (xb % 16)


def deswizzle(data, w, h, bpp, block_height):
    """The w x h elements of bpp bytes each, in rows."""
    pitch = (w * bpp + 63) // 64 * 64
    gobs = pitch // 64
    out = bytearray(w * h * bpp)
    for y in range(h):
        for x in range(w):
            o = blocklinear_offset(x, y, gobs, bpp, block_height)
            out[(y * w + x) * bpp:(y * w + x + 1) * bpp] = data[o:o + bpp]
    return bytes(out)


def bc4_block(b):
    """16 bytes of one channel (a 4 x 4 block) out of 8 BC4 bytes."""
    r0, r1 = b[0], b[1]
    if r0 > r1:
        pal = [r0, r1] + [((7 - i) * r0 + i * r1) // 7 for i in range(1, 7)]
    else:
        pal = [r0, r1] + [((5 - i) * r0 + i * r1) // 5 for i in range(1, 5)] + [0, 255]
    bits = int.from_bytes(b[2:8], 'little')
    return [pal[(bits >> (3 * i)) & 7] for i in range(16)]


def decode_bc5(data, w, h):
    """BC5 -> (R, G) planes, w x h each (the blocks come in rows, one 16-byte block each)."""
    bw, bh = (w + 3) // 4, (h + 3) // 4
    blocks = data  # (already in rows: texture_rgba puts them so)
    rp, gp = bytearray(w * h), bytearray(w * h)
    for by in range(bh):
        for bx in range(bw):
            o = (by * bw + bx) * 16
            r, g = bc4_block(blocks[o:o + 8]), bc4_block(blocks[o + 8:o + 16])
            for i in range(16):
                x, y = bx * 4 + i % 4, by * 4 + i // 4
                if x < w and y < h:
                    rp[y * w + x], gp[y * w + x] = r[i], g[i]
    return rp, gp


def png(path, w, h, rgba):
    def chunk(tag, body):
        c = struct.pack('>I', len(body)) + tag + body
        return c + struct.pack('>I', zlib.crc32(tag + body) & 0xffffffff)
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def textures(d):
    """(name, format, width, height, block_height, mip 0's bytes) of every texture of a BNTX file."""
    if d[:4] != b'BNTX':
        raise ValueError('not a BNTX file')
    for m in re.finditer(b'BRTI', d):
        o = m.start()
        fmt, w, h = struct.unpack_from('<III', d, o + 0x1c)[0], *struct.unpack_from('<II', d, o + 0x24)
        block_height = 1 << struct.unpack_from('<I', d, o + 0x34)[0]
        size = struct.unpack_from('<I', d, o + 0x50)[0]
        name_at = struct.unpack_from('<Q', d, o + 0x60)[0]
        name = d[name_at + 2:name_at + 2 + struct.unpack_from('<H', d, name_at)[0]].decode('ascii', 'replace')
        mips = struct.unpack_from('<Q', d, o + 0x70)[0]
        base = struct.unpack_from('<Q', d, mips)[0]
        yield name, fmt, w, h, block_height, d[base:base + size]


# The BC5 icons are masks, not pictures: two channels, each a region of the part (the editor paints them with the part's colours).
# They are painted here in one fixed colour per kind of part, so the picker shows the shape: the second channel is the part's
# shape (its opacity); the first is not used.
TINTS = (('IconHair', (84, 56, 36)), ('IconEyebrow', (84, 56, 36)), ('IconFaceline', (244, 196, 156)), ('IconNose', (150, 96, 72)))
# What is kept: the icons that show their part. The others of the atlas (glasses, beard, mustache, mole) are plain discs once
# unmasked (their shape is not in these textures), so the editor shows those as numbers.
KEEP = ('IconHair', 'IconEye', 'IconEyebrow', 'IconNose', 'IconMouth', 'IconFaceline', 'IconFaceT_line', 'IconFaceT_make')


def recolor(px, mouth):
    """The eye and mouth icons come with placeholder colours for the editor to replace (eyes: a blue iris, green highlights;
    mouths: green upper lip, red lower): painted here in fixed ones."""
    for i in range(0, len(px), 4):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if b > 200 and r < 60 and g < 60:
            px[i:i + 3] = bytes((60, 40, 30))
        elif g > 200 and r < 80 and b < 80:
            px[i:i + 3] = bytes((236, 140, 144) if mouth else (255, 255, 255))
        elif mouth and r > 200 and g < 60 and b < 60:
            px[i:i + 3] = bytes((208, 88, 100))


def texture_rgba(fmt, w, h, block_height, data, name=''):
    """The picture as RGBA bytes."""
    if fmt == FMT_RGBA8:
        px = bytearray(deswizzle(data, w, h, 4, block_height))
        if name.startswith(('IconEye', 'IconMouth')) and not name.startswith('IconEyebrow'):
            recolor(px, name.startswith('IconMouth'))
        return bytes(px)
    if fmt == FMT_BC5:
        bw, bh = (w + 3) // 4, (h + 3) // 4
        blocks = deswizzle(data, bw, bh, 16, block_height)
        r, g = decode_bc5(blocks, w, h)
        out = bytearray(w * h * 4)
        tint = next((t for p, t in TINTS if name.startswith(p)), (96, 96, 96))
        for i in range(w * h):
            out[i * 4:i * 4 + 4] = bytes((tint[0], tint[1], tint[2], g[i]))
        return bytes(out)
    raise ValueError(f'format {fmt:#x}')


def extract(hactool, keys, nca, out):
    """Writes the icons of the Mii editor's NCA to <out>/<name>.png; how many. ValueError: the editor was read but has no atlas."""
    with tempfile.TemporaryDirectory() as tmp:
        run([hactool, '-k', keys, '--disablekeywarns', '-t', 'nca', '--romfsdir', tmp, nca])
        atlas = os.path.join(tmp, ATLAS)
        if not os.path.isfile(atlas):
            raise ValueError('the editor was read but has no icon atlas (the keys open the header, not the contents?)')
        with open(atlas, 'rb') as f:
            d = f.read()
    os.makedirs(out, exist_ok=True)
    n = 0
    for name, fmt, w, h, bh, data in textures(d):
        if not name.startswith(KEEP):
            continue
        png(os.path.join(out, name + '.png'), w, h, texture_rgba(fmt, w, h, bh, data, name))
        n += 1
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--firmware', required=True)
    ap.add_argument('--keys', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--hactool', default=shutil.which('hactool'))
    ap.add_argument('--jobs', type=int, default=8)
    a = ap.parse_args()
    if not a.hactool or not os.path.exists(a.hactool):
        sys.exit('extract_mii_icons: hactool was not found (--hactool)')
    if not os.path.isfile(a.keys) or not os.path.isdir(a.firmware):
        sys.exit('extract_mii_icons: --keys is not a file, or --firmware not a folder')
    nca = find_applet(a.hactool, a.keys, a.firmware, a.jobs)
    if not nca:
        sys.exit(f"extract_mii_icons: the Mii editor ({APPLET}) is not in this firmware")
    try:
        n = extract(a.hactool, a.keys, nca, a.out)
    except ValueError as e:
        sys.exit(f'extract_mii_icons: {e}')
    print(f'extract_mii_icons: {n} icons written to {a.out}')


if __name__ == '__main__':
    main()
