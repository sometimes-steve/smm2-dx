#!/usr/bin/env python3
"""Find where the running game keeps a value that changes with something the player does (e.g. which character is picked).

The guest's memory is the runtime's own address space (guest address == host address), so this reads it from outside with
ReadProcessMemory. Needs numpy. Work in states, changing only the thing looked for between them:

  memdiff.py snap                  full copy of the guest memory in the current state (A) -> memdiff.snap/
  memdiff.py diff  [max]           now in state B: keep every 32-bit word that differs from A and is small (0..max, default 7)
  memdiff.py keep  [same|other]    in another state: 'other' keeps words whose value differs from every state so far,
                                   'same' keeps words equal to the value they had the last time this state was seen
  memdiff.py show  [n]             the candidates left with their values per state

Candidates are stored in memdiff.cand.npz in the same folder (%TEMP%/memdiff, or MEMDIFF_DIR).
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys

import numpy as np

HERE = os.environ.get("MEMDIFF_DIR", os.path.join(os.environ.get("TEMP", "."), "memdiff"))
LO, HI = 0x7000000000, 0x7400000000
CHUNK = 16 << 20
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p), ("AllocationProtect", wt.DWORD),
                ("PartitionId", wt.WORD), ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD), ("Protect", wt.DWORD),
                ("Type", wt.DWORD)]


def pid():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq smm2-dx-runtime.exe", "/FO", "CSV", "/NH"], capture_output=True,
                         text=True).stdout
    for line in out.splitlines():
        parts = line.replace('"', "").split(",")
        if parts[0].lower() == "smm2-dx-runtime.exe":
            return int(parts[1])
    sys.exit("smm2-dx-runtime.exe is not running")


def regions(h):
    a = LO
    mbi = MBI()
    while a < HI:
        if not k32.VirtualQueryEx(h, ctypes.c_void_p(a), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        b, n = mbi.BaseAddress or 0, mbi.RegionSize
        if mbi.State == 0x1000 and mbi.Protect in (0x04, 0x08, 0x40, 0x80) and b + n > LO:  # committed and writable
            s = max(b, LO)
            yield s, min(b + n, HI) - s
        a = b + n


def read(h, addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)) or got.value != n:
        return None
    return np.frombuffer(buf.raw, dtype=np.uint32)


def pieces(h):
    for base, size in regions(h):
        for off in range(0, size, CHUNK):
            yield base + off, min(CHUNK, size - off)


def cmd_snap():
    h = k32.OpenProcess(0x0410, False, pid())
    os.makedirs(HERE, exist_ok=True)
    index = []
    with open(os.path.join(HERE, "a.bin"), "wb") as f:
        for addr, n in pieces(h):
            d = read(h, addr, n & ~3)
            if d is not None:
                f.write(d.tobytes())
                index.append((addr, len(d)))
    np.save(os.path.join(HERE, "a.idx.npy"), np.array(index, dtype=np.uint64))
    print("snapshot of", sum(n for _, n in index) * 4 >> 20, "MB in", len(index), "pieces")


def cmd_diff(mx):
    h = k32.OpenProcess(0x0410, False, pid())
    idx = np.load(os.path.join(HERE, "a.idx.npy"))
    snap = np.memmap(os.path.join(HERE, "a.bin"), dtype=np.uint32, mode="r")
    addrs, va, vb, pos = [], [], [], 0
    for addr, n in idx.tolist():
        a = snap[pos:pos + n]
        pos += n
        b = read(h, addr, n * 4)
        if b is None:
            continue
        m = (a != b) & (a <= mx) & (b <= mx)
        w = np.nonzero(m)[0]
        addrs.append(addr + w.astype(np.uint64) * 4), va.append(a[w]), vb.append(b[w])
    ad, va, vb = np.concatenate(addrs), np.concatenate(va), np.concatenate(vb)
    np.savez(os.path.join(HERE, "cand.npz"), addr=ad, vals=np.stack([va, vb], 1))
    print(len(ad), "candidates")


def cmd_keep(mode):
    h = k32.OpenProcess(0x0410, False, pid())
    z = np.load(os.path.join(HERE, "cand.npz"))
    ad, vals = z["addr"], z["vals"]
    cur = np.zeros(len(ad), dtype=np.uint32)
    for i, a in enumerate(ad.tolist()):
        d = read(h, a, 4)
        cur[i] = d[0] if d is not None else 0xFFFFFFFF
    if mode == "same":
        keep = cur == vals[:, -1]
    else:
        keep = np.all(vals != cur[:, None], axis=1) & (cur != 0xFFFFFFFF)
    vals = np.concatenate([vals, cur[:, None]], 1) if mode == "other" else vals
    np.savez(os.path.join(HERE, "cand.npz"), addr=ad[keep], vals=vals[keep])
    print(int(keep.sum()), "candidates left")


def cmd_show(n):
    z = np.load(os.path.join(HERE, "cand.npz"))
    for a, v in list(zip(z["addr"].tolist(), z["vals"].tolist()))[:n]:
        print(hex(a), v)


if __name__ == "__main__":
    c = sys.argv[1] if len(sys.argv) > 1 else ""
    arg = sys.argv[2] if len(sys.argv) > 2 else ""
    if c == "snap":
        cmd_snap()
    elif c == "diff":
        cmd_diff(int(arg or 7))
    elif c == "keep":
        cmd_keep(arg or "other")
    elif c == "show":
        cmd_show(int(arg or 40))
    else:
        print(__doc__)
