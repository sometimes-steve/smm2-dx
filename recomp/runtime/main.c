// LAS Recomp entry: load modules, set up the kernel, run nn::init::Start on the main thread.
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamebuild.h"
#include "gamelib.h"
#include "paths.h"
#include "settings.h"
#include "hle.h"
#include "kernel.h"
#include "mods.h"

const char *g_romfs_dir;

// rtld normally passes these to nn::init::Start: run every module's init/fini arrays.
static void run_array(Cpu *c, uint64_t arr, uint64_t size) {
    for (uint64_t i = 0; i < size / 8; i++) {
        uint64_t fn = ((uint64_t *)arr)[i];
        if (fn) guest_call(c, fn);
    }
}

// nn::init::Start calls x2 right after nnosInitialize and x3 after nninitStartup, just before
// nnMain: SDK constructors run early, the game's run once nninitStartup has set up its heap.
// rtld runs a module's DT_INIT, which registers it (and its TLS template) with musl via
// __nnmusl_init_dso and then runs its init_array; fall back to the array if there is no DT_INIT.
static void module_init(Cpu *c, Module *m) {
    LOG("init %s (DT_INIT %llx, %llu init_array entries)\n", m->name, m->init, m->init_array_sz / 8);
    if (m->init) guest_call(c, m->init);
    else run_array(c, m->init_array, m->init_array_sz);
}

static void rtld_init_sdk(Cpu *c) { module_init(c, &g_modules[1]); }
static void rtld_init_main(Cpu *c) { module_init(c, &g_modules[0]); }

// Claim the guest address range before anything else can map there. The host allocator places
// regions at random addresses in the same window; on a collision, re-exec for a fresh layout.
__attribute__((constructor(101))) static void reserve_guest_space(void) {
    if (plat_reserve_guest(GUEST_LO, GUEST_HI)) return;
    const char *tries = getenv("LARECOMP_REEXEC");
    int n = tries ? atoi(tries) : 0;
    if (n >= 20) {
        fprintf(stderr, "FATAL: guest address range %llx-%llx is not free\n", GUEST_LO, GUEST_HI);
        exit(1);
    }
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    setenv("LARECOMP_REEXEC", buf, 1);
    plat_exec(NULL);
    perror("re-exec");
    exit(1);
}

// ------------------------------------------------------------------ boot / in-place reboot
// Switching seeds (or vanilla) and going back to the start screen reboot the game inside this process: the window,
// GPU device, settings and shader caches stay. rt_request_reboot (menu, any thread) makes ui_run return on the
// main thread, which then: stops every guest thread (kernel_stop_threads: each unwinds at its next svc / HLE call /
// wait), resets the renderer (textures, address-keyed caches) and all per-boot host state (RT_ON_RESET hooks:
// services, HLE features, seed data, audio), empties the kernel, wipes guest memory, and boots again.
static _Atomic int g_reboot;
void rt_request_reboot(int kind) {
    LOG("[reboot] requested (%s)\n", kind == RT_REBOOT_TITLE ? "start screen" : "boot");
    g_reboot = kind;
}
int rt_reboot_requested(void) { return g_reboot; }

// Rebuild Game Code (General > Game): the compiled game library is loaded by this process, so it can't be deleted from
// here. A flag file says so, and the program starts again (the same way a stuck in-place reboot does); the new process
// deletes the library and the build cache before anything loads, and the setup screen builds it again from the ExeFS.
#define CLEAN_REBUILD_FLAG "clean-rebuild.flag"
static bool g_force_restart;
void rt_request_clean_rebuild(void) {
    FILE *f = fopen(CLEAN_REBUILD_FLAG, "wb");
    if (!f) { LOG("[rebuild] cannot write " CLEAN_REBUILD_FLAG ": nothing changed\n"); return; }
    fclose(f);
    LOG("[rebuild] clean rebuild of the game code requested: restarting\n");
    g_force_restart = true;
    rt_request_reboot(RT_REBOOT_TITLE);
}
static void clean_rebuild_apply(const char *root) {  // (start-up, nothing loaded yet)
    FILE *f = fopen(CLEAN_REBUILD_FLAG, "rb");
    if (!f) return;
    fclose(f);
    remove(CLEAN_REBUILD_FLAG);  // (first: a failure below must not repeat on every start)
    bool ok = rt_remove_game_file(root, GAMELIB_NAME ".id");
    ok &= rt_remove_game_file(root, GAMELIB_NAME);
    ok &= rt_remove_game_file(root, ".objcache");
    ok &= rt_remove_game_file(root, ".gamebuild");
    LOG("[rebuild] the compiled game code and its build cache %s: the setup screen builds it again\n", ok ? "were deleted" : "could not all be deleted");
}

