// Boot intro skip: skip the Mario logo presentation animation directly to the title screen.
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>
#include <math.h>

#include "mod.h"
#include "../../../runtime/settings.h"

#define STATE_CHANGE MAIN_ADDR(0x8b9320)

extern uint64_t renderer_present_count(void);
extern uint64_t g_test_buttons, g_test_buttons_until;  // runtime/game_hooks.c, read by runtime/hle/input.c

static volatile uint64_t g_boot_hook_at, g_boot_hook_first, g_boot_state;  // the logo's hooks last ran / first ran / state
static void boot_note(Cpu *c) {
    uint64_t now = renderer_present_count();
    g_boot_hook_at = now;
    if (!g_boot_hook_first) g_boot_hook_first = now;
    g_boot_state = g32(c->x[0] + 0x240);
}
static void boot_cappear(Cpu *c) {
    boot_note(c);
    if (!g_settings.skip_intro || g_settings.skip_mode_select) {  // (the two aren't used together: mode select wins)
        guest_call_orig(c, MAIN_ADDR(0x17E4340));
        return;
    }
    uint64_t seq = c->x[0];
    uint32_t step = g32(seq + 0x240);
    if (step == 0) {
        guest_call_orig(c, MAIN_ADDR(0x17E4340));
        s8(seq + 0x28d, 1);
        c->x[0] = seq + 0x238;
        c->x[1] = 4;
        guest_call(c, STATE_CHANGE);
    } else {
        c->x[0] = seq + 0x238;
        c->x[1] = 4;
        guest_call(c, STATE_CHANGE);
    }
    c->x[0] = 0;
}

static void boot_cinmain(Cpu *c) {
    boot_note(c);
    if (!g_settings.skip_intro || g_settings.skip_mode_select) {  // (the two aren't used together: mode select wins)
        guest_call_orig(c, MAIN_ADDR(0x17E49F0));
        return;
    }
    uint64_t seq = c->x[0];
    uint32_t step = g32(seq + 0x240);
    if (step == 0) {
        guest_call_orig(c, MAIN_ADDR(0x17E49F0));
        s8(seq + 0x28d, 1);
        c->x[0] = seq + 0x238;
        c->x[1] = 4;
        guest_call(c, STATE_CHANGE);
    } else {
        c->x[0] = seq + 0x238;
        c->x[1] = 4;
        guest_call(c, STATE_CHANGE);
    }
    c->x[0] = 0;
}

// ---- Skip mode select: from the title screen straight into Course World, as a player would go: L+R opens the Make /
// Play menu, then Play, then Course World. The presses go through the game's test-button injection
// (g_test_buttons), so the game sees an ordinary controller. Each press waits for the scene manager to be quiet
// (no scene change under way) and for the screen to have settled, so animations and fades never eat a press.
#define SCENE_MGR MAIN_ADDR(0x2a390d8)  // *: the scene manager; +0x33C target scene (u32), +0x344 change under way
enum { BTN_A = 1 << 0, BTN_L = 1 << 6, BTN_R = 1 << 7, BTN_LEFT = 1 << 12, BTN_UP = 1 << 13, BTN_RIGHT = 1 << 14,
       BTN_DOWN = 1 << 15 };  // nn::hid button bits (runtime/hle/input.c)

typedef struct { uint64_t buttons; int settle; const char *what; } MsStep;
static const MsStep g_ms_steps[] = {
    {BTN_L | BTN_R, 0,   "L+R on the title screen"},
    {BTN_RIGHT,     200, "Play"},
    {BTN_A,         60,  "open Play"},
    {BTN_DOWN,      300, "Course World"},
    {BTN_A,         60,  "open Course World"},
};

extern _Atomic int g_present_hidden;  // runtime/nvn/objects.c: frames run unpaced and unseen (counts down per frame)
extern volatile int g_audio_muted;  // runtime/services/audio.c
extern uint64_t input_player_buttons(void);  // runtime/hle/input.c: the player's own buttons

