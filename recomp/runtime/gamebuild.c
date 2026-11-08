// Builds the game code library on the player's machine (runtime/gamebuild.h).
#include "gamebuild.h"
#include "game.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gamelib.h"
#include "paths.h"
#include "platform.h"
#include "rt.h"
#include "platform/x86_cpu.h"

// (GAMELIB_CFLAGS, GAMEBUILD_HLE and GAMEBUILD_PATCHES come from tools/build.py)

#define SEP "\\"

// ------------------------------------------------------------------ child processes
// Every one running, so that quitting mid-build ends them too (atexit) instead of leaving compilers behind.
#define MAX_PROCS 80
static pthread_mutex_t g_plock = PTHREAD_MUTEX_INITIALIZER;
static PlatProc g_procs[MAX_PROCS];
static int g_nprocs;
static bool g_quitting;

static void end_all(void) {
    pthread_mutex_lock(&g_plock);
    g_quitting = true;
    for (int i = 0; i < g_nprocs; i++) plat_proc_kill(g_procs[i]);
    g_nprocs = 0;
    pthread_mutex_unlock(&g_plock);
}

static PlatProc spawn(char **argv, const char *log) {
    pthread_mutex_lock(&g_plock);
    PlatProc p = g_quitting ? 0 : plat_spawn((const char *const *)argv, log, false);
    if (p && g_nprocs < MAX_PROCS) g_procs[g_nprocs++] = p;
    pthread_mutex_unlock(&g_plock);
    return p;
}

// plat_proc_wait without blocking, then forgotten once it has ended
static int finished(PlatProc p) {
    pthread_mutex_lock(&g_plock);
    int r = plat_proc_wait(p, false);
    if (r >= 0)
        for (int i = 0; i < g_nprocs; i++)
            if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; break; }
    pthread_mutex_unlock(&g_plock);
    return r;
}

static void stop(PlatProc p) {  // ends one that is still running, then forgotten
    pthread_mutex_lock(&g_plock);
    bool ours = false;
    for (int i = 0; i < g_nprocs; i++)
        if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; ours = true; break; }
    pthread_mutex_unlock(&g_plock);
    if (ours) plat_proc_kill(p);
}

static void sleep_ms(int ms) { plat_sleep_until_ns(plat_now_ns() + (uint64_t)ms * 1000000ull); }

static int wait_for(PlatProc p) {
    int r;
    while ((r = finished(p)) < 0) sleep_ms(20);
    return r;
}

// ------------------------------------------------------------------ argument lists
typedef struct { char **v; int n, cap; } Args;
static void add(Args *a, const char *s) {
    if (a->n + 2 > a->cap) {
        a->cap = a->cap ? a->cap * 2 : 32;
        a->v = (char **)realloc(a->v, sizeof *a->v * a->cap);
    }
    a->v[a->n++] = strdup(s);
    a->v[a->n] = NULL;
}
static void args_free(Args *a) {
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    memset(a, 0, sizeof *a);
}

// ------------------------------------------------------------------ the tools
typedef struct {
    char gen[1100], gen_script[1100];  // generator: the package's python + tools/gen.py
    char cc[1100];                     // compiler: zig (as `zig cc`)
    char inc[1100], entry[1100], hle[1100];  // headers the game code includes, tools/gamelib_entry.c, HLE names
    char patches[1100];                       // the game's per-instruction patches for main ("" = none)
    char implib[1100];  // the .exe's import library (the runtime functions the game code calls)
} Tools;

static void env_put(const char *k, const char *v) {
    _putenv_s(k, v ? v : "");
}

bool gamebuild_python(char *out, size_t n) {
    if (!rt_resource("python" SEP "python.exe", NULL, out, n)) return false;  // (the package's own Python)
    env_put("PYTHONHOME", NULL);  // (the player's own Python settings don't apply to ours)
    env_put("PYTHONPATH", NULL);
    env_put("PYTHONNOUSERSITE", "1");
    env_put("PYTHONDONTWRITEBYTECODE", "1");  // (nothing written next to the scripts)
    env_put("PYTHONUTF8", "1");
    env_put("PYTHONUNBUFFERED", "1");
    return true;
}