// LARECOMP_REBOOT_AT=<frame>:<seed | vanilla | title>[,...] (test): at that presented frame, switch to the seed /
// vanilla in place, or go back to the start screen (with LARECOMP_LAUNCHER_AUTOPLAY it plays the saved choice).
// Frames count on across reboots. Called by the presenter every frame.
void reboot_test_frame(uint64_t frame) {
    static const char *next;
    static int init;
    if (!init) { init = 1; next = getenv("LARECOMP_REBOOT_AT"); }
    long at;
    char what[128];
    if (!next || !*next || sscanf(next, "%ld:%127[^,]", &at, what) != 2 || (long)frame < at) return;
    next = strchr(next, ',');
    if (next) next++;
    if (!strcmp(what, "title")) { rt_request_reboot(RT_REBOOT_TITLE); return; }
    rt_request_reboot(RT_REBOOT_GAME);
}

// PE/COFF: .rst$a < .rst$m (entries, hle.h) < .rst$z (the linker sorts by the text after '$')
__attribute__((section(".rst$a"), used)) static const ResetFn __start_resets[1];
__attribute__((section(".rst$z"), used)) static const ResetFn __stop_resets[1];

void backend_reset(void);             // renderer: GPU idle, game textures and address-keyed caches dropped
void backend_guest_memory_reset(void);  // renderer: guest memory was replaced (re-wrap the heap)
void backend_clear_window(void);      // a black frame while the game is down

static void reboot_idle(void) {  // (main thread, while guest threads stop) keep the window alive
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT) { LOG("window closed\n"); exit(0); }
}

static int g_argc;
static char **g_argv;

// A guest thread that never reaches a safe point can't be unwound (rt_unwind_point), so the in-place reboot can't
// finish: the program starts again instead (Quit to Start Screen: with the setup screen), its log carrying on.
static void restart_process(bool setup) {
    char exe[1100];
    static char *av[64];
    int n = 0;
    av[n++] = plat_exe_path(exe, sizeof exe) ? strdup(exe) : g_argv[0];
    for (int i = 1; i < g_argc && n < 60; i++)
        if (strcmp(g_argv[i], "--setup") && strcmp(g_argv[i], "--play")) av[n++] = g_argv[i];
    if (setup) av[n++] = "--setup";
    av[n] = NULL;
    LOG("[reboot] starting the program again%s\n", setup ? " (setup screen)" : "");
    setenv("LASRECOMP_LOG_APPEND", "1", 1);
    plat_exec(av);
    FATAL("the program could not be started again\n");
}

static void teardown(int kind) {
    uint64_t t0 = plat_now_ns();
    if (kernel_stop_threads(reboot_idle) != 0 || g_force_restart) restart_process(false);  // (or a forced one: the game library goes)
    backend_clear_window();
    backend_reset();
    const ResetFn *start = __start_resets, *stop = __stop_resets;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    int n = 0;
    for (const ResetFn *p = start; p < stop; p++)
        if (*p) { (*p)(); n++; }
    kernel_reset();
    if (!plat_reset_guest(GUEST_LO, GUEST_HI)) FATAL("cannot reset guest memory\n");
    backend_guest_memory_reset();
    g_nmodules = 0;
    g_reboot = RT_REBOOT_NONE;
    LOG("[reboot] game torn down in %llums (%d reset hooks)\n", (unsigned long long)((plat_now_ns() - t0) / 1000000), n);
}

static void build_progress(double fraction, const char *step) {
    static int last = -100;
    int pct = (int)(fraction * 100);
    if (pct >= last + 5) { last = pct; LOG("[gamebuild] %d%% %s\n", pct, step); }
}

// First launch of a packaged build (no start screen): the game code library isn't there yet, the player's game files
// are. It is built from them once (runtime/gamebuild.c: the generator, then the bundled compiler), into <root>.
static void first_launch_build(const char *root) {
    char main_nso[1100];
    snprintf(main_nso, sizeof main_nso, "%s/exefs/main", root);
    FILE *f = fopen(main_nso, "rb");
    if (!f) return;  // (no game files: gamelib_load says so)
    fclose(f);
    if (!gamebuild_available()) {
        LOG("[gamebuild] the game code isn't built and the builder (tools/venv + tools/gen.py, zig/) isn't here\n");
        return;
    }
    LOG("[gamebuild] first launch: building the game code from %s (a few minutes to half an hour)\n", root);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, GAME_TITLE,
                             "First launch: " GAME_TITLE " now recompiles the game code from your own game files. This "
                             "takes from a few minutes to half an hour; the game starts by itself when it's done (progress in the "
                             "console window).", NULL);
    int jobs = SDL_GetNumLogicalCPUCores();
    GameBuildOpts o = {build_progress, jobs > 1 ? jobs : 1, 0, g_settings.keep_obj_cache};
    char why[512];
    if (!gamebuild_run(root, &o, why, sizeof why)) FATAL("building the game code failed: %s (details above)\n", why);
    LOG("[gamebuild] done\n");
}

