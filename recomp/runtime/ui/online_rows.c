// The Online tab (runtime/ui/menu.c tab_online, runtime/ui/mods_page.c online_page): the one online service is
// OpenCourse World (games/smm2/mods/ocw.c), whose domain and token are the game's own rows (runtime/game_rows.h), drawn
// after anything here. Here: a globe that shows whether the game is online, a button that picks the config file
// opencourse.world gives out, and the link to where it is had. Plain C.
#include "ui/ig.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "../game_rows.h"
#include "../settings.h"
#include "option_rows.h"

bool online_rows_available(void) { return true; }
void menu_notice(const char *text, double seconds);
SDL_Window *ui_window(void);  // runtime/nvn/vulkan/device.c

#define ACCOUNT_URL "https://opencourse.world/account"

// The globe: a circle with meridians that turn and a ring that pulses outwards while online (green); amber while a
// changed config waits for a restart; grey and still when there is no config yet.
static void globe(float r, int state, double t) {
    ImVec2 p;
    igGetCursorScreenPos(&p);
    ImDrawList *dl = igGetWindowDrawList();
    ImVec2 c = IM_VEC2(p.x + r * 1.4f, p.y + r * 1.4f);
    ImU32 col = state == 1 ? IM_COL32(64, 217, 102, 255) : state == 2 ? IM_COL32(240, 180, 50, 255) : IM_COL32(130, 134, 140, 255);
    ImU32 dim = (col & 0x00ffffffu) | (70u << 24), faint = (col & 0x00ffffffu) | (150u << 24);
    if (state == 1) {  // a ring going out from the globe, twice a second-ish
        float ph = (float)fmod(t * 0.7, 1.0);
        ImDrawList_AddCircle(dl, c, r * (1.05f + 0.35f * ph), (col & 0x00ffffffu) | ((unsigned)(150 * (1 - ph)) << 24), 0, 2.0f);
    }
    ImDrawList_AddCircleFilled(dl, c, r, (col & 0x00ffffffu) | (40u << 24), 0);
    float turn = state == 1 ? (float)(t * 0.9) : 0.5f;
    for (int k = 0; k < 3; k++) {  // meridians: ellipses as wide as the angle they face
        float rx = r * fabsf(cosf(turn + k * 1.0471976f));
        ImVec2 pts[41];
        for (int i = 0; i <= 40; i++) {
            float a = i * 6.2831853f / 40;
            pts[i] = IM_VEC2(c.x + rx * cosf(a), c.y + r * sinf(a));
        }
        ImDrawList_AddPolyline(dl, pts, 41, faint, 0, 1.6f);
    }
    for (int k = -1; k <= 1; k++) {  // the equator and two parallels
        float y = r * 0.55f * k, hw = sqrtf(r * r - y * y);
        ImDrawList_AddLine(dl, IM_VEC2(c.x - hw, c.y + y), IM_VEC2(c.x + hw, c.y + y), k ? dim : faint, 1.4f);
    }
    ImDrawList_AddCircle(dl, c, r, col, 0, 2.4f);
    float fs = igGetFontSize();
    const char *title = state == 1 ? "Online" : state == 2 ? "Restart to go online" : "Offline";
    const char *sub = state == 1 ? "Using your OpenCourse World config."
                    : state == 2 ? "Restart the game to use your new config."
                                 : "Add your config below, then restart.";
    float tx = p.x + r * 3.0f;
    ImDrawList_AddText_Vec2(dl, IM_VEC2(tx, c.y - fs * 1.15f), col, title, NULL);
    ImDrawList_AddText_Vec2(dl, IM_VEC2(tx, c.y + fs * 0.1f), IM_COL32(190, 190, 182, 255), sub, NULL);
    igDummy(IM_VEC2(1, r * 2.8f));
}

// The picked file arrives on whatever thread SDL's dialog uses: held here until the menu's own thread takes it
static char g_picked[2048];
static _Atomic bool g_picked_new;
static void SDLCALL picked(void *user, const char *const *files, int filter) {
    (void)user, (void)filter;
    if (!files || !files[0]) return;  // (cancelled, or an error)
    snprintf(g_picked, sizeof g_picked, "%s", files[0]);
    atomic_store(&g_picked_new, true);
}

void online_rows(void) {
    float fs = igGetFontSize();
    int state = game_online_state();
    if (atomic_exchange(&g_picked_new, false)) {
        if (game_online_import(g_picked)) menu_notice("OpenCourse World config loaded. Restart the game to go online "
                                                      "(Quit > Restart the Game).", 6);
        else menu_notice("That file isn't a usable OpenCourse World config. Download it again from "
                         "opencourse.world/account.", 6);
        state = game_online_state();
    }
    igDummy(IM_VEC2(1, fs * 0.3f));
    globe(fs * 1.6f, state, SDL_GetTicks() / 1000.0);
    {   // the player's own profile, once the maker code is known
        OnlineProfile pr;
        game_online_profile(&pr);
        opt_section("Your Profile");
        if (!pr.known) {
            opt_action("Maker Code", "Not known yet", "Learned the first time the game loads your profile (Course World, "
                       "or the title screen's My Info). Start the game online and it shows here.", true);
        } else {
            char v[64], d[160];
            if (pr.name[0]) opt_action("Name", pr.name, "Your name on OpenCourse World.", true);
            opt_action("Maker Code", pr.maker, "Your maker code. Others find you with it.", true);
            const char *none = pr.busy ? "Loading..." : "Unavailable";
            snprintf(d, sizeof d, "From OpenCourse World, about once a minute while this tab is open.");
            struct { const char *label; unsigned n; } st[] = {{"Courses Played", pr.played}, {"Courses Cleared", pr.cleared},
                                                              {"Attempts", pr.attempts}, {"Lives Lost", pr.lives_lost},
                                                              {"World Records", pr.world_records}, {"First Clears", pr.first_clears}};
            for (int i = 0; i < 6; i++) {
                if (pr.have_stats) snprintf(v, sizeof v, "%u", st[i].n);
                else snprintf(v, sizeof v, "%s", none);
                opt_action(st[i].label, v, d, true);
            }
        }
    }
    opt_section("Miis");
    if (opt_toggle("Fix Broken Miis", &g_settings.replace_bad_mii, true,
                   "Some OpenCourse World players have a blank or broken Mii, which shows up in the game wherever they "
                   "appear. On: it is replaced by a generated Mii that stays the same for that player until the app closes. Off: shown as "
                   "OpenCourse World sends it. Your own Mii is never changed. Applies to players the game loads from now "
                   "on.")) settings_save();
    opt_section("OpenCourse World Account");
    if (opt_action("Change Config File...", "", "Pick the ocw-config.json you downloaded from opencourse.world/account. "
                   "It replaces the domain and token below. The game uses it from the next start.", true)) {
        static const SDL_DialogFileFilter filter[] = {{"OpenCourse World config", "json"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(picked, NULL, ui_window(), filter, 2, NULL, false);
    }
    if (opt_action("Get Your Config", "opencourse.world/account", "Opens opencourse.world/account in your browser. Sign "
                   "in there to download your config file (ocw-config.json), then use Change Config File.", true))
        SDL_OpenURL(ACCOUNT_URL);
}

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
                    "World config (ocw-config.json) in the menu's Online tab, then restart the game (Quit > Restart the Game).";
    rt_log("[online] notice: %s\n", t);
    menu_notice(t, 8);
}