static bool find_tools(Tools *t) {
    memset(t, 0, sizeof *t);
    if (!(gamebuild_python(t->gen, sizeof t->gen) &&
          rt_resource("tools" SEP "gen.py", "tools/gen.py", t->gen_script, sizeof t->gen_script)))
        return false;
    if (!rt_resource("zig" SEP "zig" PLAT_EXE_SUFFIX, NULL, t->cc, sizeof t->cc)) return false;
    if (!rt_resource("gamelib-src" SEP "cpu.h", "runtime/cpu.h", t->inc, sizeof t->inc) ||
        !rt_resource("gamelib-src" SEP "gamelib_entry.c", "tools/gamelib_entry.c", t->entry, sizeof t->entry) ||
        !rt_resource("gamelib-src" SEP "hle_symbols.txt", GAMEBUILD_HLE, t->hle, sizeof t->hle))
        return false;
    if (!rt_resource("gamelib-src" SEP "patches.py", GAMEBUILD_PATCHES, t->patches, sizeof t->patches)) t->patches[0] = 0;
    char *s = strrchr(t->inc, '/'), *b = strrchr(t->inc, '\\');
    if (b > s) s = b;
    if (s) *s = 0;
    return rt_resource(GAME_ID "-dx.dll.a", "build/" GAME_ID "-dx.dll.a", t->implib, sizeof t->implib);
}

bool gamebuild_available(void) {
    Tools t;
    return find_tools(&t);
}

static void compiler(Args *a, const Tools *t) {
    add(a, t->cc);
    add(a, "cc");
}

// ------------------------------------------------------------------ generator progress (its log)
typedef struct {
    const char *name;
    char log[1100], out[1100];
    PlatProc p;
    int exit;                // -1 while running
    int files;               // chunk files written so far (complete)
    unsigned long long ci, n;  // functions written / all
} Gen;

static void read_gen_log(Gen *g) {
    FILE *f = fopen(g->log, "rb");
    if (!f) return;
    static char buf[1 << 16];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0;
    fseek(f, from, SEEK_SET);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *last = NULL;
    for (const char *p = strstr(buf, ": progress "); p; p = strstr(p + 1, ": progress ")) last = p;
    unsigned long long ci, all;
    int files;
    if (last && sscanf(last, ": progress %llu/%llu files %d", &ci, &all, &files) == 3) {
        g->ci = ci, g->n = all;
        if (files > g->files) g->files = files;
    }
}

static void log_tail(const char *path, char *out, size_t n) {  // the last line of a log (an error message)
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0, SEEK_SET);
    size_t k = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[k] = 0;
    while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
    char *nl = strrchr(buf, '\n');
    snprintf(out, n, "%s", nl ? nl + 1 : buf);
}

// ------------------------------------------------------------------ compiling
typedef struct {
    char src[1100], obj[1100], objtmp[1100], log[1100];
    char key[33];            // the object cache's name for it (hash of its C, flags and headers), "" if not cached
    unsigned long long bytes;
    bool started;            // compiling, done or taken from the cache
    bool nocache;            // the shipped entry file: always compiled
} Unit;

// ------------------------------------------------------------------ the object cache
// Compiled objects are kept in <game dir>\.objcache under a hash of what they were built from: the C, the compiler
// flags, the headers and the compiler. The generator cuts its files where the functions' addresses say (tools/gen.py),
// so a new version of the game code that changes some functions changes only their files, and the build compiles
// those and takes the rest from the cache. After a successful build the cache holds that build's objects only.
// RECOMP_NO_OBJCACHE=1 turns it off.
typedef struct { uint64_t a, b; } Hash;

static void hash_init(Hash *h) { h->a = 0xcbf29ce484222325ull, h->b = 0x9E3779B97F4A7C15ull; }

static void hash_add(Hash *h, const void *p, size_t n) {
    const unsigned char *s = (const unsigned char *)p;
    uint64_t a = h->a, b = h->b;
    for (size_t i = 0; i < n; i++) {
        a = (a ^ s[i]) * 0x100000001b3ull;
        b = (b + s[i] + 1) * 0xff51afd7ed558ccdull;
        b ^= b >> 29;
    }
    h->a = a, h->b = b;
}

