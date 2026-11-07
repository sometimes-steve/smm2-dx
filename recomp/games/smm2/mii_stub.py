"""A stand-in for the console's Mii face-part archive (system data 0100000000000802), made here, from nothing of Nintendo's.

The game's own code opens that archive at start-up and stops if it is missing. It holds six files of head shapes and face
textures: ShapeHigh.dat and ShapeMid.dat ("NFSR") and NXTexture{Low,Mid}{Linear,SRGB}.dat ("NFTR"). The game reads two of them
whole (ShapeHigh.dat and NXTextureMidLinear.dat) and takes the parts it draws out of them as it needs them.

These files have the console's layout - a header, a table of parts, then each part as a zlib stream - but every part is our own.
Every shape that is not the head is three vertices that cover nothing (so there are no eyes, eyebrows, mouth, nose or hair),
and every texture is an 8 x 8 block of white (the game multiplies it by each Mii's own skin colour). Whatever Mii a player or the server hands the game, it is drawn with the body,
clothes and shoes the game makes and the head chosen with --head (the game paints it in each Mii's own skin colour):
  cube   a cube: flat faces, sharp edges, a square seen from the front
  mini   (the default) the skin of the head of a Kenney "Mini Character" (CC0, mini_head.json): a rounded box with small ears
  round  our measured head outline with a haircut: a rounded cut with a jagged fringe and sideburns (hair colour is the Mii's)
  none   no head at all

    mii_stub.py --out <data folder>/sysdata/0100000000000802 [--head cube|mini|round|none]

SLOTS says which entries of each file's table are used (runs of (first word, stride in words, count)); the rest is computed here.
"""
import argparse
import json
import math
import os
import struct
import zlib

SHAPE_HEADER = 0x14DD4    # bytes of header and table in a shape file: where the first part's data starts
TEXTURE_HEADER = 0x2428   # ... in a texture file
SHAPE_ENTRY = 22          # words in a shape's table entry
TEXTURE_ENTRY = 7         # ... in a texture's
SHAPE_CATEGORIES, TEXTURE_CATEGORIES = 12, 11  # largest-part and alignment fields in the header's words 3.. and 15.. / 14..

SLOTS = {
    'ShapeHigh.dat': [(49, 22, 27), (841, 374, 2), (1391, 264, 2), (1897, 88, 2), (2337, 22, 2), (2491, 286, 2), (3041, 110, 2), (3415, 88, 2), (3745, 1650, 2), (5681, 726, 2), (6451, 22, 13), (6759, 22, 19), (7221, 22, 2), (7287, 22, 62), (8673, 22, 6), (8827, 22, 4), (8937, 22, 2), (9003, 22, 3), (9113, 22, 24), (9663, 22, 19), (10103, 22, 3), (10191, 22, 41), (11115, 22, 11), (11379, 22, 8), (11577, 22, 6), (11731, 22, 4), (11841, 22, 2), (11907, 22, 3), (12017, 22, 41), (12941, 22, 131), (15845, 22, 102), (18111, 44, 2), (18177, 22, 32)],
    'ShapeMid.dat': [(49, 22, 27), (841, 374, 2), (1391, 264, 2), (1897, 88, 2), (2337, 22, 2), (2491, 286, 2), (3041, 110, 2), (3415, 88, 2), (3745, 1650, 2), (5681, 726, 2), (6451, 22, 13), (6759, 22, 19), (7221, 22, 2), (7287, 22, 62), (8673, 22, 6), (8827, 22, 4), (8937, 22, 2), (9003, 22, 3), (9113, 22, 24), (9663, 22, 19), (10103, 22, 3), (10191, 22, 41), (11115, 22, 11), (11379, 22, 8), (11577, 22, 6), (11731, 22, 4), (11841, 22, 2), (11907, 22, 3), (12017, 22, 2), (12083, 22, 38), (12941, 22, 131), (15845, 22, 102), (18111, 44, 2), (18177, 22, 32)],
    'NXTexture': [(88, 119, 2), (263, 84, 2), (424, 28, 2), (564, 7, 2), (613, 91, 2), (788, 35, 2), (907, 28, 2), (949, 7, 85), (1551, 7, 2), (1572, 7, 11), (1656, 7, 11), (1740, 7, 19), (1880, 7, 38), (2153, 7, 23)],
}

