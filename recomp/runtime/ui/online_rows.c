// The Online tab (runtime/ui/menu.cpp tab_online, runtime/ui/mods_page.cpp online_page): the one online service is
// OpenCourse World (games/smm2/mods/ocw.c), whose domain, address and token are the game's own rows
// (runtime/game_rows.h), drawn after anything here. Plain C: the same on every platform.
#include <SDL3/SDL.h>
#include <stdio.h>

bool online_rows_available(void) { return true; }

// Nothing of its own: OpenCourse World's rows come from the game (game_rows)
void online_rows(void) {}

// The game asked to go online and the runtime is offline (runtime/services/nifm.c): say why and what to do, at most
// every 20 s
void menu_notice(const char *text, double seconds);
void rt_log(const char *fmt, ...);
void online_offline_notice(void) {
    static Uint64 last;
    Uint64 now = SDL_GetTicks();
    if (last && now - last < 20000) return;
    last = now;
    const char *t = "You're offline. The game needs OpenCourse World, even for Course Practice: add your OpenCourse "
                    "World config (ocw-config.json) in the setup screen or the menu's Online tab, then restart the game.";
    rt_log("[online] notice: %s\n", t);
    menu_notice(t, 8);
}
