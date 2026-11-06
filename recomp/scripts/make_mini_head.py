#!/usr/bin/env python3
"""Make games/smm2/mini_head.json: the head of one of Kenney's "Mini Characters" (3D, CC0), for mii_stub.py --head mini.

Only the skin is kept. The pack's head is one mesh with the hair, eyes and mouth in it, each a patch of the pack's colour map; the
triangles whose colour is not skin (the dark eyes and mouth, the hair) are dropped, and the rest is written as plain lists. The
game paints the head in each Mii's skin colour, so the colours and texture coordinates are not kept either.

    make_mini_head.py <character-male-d.glb> <Textures/colormap.png> [--out games/smm2/mini_head.json]

The pack: https://kenney.nl/assets/mini-characters (Creative Commons Zero). Needs numpy and Pillow; run it with python -I.
"""
import argparse
import json
import struct
from pathlib import Path

import numpy as np
from PIL import Image

SKIN_MIN_GR = 0.645      # a skin colour has at least this much green for its red (hair is more orange)
MIN_LUMA = 120           # the eyes and mouth are dark


def read_glb(path):
    d = Path(path).read_bytes()
    n = struct.unpack('<I', d[12:16])[0]
    j = json.loads(d[20:20 + n])
    binary = d[20 + n + 8:]
    return j, binary


def accessor(j, binary, i, comp, dtype):
    a = j['accessors'][i]
    v = j['bufferViews'][a['bufferView']]
    off = v.get('byteOffset', 0) + a.get('byteOffset', 0)
    arr = np.frombuffer(binary, dtype=dtype, count=a['count'] * comp, offset=off)
    return arr.reshape(a['count'], comp) if comp > 1 else arr


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('glb')
    ap.add_argument('colormap')
    ap.add_argument('--out', default=str(Path(__file__).resolve().parent.parent / 'games' / 'smm2' / 'mini_head.json'))
    a = ap.parse_args()

    j, binary = read_glb(a.glb)
    node = next(n for n in j['nodes'] if n.get('name') == 'head-mesh')
    prim = j['meshes'][node['mesh']]['primitives'][0]
    pos = accessor(j, binary, prim['attributes']['POSITION'], 3, '<f4')
    nrm = accessor(j, binary, prim['attributes']['NORMAL'], 3, '<f4')
    uv = accessor(j, binary, prim['attributes']['TEXCOORD_0'], 2, '<f4')
    idx_acc = j['accessors'][prim['indices']]
    tri = accessor(j, binary, prim['indices'], 1, '<u2' if idx_acc['componentType'] == 5123 else '<u4').reshape(-1, 3)
    tex = np.array(Image.open(a.colormap).convert('RGB'))

    keep = []
    for t in tri:
        u, v = uv[t].mean(0)
        r, g, b = (int(x) for x in tex[min(int(v * tex.shape[0]), tex.shape[0] - 1), min(int(u * tex.shape[1]), tex.shape[1] - 1)])
        if g / r >= SKIN_MIN_GR and 0.299 * r + 0.587 * g + 0.114 * b >= MIN_LUMA:
            keep.append(t)
    keep = np.array(keep)
    used = np.unique(keep)
    remap = {int(o): i for i, o in enumerate(used)}
    out = {
        'source': 'Kenney Mini Characters (CC0, https://kenney.nl/assets/mini-characters), head-mesh of ' + Path(a.glb).name + ', skin triangles only',
        'positions': [[round(float(x), 5) for x in pos[i]] for i in used],
        'normals': [[round(float(x), 4) for x in nrm[i]] for i in used],
        'indices': [remap[int(i)] for i in keep.reshape(-1)],
    }
    Path(a.out).write_text(json.dumps(out, separators=(',', ':')) + '\n')
    p = pos[used]
    print('kept %d of %d triangles, %d vertices; size %s' % (len(keep), len(tri), len(used), (p.max(0) - p.min(0)).round(4)))


if __name__ == '__main__':
    main()