static bool hash_file(Hash *h, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    static char buf[1 << 16];
    size_t k, total = 0;
    while ((k = fread(buf, 1, sizeof buf, f)) > 0) hash_add(h, buf, k), total += k;
    bool ok = !ferror(f);
    fclose(f);
    hash_add(h, &total, sizeof total);
    return ok;
}

typedef struct {
    Unit *u;
    int n, cap;
} Units;

static void add_unit(Units *us, const char *src, const char *work, const char *stem) {
    if (us->n == us->cap) {
        us->cap = us->cap ? us->cap * 2 : 64;
        us->u = (Unit *)realloc(us->u, sizeof *us->u * us->cap);
    }
    Unit *u = &us->u[us->n++];
    memset(u, 0, sizeof *u);
    snprintf(u->src, sizeof u->src, "%s", src);
    snprintf(u->obj, sizeof u->obj, "%s" SEP "%s.o", work, stem);
    snprintf(u->objtmp, sizeof u->objtmp, "%s", u->obj);
    snprintf(u->log, sizeof u->log, "%s" SEP "%s.log", work, stem);
    struct stat st;
    u->bytes = stat(src, &st) == 0 ? (unsigned long long)st.st_size : 0;
}

// The flags the game code was built with (GAMELIB_CFLAGS, part of its id). zig cc reads -march=<cpu> with its own CPU
// names (x86_64_v3) and takes "-<feature>" off the end, so x86-64-v3 would be CPU "x86": the same CPU as -mcpu there.
//
// x86-64 dispatch (method 1): GAMELIB_CFLAGS carries the v2 baseline, and the build upgrades to
// v3 where this CPU has AVX2/FMA/BMI2, or downgrades an explicit v3 back to v2 where it doesn't. Modern machines lose
// nothing; pre-Haswell machines (back to Nehalem, x86-64-v2) still run: without hardware FMA the lifted code's
// __builtin_elementwise_fma lowers to correctly-rounded fmaf/fma libcalls (slower), with it to vfmadd.
// LARECOMP_GENARCH=<flag> forces an arch flag verbatim (e.g. -march=x86-64-v2), for testing.
static bool gamebuild_cpu_has_v3(void) { return x86_cpu_has_v3(); }

static void add_arch(Args *a, bool want_v3) { add(a, want_v3 ? "-mcpu=x86_64_v3" : "-mcpu=x86_64_v2"); }

static void add_cflags(Args *a) {
    bool has_v3 = gamebuild_cpu_has_v3();
    const char *forced = getenv("LARECOMP_GENARCH");
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", GAMELIB_CFLAGS);
    for (char *f = strtok(buf, " "); f; f = strtok(NULL, " ")) {
        bool is_arch = (!strncmp(f, "-march=", 7) || !strncmp(f, "-mcpu=", 6)) &&
                       (strstr(f, "x86-64-v") || strstr(f, "x86_64_v"));
        if (is_arch) {
            if (forced && *forced) {
                if (!strncmp(forced, "-march=", 7)) {
                    char cpu[128];
                    snprintf(cpu, sizeof cpu, "-mcpu=%s", forced + 7);
                    for (char *c = cpu + 6; *c; c++) if (*c == '-') *c = '_';
                    add(a, cpu);
                } else {
                    add(a, forced);
                }
            } else {
                add_arch(a, has_v3);
            }
            continue;
        }
        if (!strncmp(f, "-march=", 7)) {
            char cpu[128];
            snprintf(cpu, sizeof cpu, "-mcpu=%s", f + 7);
            for (char *c = cpu + 6; *c; c++) if (*c == '-') *c = '_';
            add(a, cpu);
        } else {
            add(a, f);
        }
    }
    // (for trying a flag without a new build of the app; part of the object cache's key, so objects of other flags aren't reused.
    // Not -fno-unwind-tables: ExitThread longjmps through the game's frames, which Windows x64 unwinds with them.)
    const char *extra = getenv("RECOMP_GAMEBUILD_CFLAGS");
    if (extra && *extra) {
        snprintf(buf, sizeof buf, "%s", extra);
        for (char *f = strtok(buf, " "); f; f = strtok(NULL, " ")) add(a, f);
    }
}

