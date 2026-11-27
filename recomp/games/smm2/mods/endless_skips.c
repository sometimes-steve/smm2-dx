// Endless skip counter (issue #32): counts the courses skipped in the Endless Challenge (runtime/ui/skips.c), for the Stats tab, and
// tells the menu layer when an Endless course is being played and how many skips this visit has had, for the Show Endless skips overlay
// (Extras page).
//
// Both come from steps of the game found by counting its function entries (fnprof, recomp/tools/autoplay.py):
//  - a skip runs main+0x12E05A0 once, and nothing else the game does does (a tap of Minus, Start Over, a clear, the first course of a run),
//  - an Endless course runs main+0x1BB9440 on every frame, and a course of Course World does not.
// recomp/tools/test_endless_skips.py checks that they still do.
#include <stdint.h>
#include <stdbool.h>

#include "mod.h"
#include "../../../runtime/ui/skips.h"

#define SKIP_STATE MAIN_ADDR(0x12E05A0)
#define ENDLESS_TICK MAIN_ADDR(0x1BB9440)
#define PLAY_INFO MAIN_ADDR(0x2c57d58)  // (mods/pc_editor.c: *(*+0x30): +0x10 kind, +0x14 what kind of course)

extern uint64_t renderer_present_count(void);

static volatile uint64_t g_tick_frame;  // the presented frame the Endless step last ran in (0: never)
static volatile uint32_t g_sub;         // the kind of course the Endless step ran in last (the info object's +0x14; 0: none yet): so an Endless course is known as soon as it plays
static volatile int g_visit_skips;      // skips since the Endless Challenge was entered: a new visit starts after a minute away from it
enum { VISIT_GAP = 3600 };              // (presented frames: a skip's loading is about ten seconds, the way out to the menus and back is longer)

void course_deaths_skip_noted(void);  // mods/course_deaths.c

static void endless_skip(Cpu *c) {
    skips_note();
    course_deaths_skip_noted();
    g_visit_skips++;
    LOG("[mod] endless skip: %d this session, %ld in all\n", skips_session(), skips_total());
    guest_call_orig(c, SKIP_STATE);
}

static void endless_tick(Cpu *c) {
    uint64_t now = renderer_present_count();
    if (!g_tick_frame || now - g_tick_frame > VISIT_GAP) g_visit_skips = 0;
    if (!g_tick_frame || now - g_tick_frame > 30) LOG("[mod] endless course: playing\n");
    g_tick_frame = now;
    uint64_t h = g64(PLAY_INFO), info = h ? g64(h + 0x30) : 0;
    if (info && g32(info + 0x10) == 2) g_sub = g32(info + 0x14);
    guest_call_orig(c, ENDLESS_TICK);
}

bool game_course_deaths(unsigned *deaths);  // mods/course_deaths.c: a course is being played

// runtime/game_hooks.c game_endless_playing: the step ran within the last few frames, or a course of the kind it last ran in is being played (the
// Show Endless skips overlay then shows with the deaths, from the course's first frame)
bool game_endless_playing(void) {
    uint64_t t = g_tick_frame;
    if (t && renderer_present_count() - t <= 5) return true;
    unsigned d;
    uint64_t h = g64(PLAY_INFO), info = h ? g64(h + 0x30) : 0;
    return g_sub && info && g32(info + 0x10) == 2 && g32(info + 0x14) == g_sub && game_course_deaths(&d);
}

// runtime/game_hooks.c game_endless_skips: the skips of this visit to the Endless Challenge (the Show Endless skips overlay)
int game_endless_skips(void) { return g_visit_skips; }

__attribute__((constructor)) static void endless_skips_register(void) {
    hle_override(SKIP_STATE, endless_skip, "mod: count Endless Challenge skips");
    hle_override(ENDLESS_TICK, endless_tick, "mod: know when an Endless Challenge course is played");
}