TEXTURE_FILES = ('NXTextureLowLinear.dat', 'NXTextureLowSRGB.dat', 'NXTextureMidLinear.dat', 'NXTextureMidSRGB.dat')


def slots(name):
    out = []
    for first, stride, count in SLOTS[name]:
        out += [first + stride * i for i in range(count)]
    return out


# ---- The head -------------------------------------------------------------------------------------------------------------
# One closed shape with the outline of the console's default face (face shape 0). It takes the place of the face-shape slots (the
# 12 shapes after the first three), so whichever face shape a Mii has, it gets this one. x is the width, y goes up from the chin
# and z is depth (the front is +z).
#
# PROFILE is (height up from the chin, half the width, how far the front is in front of the middle), measured from the console's
# face piece: its cross-sections are ellipses, the front is nearly vertical and the sides stay about 26 out up to the forehead.
# That piece stops at a cut rim at the forehead (the hair covers what is above), so above it the skull is a round dome made here:
# an ellipse from the top of PROFILE up to HEAD_TOP. The back is 0.73 of the half width behind the middle (the piece's own back
# reaches 19.1 at the jaw).
HEAD_SLOTS = range(3, 15)
HEAD_MODE = 'mini'   # --head: 'cube', 'mini', 'round' (our measured head and haircut) or 'none'
# The cube is a little smaller than the console's head (52 wide, 44 deep) so that the game's hats, made for that head, do not
# cut through its vertical edges, which reach out further than its faces: 21 is half the side, the front is level with the
# console head's, and the cube stands on the neck.
CUBE_HALF, CUBE_Y0, CUBE_FRONT = 21.0, 0.0, 23.5
PROFILE = [(0.0, 6.0, 13.6), (2.5, 10.7, 16.6), (5.0, 13.1, 18.0), (7.5, 15.0, 19.4), (10.0, 16.9, 20.6),
           (12.5, 19.5, 21.8), (15.0, 20.6, 22.4), (17.5, 21.9, 23.1), (20.0, 23.2, 23.6), (22.5, 24.0, 24.1),
           (25.0, 24.8, 24.4), (27.5, 25.3, 24.6), (30.0, 25.6, 24.6), (32.5, 26.3, 24.6), (35.0, 26.3, 24.5),
           (40.0, 26.0, 24.5)]
HEAD_TOP = 65.4
BACK_RATIO = 0.73
DOME_RINGS, HEAD_SEGMENTS = 14, 32
TEXTURE_COLOUR = (255, 255, 255)    # the colour of every texture: white, so the game's own skin colour for each Mii (it multiplies) shows as it is


def head_rings():
    """(height, half width, front, back) of each ring, from the top (a point) down to the chin."""
    top_y, top_w, top_f = PROFILE[-1][0], PROFILE[-1][1], PROFILE[-1][2]
    rings = [(HEAD_TOP, 0.0, 0.0)]
    for k in range(DOME_RINGS, 0, -1):  # an ellipse from the end of the profile up to the top
        t = k / (DOME_RINGS + 1)
        c = math.sqrt(1 - t * t)
        rings.append((top_y + (HEAD_TOP - top_y) * t, top_w * c, top_f * c))
    rings += list(reversed(PROFILE))
    return [(y, w, f, w * BACK_RATIO) for y, w, f in rings]


def head_dims(y):
    """(half width, front, back) of the head at height y."""
    rs = head_rings()
    if y >= rs[0][0]:
        return rs[0][1:]
    for a, b in zip(rs, rs[1:]):
        if b[0] <= y <= a[0]:
            t = (y - b[0]) / (a[0] - b[0])
            return tuple(b[k] + (a[k] - b[k]) * t for k in (1, 2, 3))
    return rs[-1][1:]


def ring_point(phi, y, w, front, back, grow=0.0):
    """A point of the ring at height y, angle phi from the front (x = sin, z = cos), pushed out by grow."""
    c = math.cos(phi)
    return ((w + grow) * math.sin(phi), y, (front + grow) * c if c >= 0 else (back + grow) * c)