static double ms_wall(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

// ---- the loading screen shown while the sequence runs (runtime/game_hooks.c game_splash): the frames are fast-forwarded
// and unseen, so the window would be black. The bar goes from the start of the game to Course World.
static _Atomic bool g_splash;
static _Atomic float g_splash_progress;
static const char *volatile g_splash_what = "Starting the game";

bool game_splash(float *progress, const char **what) {
    if (!atomic_load(&g_splash)) return false;
    *progress = atomic_load(&g_splash_progress);
    *what = g_splash_what;
    return true;
}

// An in-place reboot (Restart the Game, runtime/main.c) starts the sequence again, with the setting as it is by then:
// the reset hook bumps the epoch, the driver sees it once the reboot is done and starts over (the boot logo's hooks
// and the scene manager are the new boot's; the presented-frame count carries on across boots, so each boot's frames
// are counted from where it began).
extern int rt_reboot_requested(void);  // runtime/main.c: set from the request until the game is torn down
static _Atomic unsigned g_ms_epoch;
static _Atomic int g_ms_state;  // Skip mode select this boot: 0 not decided yet, 1 running, 2 done (or off)
RT_ON_RESET(skip_intro_reset) {
    g_boot_hook_at = g_boot_hook_first = g_boot_state = 0;
    atomic_fetch_add(&g_ms_epoch, 1);
    atomic_store(&g_ms_state, 0);
}

// The game is still launching with its intro skipped (runtime/game_hooks.c game_launch_skipping): Skip mode select
// until its sequence is over, Skip intro until the title screen is up (the boot logo's hooks have gone quiet).
bool game_launch_skipping(void) {
    int st = atomic_load(&g_ms_state);
    if (st == 0 ? g_settings.skip_mode_select : st == 1) return true;
    if (!g_settings.skip_intro || g_settings.skip_mode_select) return false;  // (mode select replaces Skip intro)
    bool title_up = g_boot_hook_first && renderer_present_count() - g_boot_hook_at >= 30;
    return !title_up;
}

// Hardened against slow starts: a press only goes out once the game has run at a steady frame rate for 2 s (first-run
// shader compiles and asset loads stall the frames, and a press during a stall is lost), the settle delays count in
// presented frames (so they stretch with a slow machine), and the player's own buttons cancel the sequence.
static void *ms_driver(void *arg) {
    (void)arg;
    uint64_t next_at = 0, win_frames = 0, steady_since = 0;
    double win_t = 0;
    size_t step = 0;
    bool latched = false, enabled = false, finished = false;
    unsigned epoch = atomic_load(&g_ms_epoch);
    uint64_t base = 0;  // (the frame this boot began at)
    bool logging = getenv("RECOMP_SKIP_MODE_SELECT_LOG") != NULL, started = false;
    uint64_t last_target = ~0ull, done_at = 0;
    double t_start = 0;
    uint64_t g_ms_logged_at = 0;
    for (;;) {
        usleep(20000);
        if (rt_reboot_requested()) continue;  // (the game is being torn down: its memory is about to be wiped)
        uint64_t now = renderer_present_count();
        if (epoch != atomic_load(&g_ms_epoch)) {  // (a new boot: start over)
            epoch = atomic_load(&g_ms_epoch);
            next_at = win_frames = steady_since = done_at = 0;
            win_t = t_start = 0;
            step = 0, last_target = ~0ull, base = now;
            latched = enabled = finished = started = false;
            g_test_buttons = 0;
            atomic_store(&g_present_hidden, 0), g_audio_muted = 0, atomic_store(&g_splash, false);
        }
        if (logging && now != g_ms_logged_at && now % 60 == 0) {
            g_ms_logged_at = now;
            LOG("[mod] skip mode select: (diag) frame %llu hooks first %llu last %llu state %llu\n", (unsigned long long)now,
                (unsigned long long)g_boot_hook_first, (unsigned long long)g_boot_hook_at, (unsigned long long)g_boot_state);
        }
        uint64_t s = g64(SCENE_MGR);
        if (!s) continue;
        // (the setting counts as it was when the game booted: turning it on in the menu does nothing until the next
        // start, and turning it off mid-sequence lets the sequence finish)
        if (!latched) latched = true, enabled = g_settings.skip_mode_select, atomic_store(&g_ms_state, enabled ? 1 : 2);
        if (!enabled || finished) continue;
        uint32_t target = g32(s + 0x33c);
        if (logging && target != last_target) LOG("[mod] skip mode select: scene 0x%x at frame %llu\n", target, (unsigned long long)now);
        last_target = target;
        // The presses run unseen and unpaced (the window stays black), then Course World appears: the lease is
        // renewed while the sequence runs and let go ~1.5 s of game time after the last press, or after 120 s.
        if (!t_start) t_start = ms_wall();
        if (ms_wall() - t_start > 120.0) { atomic_store(&g_present_hidden, 0), g_audio_muted = 0, atomic_store(&g_splash, false), finished = true, atomic_store(&g_ms_state, 2); continue; }
        if (step >= sizeof g_ms_steps / sizeof *g_ms_steps) {
            if (!done_at) done_at = now;
            if (now - done_at >= 90) { atomic_store(&g_present_hidden, 0), g_audio_muted = 0, atomic_store(&g_splash, false), finished = true, atomic_store(&g_ms_state, 2); continue; }
        }
        atomic_store(&g_present_hidden, 300), g_audio_muted = 1, atomic_store(&g_splash, true);
        {   // the loading bar: waiting for the title screen, then one fifth of the rest for each press, then Course World
            const size_t nsteps = sizeof g_ms_steps / sizeof *g_ms_steps;
            float p;
            if (step >= nsteps) {
                p = 0.9f + 0.1f * (done_at ? fminf((float)(now - done_at) / 90.0f, 1.0f) : 0.0f);
                g_splash_what = "Opening Course World";
            } else if (!started) {
                p = 0.15f * fminf((float)(now - base) / 900.0f, 1.0f);
                g_splash_what = "Waiting for the title screen";
            } else {
                p = 0.2f + 0.7f * (float)step / (float)nsteps;
                g_splash_what = g_ms_steps[step].what;
            }
            atomic_store(&g_splash_progress, p);
        }
        if (step >= sizeof g_ms_steps / sizeof *g_ms_steps) continue;  // (all pressed: waiting for the screen)
        if (started && input_player_buttons()) {  // the player took over
            LOG("[mod] skip mode select: cancelled, the player pressed a button\n");
            g_test_buttons = 0;
            atomic_store(&g_present_hidden, 0), g_audio_muted = 0, atomic_store(&g_splash, false);
            finished = true, atomic_store(&g_ms_state, 2);
            continue;
        }
        // frame rate over one-second windows: steady = at least 45 fps and no change pending in the scene manager
        double t = ms_wall();
        if (!win_t) win_t = t, win_frames = now;
        if (t - win_t >= 1.0) {
            bool ok = (double)(now - win_frames) >= 45.0 * (t - win_t) && !g8(s + 0x344);
            if (!ok) steady_since = 0, next_at = 0;
            else if (!steady_since) steady_since = now;
            win_t = t, win_frames = now;
        }
        // the title screen is up once the boot logo's hooks have run and gone quiet (with Skip intro off the logo plays
        // for ~1100 frames and any press before then would be lost); if they never run, go after 2400 frames. The title
        // screen takes input some time after the hooks stop (it fades in): 30 frames was too early on some starts (the
        // L+R was lost and the rest of the sequence did nothing), 190 worked, so wait 180
        bool title_up = (g_boot_hook_first && now - g_boot_hook_at >= 180) || now - base >= 2400;
        if (!title_up || !steady_since || now - steady_since < 120) continue;
        if (!next_at) next_at = now + g_ms_steps[step].settle;
        if (now < next_at) continue;
        LOG("[mod] skip mode select: %s (frame %llu, scene 0x%x)\n", g_ms_steps[step].what, (unsigned long long)now, target);
        g_test_buttons = g_ms_steps[step].buttons, g_test_buttons_until = now + 8;
        started = true;
        step++;
        next_at = step < sizeof g_ms_steps / sizeof *g_ms_steps ? now + g_ms_steps[step].settle + 8 : 0;
    }
    return NULL;
}

__attribute__((constructor)) static void skip_intro_register(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, ms_driver, NULL) == 0) pthread_detach(th);
    hle_override(MAIN_ADDR(0x17E4340), boot_cappear, "mod: skip boot intro presentation (cAppear)");
    hle_override(MAIN_ADDR(0x17E49F0), boot_cinmain, "mod: skip boot intro presentation (cInMain)");
}