static void boot(const char *root) {
    char exefs[1024], romfs[1024], npdm_path[1024];
    snprintf(exefs, sizeof exefs, "%s/exefs", root);
    snprintf(romfs, sizeof romfs, "%s/romfs", root);
    snprintf(npdm_path, sizeof npdm_path, "%s/main.npdm", exefs);
    if (!g_romfs_dir) g_romfs_dir = strdup(romfs);

    char why[512];
    if (!gamelib_present(root)) first_launch_build(root);
    if (!gamelib_load(root, why, sizeof why)) FATAL("%s\n", why);
    loader_load(exefs);
    { extern void net_config_reload(void); net_config_reload(); }  // (the network settings read again: before the mods)
    mods_boot();
    { extern void settings_load(void); settings_load(); }
    kernel_init();
    dispatch_init();

    uint32_t stack_size = 0x100000;
    int prio = 44;
    FILE *f = fopen(npdm_path, "rb");
    if (f) {
        uint8_t meta[0x80];
        if (fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4)) {
            prio = meta[0xE];
            stack_size = *(uint32_t *)(meta + 0x1C);
        }
        fclose(f);
    }
    LOG("main thread: prio %d, stack 0x%x\n", prio, stack_size);

    uint64_t start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (!start) FATAL("nn::init::Start not found\n");
    uint64_t init_fn = hle_register("<rtld_init_sdk>", rtld_init_sdk);
    uint64_t fini_fn = hle_register("<rtld_init_main>", rtld_init_main);
    KThread *t = kernel_create_main_thread(start, stack_size, prio);
    kernel_run_main(t, t->handle, 0, init_fn, fini_fn);  // nn::init::Start(thread_handle, args_region, init, fini)
}

int main(int argc, char **argv) {
    g_argc = argc, g_argv = argv;
    plat_init();
    setvbuf(stderr, NULL, _IONBF, 0);  // the Windows CRT treats _IOLBF as full buffering
    const char *root = "extracted";  // (in the data root)
    static char arg_root[1024];
    bool setup = false;  // --setup: the setup screen even when everything is there
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--play")) continue;  // (no setup screen unless something is missing, as before)
        else if (!strcmp(argv[i], "--setup")) setup = true;
        else if (argv[i][0] != '-' && !realpath(argv[i], arg_root)) root = argv[i];  // (relative to where we started)
        else if (argv[i][0] != '-') root = arg_root;
    }
    rt_paths_init();  // the data root becomes the working directory (runtime/paths.h)
    {   // tests: recomp-test.env in the data root, KEY=value lines, sets the environment
        FILE *f = fopen("recomp-test.env", "r");
        char line[1024];
        while (f && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (line[0] == '#' || !eq || eq == line) continue;
            *eq = 0;
            setenv(line, eq + 1, 1);
            fprintf(stderr, "[main] recomp-test.env: %s set\n", line);
        }
        if (f) fclose(f);
    }
    // The setup screen (runtime/ui/launcher.cpp): in place of the game until the player's files are set up, and after
    // Quit to Start Screen. Then the game boots in its window.
    extern bool launcher_needed(const char *root, bool asked);
    extern void launcher_run(const char *root);
    extern void launcher_boot_env(void);
    extern void launcher_update_check_start(void);
    clean_rebuild_apply(root);  // (Rebuild Game Code, asked for in the last run)
    launcher_update_check_start();  // (notifies only: the setup screen and the overlay menu's Mods tab)
    for (;;) {
        if (launcher_needed(root, setup)) launcher_run(root);
        launcher_boot_env();  // (also after the setup screen: the config it chose is the one the game reads)
        setup = false;
        boot(root);
        int kind = ui_run();  // window and input on the main thread; returns when a reboot is requested
        teardown(kind);
        if (kind == RT_REBOOT_TITLE) setup = true;  // (Quit to Start Screen: the setup screen)
    }
}
