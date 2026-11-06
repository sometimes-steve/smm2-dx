// Mod settings (games/smm2/mods/mod.h) and start-up: environment variables win over smm2mods.ini (key=value lines
// in the data folder, next to larecomp.ini). Nothing is on unless asked for.
//
//   RECOMP_MOD_LOG=1             log=1              log the mods' overrides
//
// Course World and the online backend are OpenCourse World's alone (ocw.c, runtime/net/netcfg.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mod.h"
#include "../../../runtime/game_rows.h"
#include "../../../runtime/net/netcfg.h"
#include "../../../runtime/net/online.h"

ModSettings g_mod;

static void set(const char *key, const char *val) {
    if (!strcmp(key, "log")) g_mod.log = atoi(val) != 0;
}

void mod_settings_load(void) {
    static int done;
    if (done) return;
    done = 1;
    FILE *f = fopen(getenv("RECOMP_MOD_INI") ? getenv("RECOMP_MOD_INI") : "smm2mods.ini", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '='), *e;
            if (line[0] == '#' || !eq) continue;
            *eq = 0;
            for (e = eq + 1 + strlen(eq + 1); e > eq + 1 && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); ) *--e = 0;
            set(line, eq + 1);
        }
        fclose(f);
    }
    if (getenv("RECOMP_MOD_LOG")) set("log", getenv("RECOMP_MOD_LOG"));
}

// The game's network session is its own (OpenCourse World): nothing is pretended unless a test asks for it
// (runtime/net/online.h).
bool net_pretend_online(void) { return getenv("RECOMP_PRETEND_ONLINE") && atoi(getenv("RECOMP_PRETEND_ONLINE")); }

// OpenCourse World's own section, with its domain and token (mods/ocw.c): rows rather than switches, so it goes through
// the GameModRow hook (runtime/game_rows.h)
int ocw_rows(GameModRow *out, int room);
void ocw_rows_changed(void);

// The game's rows in the menu (runtime/game_hooks.c): OpenCourse World's section.
int game_mod_rows(const GameModRow **out) {
    static GameModRow all[8];
    int n = ocw_rows(all, (int)(sizeof all / sizeof *all));
    *out = n ? all : NULL;
    return n;
}

void game_mod_rows_changed(void) { ocw_rows_changed(); }

int game_gamebanana_id(void) { return 7348; }  // (Super Mario Maker 2 on GameBanana: runtime/gamebanana.h)
