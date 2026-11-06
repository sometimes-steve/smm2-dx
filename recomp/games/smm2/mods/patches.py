"""Mod hook points for the generator: host code after single instructions of SMM2 v3.0.3 (merged into
games/smm2/patches.py).

Each entry keeps the game's own instruction and then calls smm2_mod_after(c, address) (games/smm2/mods/hooks.c). The
host side decides, per address and per active mod, whether to change anything (a register, the flags, a store); with
every mod off the instruction behaves exactly as before. So the behaviour of a hook point can change without
recompiling; only adding or removing a point needs the generator again.

Each point is checked against the instruction word it expects, so another build of the game fails loudly instead of
being hooked in the wrong place. Offsets are from the start of `main` (the recomp loads it at 0x7000000000).
"""

MAIN = 0x7000000000

# offset in main -> (expected instruction word, what the host does there when the mod is on)
HOOK_POINTS = {
    # The game's worker request queue (main+0x4d2d80, after its prologue: x0 is the queue): a request whose object was
    # destroyed is made to fail instead of calling into it ("Pure virtual function called!"; worker_guard.c) ...
    0x4d2d90: (0x9100C3FD, 'worker queue: a destroyed object\'s request fails; the queue step takes the queue lock'),
    # ... and the queue's other changes, under one host lock (worker_guard.c): the queue has none of its own, and a
    # request added from the game thread while the worker steps the queue could be lost or a finished one come back
    # (each point: where the lock is taken - the count loaded again under it - or released)
    0x4d2db4: (0xB94002EB, 'worker queue step: the head request read (lock released unless it is a kind 3)'),
    0x4d2dec: (0xAA1303E0, 'worker queue step: kind 3 popped (lock released)'),
    0x4d2f0c: (0xB9407A68, 'worker queue step: count for the pop (lock)'),
    0x4d2f8c: (0xB9407A68, 'worker queue step: count for the pop (lock)'),
    0x4d2f2c: (0xA9437BFD, 'worker queue step: exit (lock released)'),
    0x4d32f8: (0xB9407B69, 'worker queue: a request added (lock)'),
    0x4d3330: (0x910083E0, 'worker queue: a request added (lock released)'),
    0x4d3720: (0xB9407809, 'worker queue: an attach request added (lock)'),
    0x4d3760: (0xA9000509, 'worker queue: an attach request added (lock released)'),
    0x4d3768: (0x2A1F03E0, 'worker queue: queue full (lock released)'),
    0x4d3fe4: (0xB9407A68, 'worker queue: kind 3 requeues (lock)'),
    0x4d4064: (0xA9457BFD, 'worker queue: kind 3 requeues (lock released)'),
    0x4d4134: (0xB9407A69, 'worker queue: an attach request added (lock)'),
    0x4d4170: (0xA9000109, 'worker queue: an attach request added (lock released)'),
    0x4d4184: (0x2A1F03E0, 'worker queue: queue full (lock released)'),
    0x4d41a0: (0xB9407809, 'worker queue: a detach request added (lock)'),
    0x4d41e0: (0xA9000509, 'worker queue: a detach request added (lock released)'),
    0x4d41e8: (0x2A1F03E0, 'worker queue: queue full (lock released)'),
}


def _hook(word):
    def patch(gen, addr):
        have = gen.words[(addr - gen.lo) // 4]
        if have != word:
            raise SystemExit(f'mods/patches.py: {addr:#x} is {have:#010x}, not {word:#010x} (not v3.0.3?)')
        orig = ' '.join(gen.render_single(gen.lift(addr), addr, None, set()))
        return f'{orig} {{ extern void smm2_mod_after(Cpu *, uint64_t); smm2_mod_after(c, {addr:#x}ull); }}'
    return patch


PATCHES = {MAIN + off: _hook(word) for off, (word, _) in HOOK_POINTS.items()}


# OpenCourse World (mods/ocw.c): the call that builds the WebSocket handshake's HTTP headers. The host builds them
# instead, so the player's OCW token rides along in one of them; this is the console mod's one code patch
# (_vendor/ocw-client-mod/source/program/main.cpp branch-links over the same instruction).
#
# Unlike the hook points above this one *replaces* the instruction rather than running after it, because the point is
# to do the call's job differently. With OCW off the host hook returns false and the game's own call is made exactly
# as before, through the recompiled function the instruction called (so no assumption is made about the format string
# or its arguments).
OCW_HEADER = 0xAF560
OCW_HEADER_WORD = 0x947020B4  # bl main+0x1cb7830 (the string formatting the header is built with)


def ocw_header(gen, addr):
    have = gen.words[(addr - gen.lo) // 4]
    if have != OCW_HEADER_WORD:
        raise SystemExit(f'mods/patches.py: {addr:#x} is {have:#010x}, not {OCW_HEADER_WORD:#010x} (not v3.0.3?)')
    ins = gen.lift(addr)
    if ins.kind != 'bl':
        raise SystemExit(f'mods/patches.py: {addr:#x} is {have:#010x}, not a call (not v3.0.3?)')
    return ('{ extern _Bool smm2_ocw_header(Cpu *); extern void guest_call_orig(Cpu *, uint64_t); '
            f'if (!smm2_ocw_header(c)) guest_call_orig(c, {ins.target:#x}ull); }}')


PATCHES[MAIN + OCW_HEADER] = ocw_header