def grid_mesh(grid, centre):
    """(positions, normals, uvs, indices) of rows of points (top to bottom, each row a ring that wraps round): normals point
    away from centre."""
    seg, last = len(grid[0]), len(grid) - 1
    pos, nrm, uv = [], [], []
    for i in range(last + 1):
        for j in range(seg + 1):
            p = grid[i][j % seg]
            tu = [grid[i][(j + 1) % seg][k] - grid[i][(j - 1) % seg][k] for k in range(3)]
            tv = [grid[min(i + 1, last)][j % seg][k] - grid[max(i - 1, 0)][j % seg][k] for k in range(3)]
            n = (tu[1] * tv[2] - tu[2] * tv[1], tu[2] * tv[0] - tu[0] * tv[2], tu[0] * tv[1] - tu[1] * tv[0])
            m = math.sqrt(sum(x * x for x in n))
            if m < 1e-6:  # at a point where the ring shrinks to nothing
                n, m = (0.0, 1.0 if i == 0 else -1.0, 0.0), 1.0
            n = (n[0] / m, n[1] / m, n[2] / m)
            if sum(n[k] * (p[k] - centre[k]) for k in range(3)) < 0:
                n = (-n[0], -n[1], -n[2])
            pos.append(p)
            nrm.append(n)
            uv.append((j / seg, i / last))
    idx = []
    for i in range(last):
        for j in range(seg):
            a, b = i * (seg + 1) + j, (i + 1) * (seg + 1) + j
            idx += [a, b, a + 1, a + 1, b, b + 1]
    return pos, nrm, uv, idx


def cube_mesh():
    """(positions, normals, uvs, indices) of the cube: four vertices a face, so each face keeps its own flat normal."""
    h, y0, y1 = CUBE_HALF, CUBE_Y0, CUBE_Y0 + 2 * CUBE_HALF
    z0, z1 = CUBE_FRONT - 2 * h, CUBE_FRONT
    x0, x1 = -h, h
    faces = [  # (outward normal, four corners, counter-clockwise seen from outside)
        ((0, 0, 1), [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]),
        ((0, 0, -1), [(x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)]),
        ((1, 0, 0), [(x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)]),
        ((-1, 0, 0), [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)]),
        ((0, 1, 0), [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)]),
        ((0, -1, 0), [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)]),
    ]
    pos, nrm, uv, idx = [], [], [], []
    for n, c in faces:
        base = len(pos)
        pos += c
        nrm += [n] * 4
        uv += [(0, 1), (1, 1), (1, 0), (0, 0)]
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return pos, nrm, uv, idx


# The mini head: the skin of the head of one of Kenney's Mini Characters (CC0), stored in mini_head.json:
# a rounded box with small ears, no hair or face. It is scaled up and stands on the neck with its front where the cube's is.
MINI_SCALE, MINI_FRONT = 144.0, CUBE_FRONT   # the pack is in metres: 0.45 wide becomes 65
def mini_mesh():
    """(positions, normals, uvs, indices) of the mini head (the front is projected flat for the texture coordinates)."""
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mini_head.json')) as f:
        d = json.load(f)
    raw = d['positions']
    lo = [min(p[k] for p in raw) for k in range(3)]
    hi = [max(p[k] for p in raw) for k in range(3)]
    pos = [((p[0] - (lo[0] + hi[0]) / 2) * MINI_SCALE, (p[1] - lo[1]) * MINI_SCALE, (p[2] - hi[2]) * MINI_SCALE + MINI_FRONT) for p in raw]
    uv = [((p[0] - lo[0]) / (hi[0] - lo[0]), 1 - (p[1] - lo[1]) / (hi[1] - lo[1])) for p in raw]
    return pos, [tuple(n) for n in d['normals']], uv, d['indices']


def head_mesh():
    rings = head_rings()
    grid = [[ring_point(2 * math.pi * j / HEAD_SEGMENTS, y, w, f, b) for j in range(HEAD_SEGMENTS)] for y, w, f, b in rings]
    return grid_mesh(grid, (0.0, 25.0, 0.0))