// What every object's key starts with: the flags, the headers the C includes and the compiler (its size and date).
static Hash cache_seed(const Tools *t) {
    Hash h;
    hash_init(&h);
    Args a = {0};
    add_cflags(&a);
    for (int i = 0; i < a.n; i++) hash_add(&h, a.v[i], strlen(a.v[i]) + 1);
    args_free(&a);
    static const char *const hdrs[] = {"cpu.h", "simd.h", "simd_fp.h"};
    for (int i = 0; i < 3; i++) {
        char p[1300];
        snprintf(p, sizeof p, "%s" SEP "%s", t->inc, hdrs[i]);
        hash_add(&h, hdrs[i], strlen(hdrs[i]));
        hash_file(&h, p);
    }
    struct stat st;
    if (stat(t->cc, &st) == 0) {
        long long z[2] = {(long long)st.st_size, (long long)st.st_mtime};
        hash_add(&h, z, sizeof z);
    }
    return h;
}

// Names the unit's object in the cache (its source is complete). true: it is there already.
static bool cache_lookup(Unit *u, const Hash *seed, const char *dir) {
    Hash h = *seed;
    if (!hash_file(&h, u->src)) return false;
    snprintf(u->key, sizeof u->key, "%016llx%016llx", (unsigned long long)h.a, (unsigned long long)h.b);
    snprintf(u->obj, sizeof u->obj, "%s" SEP "%s.o", dir, u->key);
    snprintf(u->objtmp, sizeof u->objtmp, "%s.tmp", u->obj);
    struct stat st;
    return stat(u->obj, &st) == 0 && st.st_size > 0;
}

// After a successful build: the objects of the build before that this one didn't use are deleted, and this one's are listed.
static void cache_prune(const char *dir, const Units *us) {
    char path[1200], line[128];
    snprintf(path, sizeof path, "%s" SEP "manifest.txt", dir);
    FILE *f = fopen(path, "rb");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        bool used = false;
        for (int k = 0; k < us->n && !used; k++) used = !strcmp(us->u[k].key, line);
        if (!used) {
            char o[1300];
            snprintf(o, sizeof o, "%s" SEP "%s.o", dir, line);
            remove(o);
        }
    }
    if (f) fclose(f);
    if ((f = fopen(path, "wb"))) {
        for (int k = 0; k < us->n; k++)
            if (us->u[k].key[0]) fprintf(f, "%s\n", us->u[k].key);
        fclose(f);
    }
}

static PlatProc start_compile(const Tools *t, const Unit *u) {
    Args a = {0};
    compiler(&a, t);
    add_cflags(&a);
    char inc[1200];
    snprintf(inc, sizeof inc, "-I%s", t->inc);
    add(&a, inc);
    add(&a, "-DGAMELIB_ID=\"" GAMELIB_ID "\"");
    add(&a, "-c");
    add(&a, u->src);
    add(&a, "-o");
    add(&a, u->objtmp);
    PlatProc p = spawn(a.v, u->log);
    args_free(&a);
    return p;
}

// ------------------------------------------------------------------ the build
// The generator's, the compiler's and the linker's output is kept in the log folder (gamebuild.log) before the work
// folder is deleted: a failed setup leaves what a report needs. Each compile log is kept only when the build failed.
static void keep_one(FILE *out, const char *title, const char *path, long max) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size > 0) {
        long from = size > max ? size - max : 0;
        fseek(f, from, SEEK_SET);
        fprintf(out, "==== %s%s\n", title, from ? " (the end of it)" : "");
        char buf[4096];
        size_t k;
        while ((k = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, k, out);
        fputc('\n', out);
    }
    fclose(f);
}

static void keep_logs(const char *work, const Gen *gens, const Units *units, bool ok) {
    char dir[1100], path[1200], lp[1200];
    if (!plat_log_dir(dir, sizeof dir)) return;
    snprintf(path, sizeof path, "%sgamebuild.log", dir);
    FILE *out = fopen(path, "wb");
    if (!out) return;
    for (int i = 0; i < 2; i++) keep_one(out, gens[i].name, gens[i].log, 1 << 16);
    for (int k = 0; !ok && k < units->n; k++) keep_one(out, units->u[k].src, units->u[k].log, 1 << 14);
    snprintf(lp, sizeof lp, "%s" SEP "link.log", work);
    keep_one(out, "link", lp, 1 << 16);
    fclose(out);
}

