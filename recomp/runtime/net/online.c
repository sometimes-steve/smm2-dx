// Default for net_pretend_online() (runtime/net/online.h): a game's mod code may define it instead.
#include "online.h"

#include <stdlib.h>

#include "netcfg.h"

__attribute__((weak)) bool net_pretend_online(void) {
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_PRETEND_ONLINE") && atoi(getenv("RECOMP_PRETEND_ONLINE")) != 0;
    return on;
}

// (the host not resolving - OpenCourse World down, or no internet - is offline: the game is told so and does not dial)
bool net_online(void) { return net_pretend_online() || (net_real_sockets() && net_ocw_reachable() != 0); }