# ---- The hair -------------------------------------------------------------------------------------------------------------
# One haircut: a shell over the top and back of the head, 3.6 thick at the crown and 2.2 at its edge (the head plus this makes the
# outline of the console's default Mii with its hair: 28.8 out at the sides and about 69 high). Its lower edge is a level run of
# teeth across the front (the fringe: tips low, gaps high), drops to sideburns at the ears and to the nape at the back.
# Every hair slot gets it, so whichever hair a Mii has, it wears this in its own hair colour.
HAIR_RUN, HAIR_COUNT = 12941, 131     # the hair shapes: HAIR_COUNT entries from this word
HAIR_SEG, HAIR_RINGS = 96, 30
HAIR_EDGE_THICK, HAIR_CROWN_THICK = 2.2, 1.4
FRINGE_BASE, FRINGE_AMP, FRINGE_TEETH = 49.0, 6.5, 5   # middle height of the fringe, half its tooth height, number of teeth
FRINGE_SPAN = math.radians(55)                         # the teeth reach this far round each side of the front
SIDE_EDGE, BACK_EDGE = 30.0, 24.0                      # height of the edge at the sideburns and at the nape


def smooth(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3 - 2 * t)


def edge_y(phi):
    """Height of the hair's lower edge at angle phi from the front."""
    a = abs((phi + math.pi) % (2 * math.pi) - math.pi)   # 0 at the front, pi at the back
    if a <= FRINGE_SPAN:
        wave = abs(((a / FRINGE_SPAN + 1) * FRINGE_TEETH / 2) % 1.0 * 2 - 1)   # 1 at a tip's centre... triangle wave in 0..1
        return FRINGE_BASE + FRINGE_AMP * (1 - 2 * wave)
    if a <= math.radians(95):
        return FRINGE_BASE + (SIDE_EDGE - FRINGE_BASE) * smooth((a - FRINGE_SPAN) / (math.radians(95) - FRINGE_SPAN))
    return SIDE_EDGE + (BACK_EDGE - SIDE_EDGE) * smooth((a - math.radians(95)) / (math.pi - math.radians(95)))


def haircut_mesh():
    grid = []
    for i in range(HAIR_RINGS + 1):
        s = i / HAIR_RINGS
        row = []
        for j in range(HAIR_SEG):
            phi = 2 * math.pi * j / HAIR_SEG
            y = HEAD_TOP - (HEAD_TOP - edge_y(phi)) * s ** 1.8          # (rings close together near the crown)
            w, f, b = head_dims(y)
            thick = HAIR_EDGE_THICK + HAIR_CROWN_THICK * (1 - s) ** 1.5
            lift = thick * (1 - s)                                       # the crown is raised the same all round
            q = min(1.0, w / 10.0)                                       # thickens sideways only where the head is wide
            row.append(ring_point(phi, y + lift, w, f, b, thick * q))
        grid.append(row)
    top = max(p[1] for p in grid[1])    # the top point sits level with the ring below it, so the crown has no tip
    grid[0] = [(0.0, top, 0.0)] * HAIR_SEG
    return grid_mesh(grid, (0.0, 25.0, 0.0))


# ---- The files ------------------------------------------------------------------------------------------------------------
def pack_snorm10(v):
    def c(x):
        return int(round(max(-1.0, min(1.0, x)) * 511)) & 0x3FF
    return c(v[0]) | c(v[1]) << 10 | c(v[2]) << 20


def part_from_mesh(pos, nrm, uv, idx):
    """A shape's data: positions (4 half floats a vertex), normals (10-10-10 signed, 4 bytes), texture coordinates (2 half
    floats), then 16-bit indices, each buffer aligned to 8; and the table entry's fields that describe them."""
    n = len(pos)
    blob = bytearray()
    for p in pos:
        blob += struct.pack('<4e', p[0], p[1], p[2], 1.0)
    off_n = len(blob)
    for v in nrm:
        blob += struct.pack('<I', pack_snorm10(v))
    blob += bytes(-len(blob) % 8)
    off_uv = len(blob)
    for t in uv:
        blob += struct.pack('<2e', t[0], t[1])
    blob += bytes(-len(blob) % 8)
    off_i = len(blob)
    blob += struct.pack('<%dH' % len(idx), *idx)
    box = [min(p[k] for p in pos) for k in range(3)] + [max(p[k] for p in pos) for k in range(3)]
    return bytes(blob), (n * 8, off_uv, off_n, n * 4, n * 4, off_i, len(idx) * 2), box


def empty_part():
    """A shape of three vertices at the origin: it covers nothing."""
    return part_from_mesh([(0.0, 0.0, 0.0)] * 3, [(0.0, 1.0, 0.0)] * 3, [(0.0, 0.0)] * 3, [0, 1, 2])


