// Calls into the recompiled game from a host override (games/smm2/mods/mod.h), and the network-session object's lock.
#include "mod.h"

#define NETOBJ_HOLDER MAIN_ADDR(0x2a37380) // + 0x3e0: the network-session object
#define WRAPPER_TRUE MAIN_ADDR(0x2a17ad8)  // the bytes the request functions return (success / failure)
#define LOCK_FN MAIN_ADDR(0x292410)        // the lock / unlock the wrappers take on NetObj + 0x20
#define UNLOCK_FN MAIN_ADDR(0x292430)

uint64_t netobj(void) { return g64(g64(NETOBJ_HOLDER) + 0x3e0); }
uint8_t wrapper_ok(void) { return g8(WRAPPER_TRUE); }

void guest_call_orig(Cpu *c, uint64_t addr) {
    GuestFn f = dispatch_lookup(addr);
    if (!f) FATAL("[mod] no recompiled function at %llx\n", (unsigned long long)addr);
    uint64_t lr = c->x[30];
    f(c);
    c->x[30] = lr;
}

void netobj_lock(Cpu *c) {
    uint64_t save0 = c->x[0];
    c->x[0] = netobj() + 0x20;
    guest_call(c, LOCK_FN);
    c->x[0] = save0;
}

void netobj_unlock(Cpu *c) {
    uint64_t save0 = c->x[0];
    c->x[0] = netobj() + 0x20;
    guest_call(c, UNLOCK_FN);
    c->x[0] = save0;
}
