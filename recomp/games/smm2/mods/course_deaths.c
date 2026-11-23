// Personal death counter (issue #33): counts the player's deaths in each course (runtime/ui/deaths.c) and tells the menu layer how many the
// course being played has, for the Show course deaths text overlay (Extras page).
//
// Steps of the game found by counting its function entries (fnprof, recomp/tools/autoplay.py) and then hooking the candidates to see which
// scenario runs them (recomp/tools/test_course_deaths.py checks that a death is still counted):
//  - main+0x16D6CD0 runs once when a course is taken down: after a death, but also for Exit Course, a skip and a clear. (An earlier pick,
//    main+0x15E57A0, ran about a second after a course started and not for deaths: a counter built on it counted deaths that never happened.)
//    A death is told from the others by what follows: the same course is played again within ten seconds (a death retries it; a skip or a
//    clear brings another course; Exit Course goes to the menus). A death that ends an Endless run has no retry and is not counted.
//  - main+0x1BAB880 runs on every frame of a course being played (Endless or Course World), and not in the menus.
//  - the global main+0x2C4B710 points at the object of the course the game has loaded; its data id (the number OpenCourse World serves the
//    course under) is the u32 at +0x30.
#include <stdint.h>
#include <stdbool.h>

#include "mod.h"
#include "../../../runtime/ui/deaths.h"

#define TEARDOWN_STEP MAIN_ADDR(0x16D6CD0)
enum { RETRY_FRAMES = 600 };  // (a retry after a death starts about 300 presented frames after the course is taken down)
#define PLAY_TICK MAIN_ADDR(0x1BAB880)
#define COURSE_HOLDER MAIN_ADDR(0x2C4B710)
#define PLAY_INFO MAIN_ADDR(0x2c57d58)  // (mods/pc_editor.c: *(*+0x30): +0x10 kind, 0 = the Course Maker)

extern uint64_t renderer_present_count(void);

static volatile uint64_t g_tick_frame;  // the presented frame a course was last seen being played in (0: never)

static bool in_maker(void) {
    uint64_t holder = g64(PLAY_INFO), info = holder ? g64(holder + 0x30) : 0;
    return info && g32(info + 0x10) == 0;  // (a test play in the Course Maker is no course of the player's)
}

static uint32_t current_course(void) {
    uint64_t holder = g64(COURSE_HOLDER);
    return holder ? g32(holder + 0x30) : 0;
}

static uint32_t g_down_course;           // the course that was taken down last (0: none waiting for a retry)
static uint64_t g_down_frame;

static void teardown_step(Cpu *c) {
    bool maker = in_maker();
    g_down_course = maker ? 0 : current_course();
    g_down_frame = renderer_present_count();
    LOG("[mod] course taken down: %u, maker %d (frame %llu)\n", current_course(), maker, (unsigned long long)g_down_frame);
    guest_call_orig(c, TEARDOWN_STEP);
}

static void play_tick(Cpu *c) {
    uint64_t now = renderer_present_count();
    uint64_t gap = g_tick_frame ? now - g_tick_frame : 0;
    if (g_down_course && gap > 3) LOG("[mod] ticks paused %llu frames, %llu after the take-down\n", (unsigned long long)gap, (unsigned long long)(now - g_down_frame));
    // (a course starts being played; or, right after one was taken down, the ticks come back after a short pause: some courses retry that way)
    if (!g_tick_frame || gap > 30 || (g_down_course && gap > 3 && now - g_down_frame <= RETRY_FRAMES)) {
        uint32_t id = current_course();
        LOG("[mod] course playing: %u (frame %llu, waiting for a retry of %u, down %llu frames ago)\n", id, (unsigned long long)now, g_down_course,
            (unsigned long long)(g_down_course ? now - g_down_frame : 0));
        if (g_down_course && g_down_course == id && now - g_down_frame <= RETRY_FRAMES) {  // (the same course again, soon: Mario died)
            deaths_add(id);
            LOG("[mod] course death: course %u, %u in all (frame %llu)\n", id, deaths_get(id), (unsigned long long)now);
        }
        g_down_course = 0;
    }
    g_tick_frame = now;
    guest_call_orig(c, PLAY_TICK);
}

// runtime/game_hooks.c game_course_deaths: a course is being played (the step ran within the last few frames): its deaths so far
bool game_course_deaths(unsigned *deaths) {
    uint64_t t = g_tick_frame;
    if (!t || renderer_present_count() - t > 5 || in_maker()) return false;
    uint32_t id = current_course();
    if (!id) return false;
    *deaths = deaths_get(id);
    static uint32_t last_id = ~0u, last_n = ~0u;
    if (id != last_id || *deaths != last_n) {
        LOG("[mod] course deaths shown: course %u, %u\n", id, *deaths);
        last_id = id, last_n = *deaths;
    }
    return true;
}

__attribute__((constructor)) static void course_deaths_register(void) {
    hle_override(TEARDOWN_STEP, teardown_step, "mod: know when a course is taken down (a death retries it)");
    hle_override(PLAY_TICK, play_tick, "mod: know when a course is being played");
}