bool gamebuild_run(const char *game_dir, const GameBuildOpts *o, char *why, size_t why_size) {
    uint64_t t0 = plat_now_ns();
    static bool registered;
    if (!registered) registered = true, atexit(end_all);
    Tools t;
    if (!find_tools(&t)) {
        snprintf(why, why_size, "the game code builder is missing from this copy of " GAME_TITLE);
        return false;
    }
    char work[1100], lib[1100], tmp[1100];
    snprintf(work, sizeof work, "%s" SEP ".gamebuild", game_dir);
    snprintf(lib, sizeof lib, "%s" SEP "%s", game_dir, GAMELIB_NAME);
    snprintf(tmp, sizeof tmp, "%s" SEP "%s.tmp", work, GAMELIB_NAME);
    rt_remove_game_file(game_dir, ".gamebuild");
    rt_remove_game_file(game_dir, GAMELIB_NAME);
    rt_remove_game_file(game_dir, GAMELIB_NAME ".id");
    mkdir(work, 0755);
    {  // zig's caches in the work folder too (deleted below), not the user's profile: hundreds of MB, used once
        char zc[1200];
        snprintf(zc, sizeof zc, "%s" SEP "zig-cache", work);
        _putenv_s("ZIG_GLOBAL_CACHE_DIR", zc);
        _putenv_s("ZIG_LOCAL_CACHE_DIR", zc);
    }
    char cache[1100] = "";
    Hash seed = {0, 0};
    const char *no_cache = getenv("RECOMP_NO_OBJCACHE");
    if (!(no_cache && *no_cache && *no_cache != '0')) {
        snprintf(cache, sizeof cache, "%s" SEP ".objcache", game_dir);
        mkdir(cache, 0755);
        seed = cache_seed(&t);
    }
    LOG("[gamebuild] generator %s %s, compiler %s cc, %d jobs (%d once the generators are done), object cache %s\n", t.gen,
        t.gen_script, t.cc, o->jobs, o->jobs_after_gen > o->jobs ? o->jobs_after_gen : o->jobs, cache[0] ? "on" : "off");
    {
        const char *forced_arch = getenv("LARECOMP_GENARCH");
        LOG("[gamebuild] x86-64 game code: %s (%s)\n", gamebuild_cpu_has_v3() ? "x86-64-v3 (AVX2/FMA/BMI2)" : "x86-64-v2 baseline",
            forced_arch && *forced_arch ? "LARECOMP_GENARCH override" : "detected");
    }

    // 1. the generator, both modules at once; their finished files are compiled while it goes on
    Gen gens[2] = {{.name = "main"}, {.name = "sdk"}};
    static const char *const base[2] = {"0x7000000000", "0x7004000000"};
    bool ok = true;
    for (int i = 0; i < 2 && ok; i++) {
        Gen *g = &gens[i];
        g->exit = -1;
        snprintf(g->log, sizeof g->log, "%s" SEP "gen_%s.log", work, g->name);
        snprintf(g->out, sizeof g->out, "%s" SEP "%s", work, g->name);
        char nso[1100];
        snprintf(nso, sizeof nso, "%s" SEP "exefs" SEP "%s", game_dir, g->name);
        Args a = {0};
        add(&a, t.gen);
        add(&a, t.gen_script);
        add(&a, g->name);
        add(&a, nso);
        add(&a, base[i]);
        add(&a, g->out);
        if (i == 1) { add(&a, "--hle"); add(&a, t.hle); }
        if (i == 0 && t.patches[0]) { add(&a, "--patches"); add(&a, t.patches); }
        g->p = spawn(a.v, g->log);
        args_free(&a);
        if (!g->p) { snprintf(why, why_size, "the game code builder didn't start"); ok = false; }
    }

    Units units = {0};
    int queued[2] = {0, 0};  // chunk files of each module put in units so far
    bool tables[2] = {false, false};
    // (the generators' memory is free again once they are done: more compilers then)
    int nstarted = 0, ncached = 0, jobs = o->jobs < 1 ? 1 : o->jobs > 64 ? 64 : o->jobs;
    int jobs_after = o->jobs_after_gen > jobs ? (o->jobs_after_gen > 64 ? 64 : o->jobs_after_gen) : jobs;
    struct { PlatProc p; int unit; } run[64];
    int nrun = 0;
    unsigned long long done_bytes = 0;
    double shown = 0;
    {
        add_unit(&units, t.entry, work, "gamelib_entry");
        units.u[0].nocache = true;
    }
    while (ok) {
        // generator progress, newly finished files
        bool gens_done = true;
        double gen_frac = 0;
        unsigned long long written = 0;
        for (int i = 0; i < 2; i++) {
            Gen *g = &gens[i];
            if (g->exit < 0) {
                read_gen_log(g);
                int r = finished(g->p);
                if (r >= 0) {
                    g->exit = r;
                    read_gen_log(g);
                    if (r != 0) {
                        char last[300];
                        log_tail(g->log, last, sizeof last);
                        LOG("[gamebuild] generator (%s) failed: %s\n", g->name, last);
                        if (strstr(last, "MemoryError"))
                            snprintf(why, why_size, "the computer ran out of memory preparing the game code (it needs "
                                                    "about 4 GB of free memory: close other programs and try again)");
                        else  // (the generator's own last line: what a report needs, issue #12)
                            snprintf(why, why_size, "the game code couldn't be prepared (%s: %s)", g->name,
                                     last[0] ? last : "no output");
                        ok = false;
                    }
                }
            }
            if (g->exit != 0) gens_done = false;
            for (; queued[i] < g->files; queued[i]++) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_%03d", g->name, queued[i]);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem);
            }
            if (g->exit == 0 && !tables[i]) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_table", g->name);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem);
                tables[i] = true;
            }
            gen_frac += g->exit == 0 ? 0.5 : g->n ? 0.5 * (double)g->ci / (double)g->n : 0;
        }
        if (!ok) break;
        for (int k = 0; k < units.n; k++) written += units.u[k].bytes;

        // compiles: the biggest file waiting first (the last one to start is then a small one, not the long pole); one that
        // the object cache has needs no compiler
        while (nrun < (gens_done ? jobs_after : jobs)) {
            int pick = -1;
            for (int k = 0; k < units.n; k++)
                if (!units.u[k].started && (pick < 0 || units.u[k].bytes > units.u[pick].bytes)) pick = k;
            if (pick < 0) break;
            Unit *u = &units.u[pick];
            u->started = true;
            nstarted++;
            if (cache[0] && !u->nocache && cache_lookup(u, &seed, cache)) {
                ncached++;
                done_bytes += u->bytes;
                remove(u->src);
                continue;
            }
            PlatProc p = start_compile(&t, u);
            if (!p) { snprintf(why, why_size, "the compiler didn't start"); ok = false; break; }
            run[nrun].p = p, run[nrun].unit = pick;
            nrun++;
        }
        for (int k = 0; k < nrun; k++) {
            int r = finished(run[k].p);
            if (r < 0) continue;
            Unit *u = &units.u[run[k].unit];
            if (r != 0) {
                char last[300];
                log_tail(u->log, last, sizeof last);
                LOG("[gamebuild] compiling %s failed: %s\n", u->src, last);
                snprintf(why, why_size, "the game code didn't compile");
                ok = false;
            }
            else if (strcmp(u->obj, u->objtmp)) {  // (named in the cache once it is whole)
                remove(u->obj);
                if (rename(u->objtmp, u->obj) != 0) {
                    snprintf(why, why_size, "the game code couldn't be saved");
                    ok = false;
                }
            }
            done_bytes += u->bytes;
            if (!u->nocache) remove(u->src);  // (generated C, done with: not kept. The shipped entry file is)
            run[k--] = run[--nrun];
        }
        if (!ok) break;
        if (gens_done && nstarted == units.n && nrun == 0) break;

        // progress: the generator's share (its functions written), the compiler's (bytes of C compiled, of an
        // estimate of all of it while the generator still writes)
        double est_total = gens_done ? (double)written : gen_frac > 0.02 ? (double)written / gen_frac : 0;
        double comp_frac = est_total > 0 ? (double)done_bytes / est_total : 0;
        if (comp_frac > 1) comp_frac = 1;
        double f = 0.2 * gen_frac + 0.75 * comp_frac;
        double secs = (double)(plat_now_ns() - t0) / 1e9;
        if (gen_frac == 0) f = 0.05 * (secs / (secs + 20));  // (the generator's first pass: no count yet)
        if (f > shown) shown = f;
        if (o->progress) o->progress(shown, gen_frac < 1 && !gens_done ? "Reading the game code..." : "Recompiling...");
        sleep_ms(50);
    }
    for (int k = 0; k < nrun; k++) {  // (a failure: the rest is of no use - the failure shows at once)
        stop(run[k].p);
        if (strcmp(units.u[run[k].unit].obj, units.u[run[k].unit].objtmp)) remove(units.u[run[k].unit].objtmp);
    }
    for (int i = 0; i < 2; i++)
        if (gens[i].p && gens[i].exit < 0) stop(gens[i].p);

    // 2. link
    if (ok) {
        if (o->progress) o->progress(shown > 0.95 ? shown : 0.95, "Finishing...");
        char rsp[1100], log[1100];
        snprintf(rsp, sizeof rsp, "%s" SEP "link.rsp", work);
        snprintf(log, sizeof log, "%s" SEP "link.log", work);
        FILE *f = fopen(rsp, "wb");
        if (f) {
            for (int k = 0; k < units.n; k++) {  // (forward slashes: the response file's backslashes are escapes)
                fputc('"', f);
                for (const char *c = units.u[k].obj; *c; c++) fputc(*c == '\\' ? '/' : *c, f);
                fputs("\"\n", f);
            }
            fclose(f);
        }
        Args a = {0};
        char at[1200];
        snprintf(at, sizeof at, "@%s", rsp);
        compiler(&a, &t);
        add(&a, "-shared");
        add(&a, "-o");
        add(&a, tmp);
        add(&a, at);
        add(&a, t.implib);
        PlatProc p = f ? spawn(a.v, log) : 0;
        args_free(&a);
        int r = p ? wait_for(p) : 255;
        if (r != 0) {
            char last[300];
            log_tail(log, last, sizeof last);
            LOG("[gamebuild] linking failed: %s\n", last);
            snprintf(why, why_size, "the game code didn't link");
            ok = false;
        }
    }
    if (ok) {
        remove(lib);
        char idp[1100];
        snprintf(idp, sizeof idp, "%s.id", lib);
        FILE *f = NULL;
        // (x86-64: the level it was built for follows the id, gamelib.c id_matches: a v3 build moved to a CPU without
        // AVX2 is built again instead of failing on its first AVX2 instruction)
        const char *forced = getenv("LARECOMP_GENARCH");
        const char *level = forced && *forced ? " forced" : gamebuild_cpu_has_v3() ? " x86-64-v3" : " x86-64-v2";
        if (rename(tmp, lib) != 0 || !(f = fopen(idp, "wb")) || fprintf(f, "%s%s\n", GAMELIB_ID, level) < 0) {
            snprintf(why, why_size, "the game code couldn't be saved");
            ok = false;
        }
        if (f) fclose(f);
    }
    if (ok && cache[0]) {
        const char *keep = getenv("RECOMP_KEEP_OBJCACHE");
        // (a build that failed keeps what it compiled: trying again goes on from there. The next one that works clears it)
        if (o->keep_cache || (keep && *keep && *keep != '0')) cache_prune(cache, &units);
        else rt_remove_game_file(game_dir, ".objcache");
    }
    keep_logs(work, gens, &units, ok);
    free(units.u);
    rt_remove_game_file(game_dir, ".gamebuild");  // the C, the objects, the logs
    LOG("[gamebuild] %s in %.0fs (%d of %d objects from the cache)\n", ok ? "done" : "failed",
        (double)(plat_now_ns() - t0) / 1e9, ncached, units.n);
    if (ok && o->progress) o->progress(1, "Finishing...");
    return ok;
}
