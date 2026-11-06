// Host side of the mod hook points (games/smm2/mods/patches.py): called right after the game's own instruction at
// each listed address. The worker request queue's (worker_guard.c) are the only ones.
#include "mod.h"

void smm2_mod_after(Cpu *c, uint64_t addr) {
    if (addr >= MAIN_ADDR(0x4d2d90) && addr <= MAIN_ADDR(0x4d41e8)) {  // the worker request queue (worker_guard.c)
        extern void worker_queue_after(Cpu *c, uint64_t addr);
        worker_queue_after(c, addr);
    }
}