def texture_part():
    """An 8 x 8 block-compressed texture with 4 mip levels, 0x200 bytes each, in TEXTURE_COLOUR: every 8-byte block is two
    equal colours and indices that all pick the first."""
    r, g, b = TEXTURE_COLOUR
    c565 = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)
    return struct.pack('<HHI', c565, c565, 0) * (0x800 // 8)


def slot_kind(s):
    """What a table entry (the word it starts at) holds: 'head', 'hair' or 'empty'."""
    if HEAD_MODE == 'none':
        return 'empty'
    if s < 841 and (s - 49) // SHAPE_ENTRY in HEAD_SLOTS:
        return 'head'
    i, r = divmod(s - HAIR_RUN, SHAPE_ENTRY)
    if HEAD_MODE == 'round' and r == 0 and 0 <= i < HAIR_COUNT:
        return 'hair'
    return 'empty'


def make_shape_file(name):
    h = bytearray(SHAPE_HEADER)
    data = bytearray()
    parts = {}   # kind -> where its data is: the entries of a kind share one copy
    largest = 0
    for s in slots(name):
        key = slot_kind(s)
        if key not in parts:
            blob, fields, box = {'head': lambda: part_from_mesh(*{'cube': cube_mesh, 'mini': mini_mesh}.get(HEAD_MODE, head_mesh)()), 'hair': lambda: part_from_mesh(*haircut_mesh()),
                                 'empty': empty_part}[key]()
            comp = zlib.compress(blob, 9)
            parts[key] = (SHAPE_HEADER + len(data), blob, comp, fields, box)
            data += comp
        off, blob, comp, (pos, off_uv, off_n, a, b, off_i, idx), box = parts[key]
        entry = [off, len(blob), len(comp), 0x806, 0, pos, off_uv, 0, 0, off_n, a, b, 0, 0, off_i, idx]
        struct.pack_into('<16I', h, 4 * s, *entry)
        struct.pack_into('<6f', h, 4 * (s + 16), *box)
        largest = max(largest, len(blob))
    finish(h, b'NFSR', SHAPE_CATEGORIES, 3, 15, 4, largest)
    return bytes(h) + bytes(data)


def make_texture_file():
    h = bytearray(TEXTURE_HEADER)
    raw = texture_part()
    comp = zlib.compress(raw, 9)
    data = bytearray()
    for s in slots('NXTexture'):
        struct.pack_into('<7I', h, 4 * s, TEXTURE_HEADER + len(data), len(raw), len(comp), 0x806, 0x200, 0x80008, 0x403)
        data += comp
    finish(h, b'NFTR', TEXTURE_CATEGORIES, 3, 14, 0x200, len(raw))
    return bytes(h) + bytes(data)


def finish(h, magic, cats, first_max, first_align, align, largest):
    h[0:4] = magic
    for i in range(cats):  # the largest part of each category, and the alignment of its parts
        struct.pack_into('<I', h, 4 * (first_max + i), largest)
        struct.pack_into('<I', h, 4 * (first_align + i), align)


def write(out):
    os.makedirs(out, exist_ok=True)
    files = {n: make_shape_file(n) for n in ('ShapeHigh.dat', 'ShapeMid.dat')}
    tex = make_texture_file()
    files.update({n: tex for n in TEXTURE_FILES})
    for name, body in files.items():
        b = bytearray(body)
        struct.pack_into('<II', b, 4, 1, len(b))  # version 1, the file's size
        with open(os.path.join(out, name), 'wb') as f:
            f.write(b)
    return files


def main():
    ap = argparse.ArgumentParser(description="A stand-in for the console's Mii face-part archive")
    ap.add_argument('--out', required=True, help='sysdata/0100000000000802 in the data folder')
    ap.add_argument('--head', choices=('cube', 'mini', 'round', 'none'), default='mini', help='the head to draw (default: mini)')
    a = ap.parse_args()
    global HEAD_MODE
    HEAD_MODE = a.head
    files = write(a.out)
    print('mii_stub: wrote %d files (%d bytes) to %s' % (len(files), sum(len(v) for v in files.values()), a.out))


if __name__ == '__main__':
    main()
