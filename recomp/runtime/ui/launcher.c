// The setup screen (start screen), shown in place of the game until it is set up: nothing of the game ships with the
// app, so the player points it at their own dump, and the app prepares everything from it on their machine, then runs
// the game. Modelled on melee-pc's launcher (DECISIONS.md, "Releases: a launcher plus recompiler per platform"); the
// window, render loop and update check follow LAS Recomp's start screen (UPSTREAM.md).
//
//   1. Your game: Super Mario Maker 2 with the 3.0.3 update - folders of the decrypted, extracted ExeFS and RomFS,
//      or NSP / XCI files (decrypted with prod.keys). games/smm2/setup.py checks the version (build ids and hashes of the player's
//      own files against games/smm2/versions.py) and lays the files out in <data>/extracted.
//   2. prod.keys and the firmware (the console's system NCAs): games/smm2/sysdata.py copies the button glyph font (the game
//      crashes without it) into <data>/fonts, and the Mii face-part archive (MiiModel 0802) into
//      <data>/sysdata.
//      The keys are read where they are, by hactool only: never copied, logged or shown.
//   Then the game code is recompiled from the player's ExeFS (runtime/gamebuild.c: the bundled Python runs the
//   generator, the bundled zig compiles it), and the game boots in the same window.
//
// What the player chose is kept in <data>/launcher.ini (paths only). Once everything is there the game starts straight
// away; `--setup`, or the overlay menu's Quit > Quit to Start Screen, brings this screen back. RECOMP_NO_LAUNCHER=1:
// never (the game starts, or says what's missing, as before).
//
// The update check asks GitHub for the project's latest release (a background curl, which Windows 10+ ships)
// and only says that there is one, with a link to its page: nothing is downloaded.
//
// Tests: RECOMP_LAUNCHER_AUTO="game=<path>|game=<path>|keys=<file>|firmware=<dir>" fills the page in and
// presses Set up after a moment (as the player would); RECOMP_LAUNCHER_SHOTS=<dir> saves the window as PNG whenever
// the screen changes (and every 30 s while a step runs); RECOMP_NO_UPDATE_CHECK=1; RECOMP_UPDATE_TAGS=<tag>: pretend
// that release exists (no network).


#include "ui/ig.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "paths.h"

#include <SDL3/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include "launcher_art.h"
#include "option_rows.h"
#include "osk.h"

#include "gamebuild.h"
#include "gamelib.h"
#include "platform.h"
void rt_log(const char *fmt, ...);  // runtime/rt.h LOG
void ui_window_init(void);
SDL_Window *ui_window(void);
void menu_apply_window_requests(void *sdl_window);
bool vk_frame_begin(const float clear[4]);
void vk_frame_end(const char *shot_path);
void *vk_frame_cmd(void);
bool vk_imgui_backend_init(void);
void vk_imgui_backend_shutdown(void);

#define SEP "\\"

#define LOG(...) rt_log(__VA_ARGS__)
#ifndef RECOMP_VERSION
#define RECOMP_VERSION "dev"
#endif
#define RELEASES_API "https://api.github.com/repos/sometimes-steve/smm2-dx/releases/latest"
#define RELEASES_PAGE "https://github.com/sometimes-steve/smm2-dx/releases"

#define PATHLEN 4400  // (a path, with room for what is appended to it)
#define CFGLEN 4200   // (a path kept in launcher.ini)

static const char MAIN_303[] = "c2dc405ac414c37c8b1c50219c7a0f0c";  // (games/smm2/versions.py: v3.0.3 main build id)
static const char MAIN_100[] = "3ebb1eb2fffb029a8b87579b961a7b";    // (v1.0.0: the base game without the update)
static const char *FONT_STD = "fonts" SEP "nintendo_ext_003.bfttf";  // (the button glyph font: the one the game cannot do without)

// ------------------------------------------------------------------ small string helpers (malloc'd, freed by the owner)
static void *xalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "launcher: out of memory\n");
        abort();
    }
    return p;
}
static char *xdup_n(const char *s, size_t n) {
    char *d = (char *)xalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}
static char *xdup(const char *s) { return xdup_n(s, strlen(s)); }
static char *xvfmt(const char *fmt, va_list ap) {
    va_list a2;
    va_copy(a2, ap);
    int n = vsnprintf(NULL, 0, fmt, a2);
    va_end(a2);
    if (n < 0) n = 0;
    char *s = (char *)xalloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    return s;
}
static char *xfmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = xvfmt(fmt, ap);
    va_end(ap);
    return s;
}
static void set_str(char **dst, const char *s) {  // replaces a malloc'd string
    char *n = xdup(s ? s : "");
    free(*dst);
    *dst = n;
}

// A list of strings
typedef struct {
    char **v;
    int n;
} StrList;
static void strlist_push(StrList *l, const char *s) {
    l->v = (char **)realloc(l->v, (size_t)(l->n + 1) * sizeof(char *));
    if (!l->v) abort();
    l->v[l->n++] = xdup(s);
}
static void strlist_remove(StrList *l, int i) {
    free(l->v[i]);
    memmove(&l->v[i], &l->v[i + 1], (size_t)(l->n - i - 1) * sizeof(char *));
    l->n--;
}
static void strlist_clear(StrList *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = 0;
}

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}
static bool dir_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static const char *home_dir(void) {
    const char *h = getenv("USERPROFILE");
    return h ? h : "";
}
static bool ends_with(const char *s, const char *e) {  // (case-insensitive; e is lower case)
    size_t n = strlen(e), sl = strlen(s);
    if (sl < n) return false;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)s[sl - n + i]) != e[i]) return false;
    return true;
}
static const char *base_name(const char *p) {
    const char *b = p;
    for (const char *c = p; *c; c++)
        if (*c == '/' || *c == '\\') b = c + 1;
    return b;
}
static bool starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

// Trailing newlines / spaces dropped in place, leading spaces skipped: the trimmed text (inside s)
static char *trim_ptr(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
    while (*s == ' ') s++;
    return s;
}
// The trimmed copy of src in dst (cap bytes)
static void trim_into(char *dst, size_t cap, const char *src) {
    char *tmp = xdup(src);
    snprintf(dst, cap, "%s", trim_ptr(tmp));
    free(tmp);
}

// The build id of an NSO (hex, trailing zero bytes dropped) in out; false (out empty) if it isn't one
static bool nso_build_id(const char *path, char *out, size_t cap) {
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[0x60] = {0};
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n != sizeof h || memcmp(h, "NSO0", 4)) return false;
    int end = 0x60;
    while (end > 0x40 && !h[end - 1]) end--;
    size_t len = 0;
    for (int i = 0x40; i < end && len + 3 <= cap; i++) len += (size_t)snprintf(out + len, cap - len, "%02x", h[i]);
    return out[0] != 0;
}

// ------------------------------------------------------------------ what's ready
static char g_root[PATHLEN];  // <data>/extracted

static bool game_files_ok(const char *root) {
    char p[PATHLEN], id[96];
    snprintf(p, sizeof p, "%s" SEP "exefs" SEP "main", root);
    nso_build_id(p, id, sizeof id);
    if (!starts_with(id, MAIN_303)) return false;
    snprintf(p, sizeof p, "%s" SEP "romfs", root);
    if (!dir_exists(p)) return false;
    snprintf(p, sizeof p, "%s" SEP "exefs" SEP "main.npdm", root);
    return file_exists(p);
}
static bool fonts_ok(void) { return file_exists(FONT_STD); }  // (relative: the data root is the working directory)

static bool ocw_config_ready(void);  // (below: a usable OpenCourse World config is chosen or in the data folder)

bool launcher_needed(const char *root, bool asked) {
    if (getenv("RECOMP_NO_LAUNCHER") && atoi(getenv("RECOMP_NO_LAUNCHER"))) return false;
    if (getenv("LARECOMP_REBOOT_AT")) asked = false;  // (reboot tests: straight back into the game, as before)
    if (asked || getenv("RECOMP_LAUNCHER_AUTO")) return true;
    // (a source checkout started in its data folder: its own fonts or none, as before)
    return !game_files_ok(root) || !gamelib_present(root) || (!fonts_ok() && !rt_dev_mode()) ||
           (!ocw_config_ready() && !rt_dev_mode());
}

// ------------------------------------------------------------------ launcher.ini (paths only, never key contents)
typedef struct {
    StrList game;  // folders with the decrypted game
    char keys[CFGLEN], firmware[CFGLEN];
    char ocw[CFGLEN];  // the ocw-config.json exported from opencourse.world (read where it is)
    bool update_check;
} Config;
static Config g_cfg = {{NULL, 0}, "", "", "", true};
static const char *INI = "launcher.ini";

static void config_load(void) {
    strlist_clear(&g_cfg.game);
    g_cfg.keys[0] = g_cfg.firmware[0] = g_cfg.ocw[0] = 0;
    g_cfg.update_check = true;
    FILE *f = fopen(INI, "r");
    if (!f) return;
    char line[4200];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        const char *k = trim_ptr(line), *v = trim_ptr(eq + 1);
        if (!strcmp(k, "game") && *v) strlist_push(&g_cfg.game, v);
        else if (!strcmp(k, "keys")) snprintf(g_cfg.keys, sizeof g_cfg.keys, "%s", v);
        else if (!strcmp(k, "firmware")) snprintf(g_cfg.firmware, sizeof g_cfg.firmware, "%s", v);
        else if (!strcmp(k, "ocw")) snprintf(g_cfg.ocw, sizeof g_cfg.ocw, "%s", v);
        else if (!strcmp(k, "update_check")) g_cfg.update_check = atoi(v) != 0;
    }
    fclose(f);
}
static void config_save(void) {
    FILE *f = fopen(INI, "w");
    if (!f) return;
    fprintf(f, "# " GAME_TITLE " setup screen: where your own files are (paths only)\n");
    for (int i = 0; i < g_cfg.game.n; i++) fprintf(f, "game=%s\n", g_cfg.game.v[i]);
    fprintf(f, "keys=%s\nfirmware=%s\nocw=%s\nupdate_check=%d\n", g_cfg.keys, g_cfg.firmware, g_cfg.ocw,
            g_cfg.update_check ? 1 : 0);
    fclose(f);
}

// Before every boot (the setup keeps no other paths to hand on): the OpenCourse World config chosen here is the one
// the game reads, where it is (games/smm2/mods/ocw.c: RECOMP_OCW_CONFIG), so its token is never copied
void launcher_boot_env(void) {
    config_load();
    if (g_cfg.ocw[0] && !getenv("RECOMP_OCW_CONFIG")) setenv("RECOMP_OCW_CONFIG", g_cfg.ocw, 0);
}

// Where emulators keep the keys and the firmware they were given (suggestions; the player can choose others): the
// folder they are under (%APPDATA%, or the profile's AppData\Roaming)
static void appdata_dir(char *out, size_t cap) {
    const char *ad = getenv("APPDATA");
    if (ad) snprintf(out, cap, "%s", ad);
    else snprintf(out, cap, "%s\\AppData\\Roaming", home_dir());
}
static const char *KEYS_SUFFIX[] = {"\\Ryujinx\\system\\prod.keys", "\\yuzu\\keys\\prod.keys", "\\suyu\\keys\\prod.keys",
                                    "\\eden\\keys\\prod.keys", "\\citron\\keys\\prod.keys"};  // (under AppData; then ~\.switch\prod.keys)
static const char *KEYS_HOME_SUFFIX = "\\.switch\\prod.keys";
static const char *FIRMWARE_SUFFIX[] = {"\\Ryujinx\\bis\\system\\Contents\\registered", "\\yuzu\\nand\\system\\Contents\\registered",
                                        "\\eden\\nand\\system\\Contents\\registered"};

// ------------------------------------------------------------------ checks of what was chosen (cheap, no keys shown)
typedef struct {
    int kind;    // 0 matched (green), 1 warning, 2 detail (grey)
    char *text;  // (malloc'd)
} CheckLine;
typedef struct {
    bool ok, warn;
    char text[512];
    CheckLine *lines;  // the verbose part, shown under the folder's line (malloc'd array of malloc'd texts)
    int nlines;
} Check;
typedef void (*CheckFn)(const char *p, Check *c);

static void check_free(Check *c) {
    for (int i = 0; i < c->nlines; i++) free(c->lines[i].text);
    free(c->lines);
    memset(c, 0, sizeof *c);
}
static void check_clone(Check *dst, const Check *src) {
    *dst = *src;
    dst->lines = (CheckLine *)xalloc((size_t)(src->nlines ? src->nlines : 1) * sizeof(CheckLine));
    for (int i = 0; i < src->nlines; i++) {
        dst->lines[i].kind = src->lines[i].kind;
        dst->lines[i].text = xdup(src->lines[i].text);
    }
}
static void check_text(Check *c, const char *t) { snprintf(c->text, sizeof c->text, "%s", t); }
static void check_line(Check *c, int kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *t = xvfmt(fmt, ap);
    va_end(ap);
    c->lines = (CheckLine *)realloc(c->lines, (size_t)(c->nlines + 1) * sizeof(CheckLine));
    if (!c->lines) abort();
    c->lines[c->nlines].kind = kind;
    c->lines[c->nlines].text = t;
    c->nlines++;
}

// A source of game files: a folder with the decrypted game (the version of an ExeFS in it, if there is one near the top)
static void check_game_source(const char *p, Check *c) {
    if (file_exists(p)) {
        bool container = ends_with(p, ".nsp") || ends_with(p, ".xci");
        if (container) {
            c->ok = true;
            check_text(c, "NSP / XCI - decrypted with prod.keys when set up");
        } else {
            check_text(c, "not an NSP / XCI file: add that, or the folder with the decrypted game (exefs and romfs)");
        }
        return;
    }
    if (!dir_exists(p)) { check_text(c, "not found"); return; }
    c->ok = true;
    // the folder itself, then the folders directly inside it: the first one with an ExeFS main
    StrList roots = {NULL, 0};
    strlist_push(&roots, "");
    DIR *d = opendir(p);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char sub[PATHLEN];
            snprintf(sub, sizeof sub, "%s" SEP "%s", p, e->d_name);
            if (dir_exists(sub)) strlist_push(&roots, e->d_name);
        }
        closedir(d);
    }
    const char *places[] = {"", SEP "exefs", SEP "ExeFS"};
    for (int ri = 0; ri < roots.n; ri++) {
        const char *sub = roots.v[ri];
        char root[PATHLEN];
        if (!*sub) snprintf(root, sizeof root, "%s", p);
        else snprintf(root, sizeof root, "%s" SEP "%s", p, sub);
        for (int pi = 0; pi < 3; pi++) {
            const char *pl = places[pi];
            char main_path[PATHLEN], id[96], tmp[PATHLEN];
            snprintf(main_path, sizeof main_path, "%s%s" SEP "main", root, pl);
            if (!nso_build_id(main_path, id, sizeof id)) continue;
            char *where = xfmt("%s%s%s%smain", *sub ? sub : "", *sub ? SEP : "", *pl ? pl + 1 : "", *pl ? SEP : "");
            snprintf(tmp, sizeof tmp, "%s" SEP "romfs", root);
            bool has_romfs = dir_exists(tmp);
            if (!has_romfs) {
                snprintf(tmp, sizeof tmp, "%s" SEP "RomFS", root);
                has_romfs = dir_exists(tmp);
            }
            check_line(c, 2, "folder:  %s", root);
            check_line(c, 2, "game code:  %s  (build id %s)", where, id);
            free(where);
            if (starts_with(id, MAIN_303)) {
                check_text(c, "Super Mario Maker 2 3.0.3 - matched");
                check_line(c, 0, "OK  build id matches the 3.0.3 release (%s)", MAIN_303);
            } else if (starts_with(id, MAIN_100)) {
                check_text(c, "version 1.0.0 - also add the 3.0.3 update"), c->warn = true;
                check_line(c, 1, "!  this is the 1.0.0 base game; 3.0.3 expects build id %s", MAIN_303);
            } else {
                check_text(c, "another version - 3.0.3 is needed"), c->warn = true;
                check_line(c, 1, "!  build id doesn't match the 3.0.3 release (%s)", MAIN_303);
            }
            if (has_romfs) check_line(c, 0, "OK  romfs folder found next to it");
            else check_line(c, 1, "!  no romfs folder next to it");
            if (!has_romfs) c->warn = true;
            strlist_clear(&roots);
            return;
        }
    }
    strlist_clear(&roots);
    check_text(c, "searched for exefs and romfs when set up");
    check_line(c, 2, "no exefs/main found here or one folder down; setup will search deeper");
}

static void check_keys(const char *p, Check *c) {
    if (!*p) { check_text(c, "needed for the fonts"); return; }
    FILE *f = fopen(p, "r");
    if (!f) { check_text(c, "not found"); return; }
    // only the names are looked at (each line "name = value"); nothing is kept
    char line[512];
    int n = 0;
    bool header = false, master = false;
    while (fgets(line, sizeof line, f) && n < 4000) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        n++;
        if (!strncmp(line, "header_key", 10)) header = true;
        if (!strncmp(line, "master_key_", 11)) master = true;
    }
    fclose(f);
    memset(line, 0, sizeof line);
    c->ok = header && master;
    check_text(c, c->ok ? "keys file found (read in place, never copied)" : "doesn't look like a prod.keys (no header_key / master keys)");
}

static void check_firmware(const char *p, Check *c) {
    if (!*p) { check_text(c, "needed: the console's fonts come from it"); return; }
    DIR *d = opendir(p);
    if (!d) { check_text(c, "not found"); return; }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) n += ends_with(e->d_name, ".nca");
    closedir(d);
    c->ok = n > 0;
    char b[200];
    snprintf(b, sizeof b, n ? "%d system files" : "no NCA files in this folder (choose the folder with the firmware's .nca files)", n);
    check_text(c, b);
}

// The text of a file (malloc'd, "" if it can't be read); *len: its length
static char *read_text(const char *p, size_t *len) {
    char *s = (char *)xalloc(1);
    size_t n = 0;
    s[0] = 0;
    FILE *f = fopen(p, "rb");
    if (f) {
        char buf[8192];
        size_t r;
        while ((r = fread(buf, 1, sizeof buf, f)) > 0 && n < (4u << 20)) {
            s = (char *)realloc(s, n + r + 1);
            if (!s) abort();
            memcpy(s + n, buf, r);
            n += r;
            s[n] = 0;
        }
        fclose(f);
    }
    if (len) *len = n;
    return s;
}

// The value of a "key": "value" string in the ocw-config.json text (malloc'd, "" if there is none)
static char *ocw_field(const char *text, const char *key) {
    char *pat = xfmt("\"%s\"", key);
    const char *k = strstr(text, pat);
    free(pat);
    if (!k) return xdup("");
    const char *colon = strchr(k, ':');
    const char *q1 = colon ? strchr(colon, '"') : NULL;
    const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
    return q2 ? xdup_n(q1 + 1, (size_t)(q2 - q1 - 1)) : xdup("");
}

// The OpenCourse World config exported from opencourse.world: { "domain": "...", "token": "<22 characters>" }. Only
// the domain is ever shown; the token is counted and dropped (games/smm2/mods/ocw.c holds the rules: a domain of at
// most 29 characters, a token of exactly 22)
static void check_ocw(const char *p, Check *c) {
    if (!*p) { check_text(c, "needed: the config exported from opencourse.world"); return; }
    if (!file_exists(p)) { check_text(c, dir_exists(p) ? "a folder: choose the ocw-config.json file" : "not found"); return; }
    size_t len;
    char *text = read_text(p, &len);
    if (len == 0 || len > 2000) {
        free(text);
        check_text(c, "doesn't look like an OpenCourse World config");
        return;
    }
    char *domain = ocw_field(text, "domain"), *token = ocw_field(text, "token");
    memset(text, 0, len);
    free(text);
    size_t dl = strlen(domain), tl = strlen(token);
    memset(token, 'x', tl);  // (only its length is looked at below)
    free(token);
    if (!dl || !tl) check_text(c, "no domain or no token in it: export the config again from opencourse.world");
    else if (dl > 29) check_text(c, "the domain is longer than the game's own slot allows (29 characters)");
    else if (tl != 22) check_text(c, "the token isn't 22 characters: export the config again from opencourse.world");
    else {
        c->ok = true;
        snprintf(c->text, sizeof c->text, "config found: %s, token set", domain);
    }
    free(domain);
}
static bool ocw_ok(const char *p) {
    Check c = {0};
    check_ocw(p, &c);
    bool ok = c.ok;
    check_free(&c);
    return ok;
}

// Is there a usable config for the game to read: the one RECOMP_OCW_CONFIG names, the one chosen on this screen, or
// ocw-config.json in the data folder (where the game looks without either)
static bool ocw_config_ready(void) {
    if (getenv("RECOMP_OCW_CONFIG") && *getenv("RECOMP_OCW_CONFIG")) return ocw_ok(getenv("RECOMP_OCW_CONFIG"));
    config_load();
    if (g_cfg.ocw[0] && ocw_ok(g_cfg.ocw)) return true;
    return ocw_ok("ocw-config.json");
}

// The checks of what's chosen, at most every 2 s per path (the screen draws 60 times a second; files can appear).
// out: a copy the caller frees with check_free
typedef struct {
    char *key;
    double t;
    Check c;
} CacheEntry;
static void cached_check(char kind, const char *p, CheckFn fn, Check *out) {
    static CacheEntry cache[70];
    static int ncache;
    char *key = xfmt("%c%s", kind, p);
    double now = SDL_GetTicks() / 1000.0;
    for (int i = 0; i < ncache; i++)
        if (!strcmp(cache[i].key, key)) {
            if (now - cache[i].t > 2) {
                check_free(&cache[i].c);
                fn(p, &cache[i].c);
                cache[i].t = now;
            }
            check_clone(out, &cache[i].c);
            free(key);
            return;
        }
    if (ncache > 64) {
        for (int i = 0; i < ncache; i++) free(cache[i].key), check_free(&cache[i].c);
        ncache = 0;
    }
    CacheEntry *e = &cache[ncache++];
    e->key = key;
    e->t = now;
    memset(&e->c, 0, sizeof e->c);
    fn(p, &e->c);
    check_clone(out, &e->c);
}

// ------------------------------------------------------------------ the tools
typedef struct {
    char python[1100], setup[1100], sysdata[1100], hactool[1100];
} Tools;
static bool find_tools(Tools *t, char *why, size_t why_cap) {
    char b[1100];
    if (!gamebuild_python(b, sizeof b)) { snprintf(why, why_cap, "the Python that comes with this program is missing"); return false; }
    snprintf(t->python, sizeof t->python, "%s", b);
    if (!rt_resource("tools" SEP "games" SEP "smm2" SEP "setup.py", "games/smm2/setup.py", b, sizeof b)) {
        snprintf(why, why_cap, "tools/games/smm2/setup.py is missing");
        return false;
    }
    snprintf(t->setup, sizeof t->setup, "%s", b);
    if (!rt_resource("tools" SEP "games" SEP "smm2" SEP "sysdata.py", "games/smm2/sysdata.py", b, sizeof b)) {
        snprintf(why, why_cap, "tools/games/smm2/sysdata.py is missing");
        return false;
    }
    snprintf(t->sysdata, sizeof t->sysdata, "%s", b);
    if (rt_resource("hactool" SEP "hactool" PLAT_EXE_SUFFIX, NULL, b, sizeof b)) snprintf(t->hactool, sizeof t->hactool, "%s", b);
    else t->hactool[0] = 0;  // (a source checkout: the scripts look on PATH and in ~/smm-work/tools/bin)
    return true;
}

// ------------------------------------------------------------------ the setup job (background thread)
typedef enum StepState { PENDING, RUNNING, DONE, SKIPPED, FAILED } StepState;
typedef struct {
    const char *name;
    StepState state;
    char *detail;  // (malloc'd; NULL: none)
} Step;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Step g_steps[3] = {{"Check and lay out your game files", PENDING, NULL},
                          {"Copy the fonts and system data from your firmware", PENDING, NULL},
                          {"Recompile the game code", PENDING, NULL}};
static _Atomic float g_progress;
static _Atomic int g_job;          // 0 idle, 1 running, 2 done (play), 3 failed
static _Atomic bool g_built_wait;  // the game code was just built: Play, not a start
static char *g_err_what, *g_err_hint, *g_err_log;
static StrList g_found;
static _Atomic bool g_cancel;
static PlatProc g_child;

// The log window's lines, like the Build Helper's console: a step's name in blue, what it does in grey, a finished
// step in green, an error in red (kind 0 / 1 / 2 / 3), each with the time since the setup started.
typedef struct {
    unsigned secs;
    int kind;
    char *text;  // (malloc'd)
} LogLine;
static LogLine *g_log;
static int g_nlog;
static _Atomic unsigned g_job_t0;
static void add_log(int kind, const char *text) {
    char *t = xdup(text);
    pthread_mutex_lock(&g_lock);
    g_log = (LogLine *)realloc(g_log, (size_t)(g_nlog + 1) * sizeof(LogLine));
    if (!g_log) abort();
    g_log[g_nlog].secs = (unsigned)((SDL_GetTicks() - g_job_t0) / 1000);
    g_log[g_nlog].kind = kind;
    g_log[g_nlog].text = t;
    g_nlog++;
    pthread_mutex_unlock(&g_lock);
}

static void set_step(int i, StepState s, const char *detail) {  // detail: NULL or "" for none
    if (!detail) detail = "";
    pthread_mutex_lock(&g_lock);
    StepState old = g_steps[i].state;
    char *old_detail = xdup(g_steps[i].detail ? g_steps[i].detail : "");
    g_steps[i].state = s;
    if (*detail || s != RUNNING) set_str(&g_steps[i].detail, detail);
    pthread_mutex_unlock(&g_lock);
    if (s == RUNNING && old != RUNNING) add_log(1, g_steps[i].name);
    if (*detail && strcmp(detail, old_detail) != 0) add_log(s == DONE || s == SKIPPED ? 2 : 0, detail);
    free(old_detail);
}
static void fail(int step, const char *what, const char *hint, const char *log) {  // log: NULL or "" for none
    if (!log) log = "";
    pthread_mutex_lock(&g_lock);
    g_steps[step].state = FAILED;
    set_str(&g_err_what, what), set_str(&g_err_hint, hint), set_str(&g_err_log, log);
    pthread_mutex_unlock(&g_lock);
    add_log(3, what);
    if (*hint) add_log(0, hint);
    if (*log) {
        char *d = xfmt("Details: %s", log);
        add_log(0, d);
        free(d);
    }
    LOG("[launcher] %s failed: %s\n", g_steps[step].name, what);
    g_job = 3;
}

// The argument list of a script (NULL-terminated)
typedef struct {
    const char **v;
    int n;
} ArgV;
static void av_push(ArgV *a, const char *s) {
    a->v = (const char **)realloc((void *)a->v, (size_t)(a->n + 1) * sizeof(char *));
    if (!a->v) abort();
    a->v[a->n++] = s;
}

// Runs a setup script, following its "@" lines (step text, progress lo..hi, what it found, its error). true: it worked.
static bool run_script(int step, ArgV *args, const char *log, float lo, float hi) {
    av_push(args, NULL);
    PlatProc p = plat_spawn(args->v, log, false);
    if (!p) {
        fail(step, "The setup tool didn't start", "Reinstall " GAME_TITLE ": its tools folder may be damaged.", log);
        return false;
    }
    pthread_mutex_lock(&g_lock);
    g_child = p;
    pthread_mutex_unlock(&g_lock);
    int rc;
    size_t seen = 0;
    char *err = NULL;
    for (;;) {
        rc = plat_proc_wait(p, false);
        size_t outlen;
        char *out = read_text(log, &outlen);
        for (char *nl; seen < outlen && (nl = strchr(out + seen, '\n')) != NULL; seen = (size_t)(nl - out) + 1) {
            *nl = 0;
            char *l = out + seen;
            size_t ll = strlen(l);
            if (ll && l[ll - 1] == '\r') l[ll - 1] = 0;
            if (starts_with(l, "@step ")) set_step(step, RUNNING, l + 6);
            else if (starts_with(l, "@progress ")) g_progress = lo + (hi - lo) * (float)atof(l + 10);
            else if (starts_with(l, "@found ")) {
                pthread_mutex_lock(&g_lock);
                strlist_push(&g_found, l + 7);
                pthread_mutex_unlock(&g_lock);
                add_log(0, l + 7);
            } else if (starts_with(l, "@error ")) set_str(&err, l + 7);
        }
        free(out);
        if (rc >= 0) break;
        SDL_Delay(100);
    }
    pthread_mutex_lock(&g_lock);
    g_child = 0;
    pthread_mutex_unlock(&g_lock);
    bool has_err = err && *err;  // (an empty "@error" line counts as none)
    if (rc == 0 && !has_err) {
        free(err);
        return true;
    }
    if (g_cancel) { free(err); return false; }
    char *what = xdup("It stopped with an error"), *hint = xdup("The log has the details.");
    if (has_err) {  // "<code>\t<what>\t<what to do>"
        char *a = strchr(err, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
        if (a) {
            free(what);
            what = b ? xdup_n(a + 1, (size_t)(b - a - 1)) : xdup(a + 1);
        }
        if (b) {
            free(hint);
            hint = xdup(b + 1);
        }
    } else {  // (a Python error: its last line; the scripts never print key values)
        size_t outlen;
        char *out = read_text(log, &outlen);
        while (outlen > 0 && (out[outlen - 1] == '\n' || out[outlen - 1] == '\r')) out[--outlen] = 0;
        const char *nl = strrchr(out, '\n');
        free(what);
        what = xdup(nl ? nl + 1 : out);
        if (strlen(what) > 300) what[300] = 0;
        free(out);
    }
    fail(step, what, hint, log);
    free(what), free(hint), free(err);
    return false;
}

static void build_progress(double f, const char *what) {
    g_progress = 0.25f + 0.75f * (float)f;
    static const char *last;
    if (what != last) last = what, set_step(2, RUNNING, what);
}

static void *job_thread(void *arg) {
    (void)arg;
    Tools t = {{0}};
    char why[256] = "";
    char root[PATHLEN];
    snprintf(root, sizeof root, "%s", g_root);
    g_progress = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < 3; i++) g_steps[i].state = PENDING, set_str(&g_steps[i].detail, "");
    strlist_clear(&g_found);
    for (int i = 0; i < g_nlog; i++) free(g_log[i].text);
    free(g_log);
    g_log = NULL;
    g_nlog = 0;
    g_job_t0 = (unsigned)SDL_GetTicks();
    pthread_mutex_unlock(&g_lock);
    bool need_files = !game_files_ok(root), need_fonts = !fonts_ok();
    // chosen files other than those the game was set up from: set it up again
    if (!need_files) {
        char sp[PATHLEN];
        snprintf(sp, sizeof sp, "%s" SEP "setup.json", root);  // what the game files were set up from (setup.json's sources)
        char *stamp = read_text(sp, NULL);
        for (int i = 0; i < g_cfg.game.n; i++) {
            const char *g = g_cfg.game.v[i];
            char *esc = (char *)xalloc(2 * strlen(g) + 3);  // (as setup.py wrote it: JSON, the path as given)
            size_t n = 0;
            esc[n++] = '"';
            for (const char *c = g; *c; c++) {
                if (*c == '\\' || *c == '"') esc[n++] = '\\';
                esc[n++] = *c;
            }
            esc[n++] = '"';
            esc[n] = 0;
            if (*stamp && !strstr(stamp, esc)) need_files = true;
            free(esc);
        }
        free(stamp);
    }
    if ((need_files || need_fonts) && !find_tools(&t, why, sizeof why)) {
        char *w = xfmt("The setup tools aren't there (%s)", why);
        fail(need_files ? 0 : 1, w, "Download " GAME_TITLE " again and unpack all of it.", NULL);
        free(w);
        return NULL;
    }
    char logs[PATHLEN], logf[PATHLEN + 16];
    snprintf(logs, sizeof logs, "%s" SEP "logs", rt_data_root());
    mkdir(logs, 0755);

    // 1. the game files
    if (need_files) {
        set_step(0, RUNNING, NULL);
        LOG("[launcher] setting up the game files from %zu source(s)\n", (size_t)g_cfg.game.n);
        ArgV a = {NULL, 0};
        av_push(&a, t.python), av_push(&a, t.setup);
        for (int i = 0; i < g_cfg.game.n; i++) av_push(&a, "--source"), av_push(&a, g_cfg.game.v[i]);
        if (g_cfg.keys[0]) av_push(&a, "--keys"), av_push(&a, g_cfg.keys);
        if (t.hactool[0]) av_push(&a, "--hactool"), av_push(&a, t.hactool);
        av_push(&a, "--out"), av_push(&a, root);
        snprintf(logf, sizeof logf, "%s" SEP "setup.log", logs);
        bool ran = run_script(0, &a, logf, 0, 0.2f);
        free((void *)a.v);
        if (!ran) return NULL;
        if (!game_files_ok(root)) {
            fail(0, "The game files weren't set up", "Check the files you chose and try again.", logf);
            return NULL;
        }
        set_step(0, DONE, "Super Mario Maker 2 3.0.3");
    } else {
        set_step(0, SKIPPED, "already set up: Super Mario Maker 2 3.0.3");
    }
    g_progress = 0.2f;

    // 2. fonts and system data
    if (need_fonts) {
        set_step(1, RUNNING, NULL);
        LOG("[launcher] copying the fonts and system data from the firmware\n");
        ArgV a = {NULL, 0};
        av_push(&a, t.python), av_push(&a, t.sysdata);
        av_push(&a, "--firmware"), av_push(&a, g_cfg.firmware), av_push(&a, "--keys"), av_push(&a, g_cfg.keys);
        av_push(&a, "--data"), av_push(&a, rt_data_root());
        if (t.hactool[0]) av_push(&a, "--hactool"), av_push(&a, t.hactool);
        snprintf(logf, sizeof logf, "%s" SEP "sysdata.log", logs);
        bool ran = run_script(1, &a, logf, 0.2f, 0.25f);
        free((void *)a.v);
        if (!ran) return NULL;
        if (!fonts_ok()) {
            fail(1, "The fonts weren't copied", "Check the firmware folder and prod.keys.", logf);
            return NULL;
        }
        set_step(1, DONE, "shared fonts and system data copied");
    } else {
        set_step(1, SKIPPED, "already there");
    }
    g_progress = 0.25f;

    // 3. the game code (setup.py checked the ExeFS against the known 3.0.3 hashes: a library built from it before is
    // still the right one, wherever the files came from)
    if (!gamelib_present(root)) {
        set_step(2, RUNNING, "Starting...");
        if (!gamebuild_available()) {
            fail(2, "The recompiler isn't there", "Download " GAME_TITLE " again and unpack all of it (zig, tools, python).", NULL);
            return NULL;
        }
        int cores = SDL_GetNumLogicalCPUCores(), ram_gb = SDL_GetSystemRAM() / 1024;
        // (a compiler takes up to ~1 GB, and the generators, which run meanwhile, ~4 GB together)
        int jobs = cores < ram_gb - 6 ? cores : ram_gb - 6;
        if (getenv("RECOMP_BUILD_JOBS") && atoi(getenv("RECOMP_BUILD_JOBS")) > 0) jobs = atoi(getenv("RECOMP_BUILD_JOBS"));
        GameBuildOpts o = {build_progress, jobs > 1 ? jobs : 1};
        char w[512] = "";
        LOG("[launcher] recompiling the game code (%d jobs)\n", o.jobs);
        if (!gamebuild_run(root, &o, w, sizeof w)) {
            if (g_cancel) return NULL;
            char *what = xfmt("The game code didn't build: %s", w);
            fail(2, what,
                 "Make sure there are about 4 GB of disk space and 4 GB of memory free, then try again. If it keeps happening, report it on GitHub "
                 "with the log.", plat_log_file());
            free(what);
            return NULL;
        }
        set_step(2, DONE, "built");
    } else {
        set_step(2, SKIPPED, "already built");
    }
    g_progress = 1;
    g_job = 2;
    return NULL;
}

// ------------------------------------------------------------------ update check (notifies only)
static char g_update_tag[256];
static pthread_mutex_t g_update_lock = PTHREAD_MUTEX_INITIALIZER;

static long long tag_rank(const char *t) {  // v<major>.<minor>.<patch> (anything after: the same release)
    int a, b, c;
    if (sscanf(t, "v%d.%d.%d", &a, &b, &c) == 3 && a >= 0 && b >= 0 && c >= 0 && b < 4096 && c < 4096)
        return ((long long)a << 24) + ((long long)b << 12) + c;
    return -1;  // (dev builds: any release is newer)
}
void menu_notice(const char *text, double seconds);  // (menu.c: a notice over the game)
static void *update_thread(void *arg) {
    (void)arg;
    char *body;
    if (getenv("RECOMP_UPDATE_TAGS")) {
        body = xfmt("\"tag_name\": \"%s\"", getenv("RECOMP_UPDATE_TAGS"));
    } else {
        const char *sr = getenv("SystemRoot");
        char *curl = xfmt("%s\\System32\\curl.exe", sr ? sr : "C:\\Windows");
        char dir[PATHLEN], out[PATHLEN + 32];
        snprintf(dir, sizeof dir, "%s" SEP "cache", rt_data_root());
        mkdir(dir, 0755);
        snprintf(out, sizeof out, "%s" SEP "latest-release.json", dir);
        const char *av[] = {curl, "-fsSL", "--max-time", "15", "-H", "Accept: application/vnd.github+json",
                            "-A", "smm2-dx/" RECOMP_VERSION, RELEASES_API, NULL};
        PlatProc p = file_exists(curl) ? plat_spawn(av, out, false) : 0;
        free(curl);
        if (!p || plat_proc_wait(p, true) != 0) { remove(out); return NULL; }
        body = read_text(out, NULL);
        remove(out);
    }
    const char *at = strstr(body, "\"tag_name\"");
    if (!at) { free(body); return NULL; }
    const char *colon = strchr(at, ':');
    const char *q1 = strchr(colon ? colon + 1 : body, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
    if (!q2) { free(body); return NULL; }
    char *tag = xdup_n(q1 + 1, (size_t)(q2 - q1 - 1));
    free(body);
    if (tag_rank(tag) > tag_rank(RECOMP_VERSION)) {
        LOG("[launcher] a newer release is out: %s (this is %s): %s\n", tag, RECOMP_VERSION, RELEASES_PAGE);
        pthread_mutex_lock(&g_update_lock);
        snprintf(g_update_tag, sizeof g_update_tag, "%s", tag);
        pthread_mutex_unlock(&g_update_lock);
        // said over the game too, once it's past its start logos: the Mods tab's row alone went unseen (issue #25)
        while (SDL_GetTicks() < 25000) SDL_Delay(500);
        char note[400];
        snprintf(note, sizeof note, "Version %s is out (this is %s). Menu (F1) > Online: New version.",
                 tag, RECOMP_VERSION);
        menu_notice(note, 12);
    }
    free(tag);
    return NULL;
}
void launcher_update_check_start(void) {
    static bool started;
    if (started || (getenv("RECOMP_NO_UPDATE_CHECK") && !getenv("RECOMP_UPDATE_TAGS"))) return;
    config_load();
    if (!g_cfg.update_check) return;
    started = true;
    pthread_t th;
    if (pthread_create(&th, NULL, update_thread, NULL) == 0) pthread_detach(th);
}
const char *launcher_update_tag(void) {
    static char copy[256];
    pthread_mutex_lock(&g_update_lock);
    snprintf(copy, sizeof copy, "%s", g_update_tag);
    pthread_mutex_unlock(&g_update_lock);
    return copy[0] ? copy : NULL;
}

// The overlay menu's Mods tab, its app section (runtime/ui/menu.c tab_mods): updates and the game files. true: drawn.
void rt_request_reboot(int kind);
void menu_toggle(void);
bool launcher_has_settings(void) { return true; }
bool launcher_settings_rows(void) {
    static bool loaded;
    if (!loaded) loaded = true, config_load();
    opt_section("App");
    bool uc = g_cfg.update_check;
    if (opt_toggle("Check for updates", &uc, true,
                   "Asks GitHub for the newest release of " GAME_TITLE " at each start and says when there is one. "
                   "Nothing is downloaded or installed by itself.")) {
        g_cfg.update_check = uc;
        config_save();
        if (uc) launcher_update_check_start();
    }
    const char *tag = launcher_update_tag();
    if (tag) {
        char d[300];
        snprintf(d, sizeof d, "Version %s is out (this is %s). Opens its download page in your browser.", tag, RECOMP_VERSION);
        if (opt_action("New version available", tag, d, true)) SDL_OpenURL(RELEASES_PAGE);
    }
    if (opt_action("Game files", "Set up...", "Back to the setup screen: choose other game files, keys or "
                   "firmware. Progress since your last save is lost.", true))
        rt_request_reboot(2 /* RT_REBOOT_TITLE */), menu_toggle();
    return true;
}

// ------------------------------------------------------------------ file dialogs (SDL calls back on any thread)
typedef enum PickTarget { PICK_GAME, PICK_KEYS, PICK_FIRMWARE, PICK_OCW } PickTarget;
typedef struct {
    int target;
    char *path;  // (malloc'd)
} Picked;
static Picked *g_picked;
static int g_npicked;
static void SDLCALL picked(void *target, const char *const *files, int filter) {
    (void)filter;
    if (!files) return;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; files[i]; i++) {
        g_picked = (Picked *)realloc(g_picked, (size_t)(g_npicked + 1) * sizeof(Picked));
        if (!g_picked) abort();
        g_picked[g_npicked].target = (int)(intptr_t)target;
        g_picked[g_npicked].path = xdup(files[i]);
        g_npicked++;
    }
    pthread_mutex_unlock(&g_lock);
}
static void dialog_start(const char *p, char *out, size_t cap) {
    if (dir_exists(p)) { snprintf(out, cap, "%s", p); return; }
    const char *s = NULL;
    for (const char *c = p; *c; c++)
        if (*c == '/' || *c == '\\') s = c;
    if (!s) snprintf(out, cap, "%s", home_dir());
    else snprintf(out, cap, "%.*s", (int)(s - p), p);
}

// ------------------------------------------------------------------ screenshots (tests: RECOMP_LAUNCHER_SHOTS)

// ------------------------------------------------------------------ drawing
static ImFont *g_font, *g_font_big, *g_font_title;
// The Build Helper console's colours, exactly as its terminal shows them (the SGR codes it prints, in Windows
// Terminal's default scheme): 36 cyan = the step lines and bar, 33 yellow = the name, 90 grey = details, 32 green = done,
// 31 red = errors, 35 magenta = the rule under the name, 97 white = the title, on the console's own black.
#define RGB8(r, g, b) {(r) / 255.0f, (g) / 255.0f, (b) / 255.0f, 1}
static const ImVec4 GOLD = RGB8(193, 156, 0), BLUE = RGB8(58, 150, 221), DIM = RGB8(118, 118, 118),
                    GOOD = RGB8(19, 161, 14), WARN = RGB8(193, 156, 0), BAD = RGB8(197, 15, 31),
                    MAGENTA = RGB8(136, 23, 152), WHITE = RGB8(242, 242, 242), TEXT = RGB8(204, 204, 204),
                    BLACK = RGB8(12, 12, 12);

static ImVec4 shade(ImVec4 c, float toward_white) {  // (a hover is the colour a quarter lighter, a press a quarter darker)
    return toward_white >= 0 ? IM_VEC4(c.x + (1 - c.x) * toward_white, c.y + (1 - c.y) * toward_white, c.z + (1 - c.z) * toward_white, 1)
                             : IM_VEC4(c.x * (1 + toward_white), c.y * (1 + toward_white), c.z * (1 + toward_white), 1);
}

// The window's content right edge in window coordinates (what ImGui::GetWindowContentRegionMax().x gave)
static float window_content_max_x(void) {
    ImGuiWindow *w = igGetCurrentWindow();
    return w->ContentRegionRect.Max.x - w->Pos.x;
}

// Square, flat, the console's black, cyan header bars, magenta-edged boxes, small "?" tooltips
static void style_init(void) {
    ImGuiStyle *s = igGetStyle();
    igStyleColorsDark(NULL);
    s->WindowRounding = s->FrameRounding = s->GrabRounding = s->ChildRounding = s->PopupRounding = s->ScrollbarRounding = 0;
    s->WindowPadding = IM_VEC2(10, 10), s->FramePadding = IM_VEC2(8, 5), s->ItemSpacing = IM_VEC2(8, 6);
    s->WindowBorderSize = 0, s->ChildBorderSize = 1, s->FrameBorderSize = 0, s->ScrollbarSize = 14;
    ImVec4 *c = s->Colors;
    c[ImGuiCol_WindowBg] = BLACK;
    c[ImGuiCol_ChildBg] = IM_VEC4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = BLACK;
    c[ImGuiCol_Border] = BLUE;
    c[ImGuiCol_Text] = TEXT;
    c[ImGuiCol_TextDisabled] = DIM;
    c[ImGuiCol_Header] = BLUE;
    c[ImGuiCol_HeaderHovered] = shade(BLUE, 0.25f);
    c[ImGuiCol_HeaderActive] = shade(BLUE, -0.25f);
    c[ImGuiCol_FrameBg] = IM_VEC4(DIM.x, DIM.y, DIM.z, 0.25f);
    c[ImGuiCol_FrameBgHovered] = IM_VEC4(DIM.x, DIM.y, DIM.z, 0.40f);
    c[ImGuiCol_FrameBgActive] = IM_VEC4(DIM.x, DIM.y, DIM.z, 0.55f);
    c[ImGuiCol_Button] = BLUE;
    c[ImGuiCol_ButtonHovered] = shade(BLUE, 0.25f);
    c[ImGuiCol_ButtonActive] = shade(BLUE, -0.25f);
    c[ImGuiCol_CheckMark] = BLUE;
    c[ImGuiCol_SliderGrab] = BLUE;
    c[ImGuiCol_NavCursor] = GOLD;
    c[ImGuiCol_PlotHistogram] = BLUE;
    c[ImGuiCol_Separator] = MAGENTA;
    c[ImGuiCol_ScrollbarBg] = BLACK;
    c[ImGuiCol_ScrollbarGrab] = DIM;
}

// The Build Helper's bar: [#####---------------]  25%  what it is doing (written into out)
static void text_bar(float f, const char *what, char *out, size_t cap) {
    int pct = (int)(f * 100 + 0.5f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int fill = pct / 5;
    snprintf(out, cap, "[%.*s%.*s] %3d%%  %s", fill, "####################", 20 - fill, "--------------------", pct, what);
}

static void wrapped(ImVec4 col, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    igPushStyleColor_Vec4(ImGuiCol_Text, col);
    igPushTextWrapPos(0);
    igTextUnformatted(buf, NULL);
    igPopTextWrapPos();
    igPopStyleColor(1);
}

static void status_line(const Check *c, float indent) {
    igSetCursorPosX(igGetCursorPosX() + indent);  // (under the path field)
    igPushStyleColor_Vec4(ImGuiCol_Text, c->ok ? (c->warn ? WARN : GOOD) : BAD);
    igTextUnformatted(c->ok ? (c->warn ? "!" : "OK") : "X", NULL);
    igPopStyleColor(1);
    igSameLine(0, -1);
    wrapped(c->ok ? DIM : BAD, "%s", c->text);
}

// A path row: the field (typed or pasted) and Browse. true when it changed.
static bool path_row(const char *id, char *path, size_t cap, const char *hint, PickTarget target, bool folder, float s) {
    igPushID_Str(id);
    igAlignTextToFramePadding();
    igTextUnformatted(id, NULL);
    igSameLine(130 * s, -1);
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", path);
    float bw = 150 * s;
    igSetNextItemWidth(-bw - igGetStyle()->ItemSpacing.x);
    bool changed = false;
    if (ui_input_text("##path", buf, sizeof buf, 0, hint, id)) trim_into(path, cap, buf), changed = true;
    igSameLine(0, -1);
    if (igButton("Browse...", IM_VEC2(bw, 0))) {
        char start[PATHLEN];
        dialog_start(path[0] ? path : home_dir(), start, sizeof start);
        if (folder) SDL_ShowOpenFolderDialog(picked, (void *)(intptr_t)target, ui_window(), start, false);
        else if (target == PICK_OCW) {
            static const SDL_DialogFileFilter ocw_filter[] = {{"OpenCourse World config", "json"}, {"All files", "*"}};
            SDL_ShowOpenFileDialog(picked, (void *)(intptr_t)target, ui_window(), ocw_filter, 2, start, false);
        } else {
            static const SDL_DialogFileFilter keys_filter[] = {{"Keys", "keys;txt"}, {"All files", "*"}};
            SDL_ShowOpenFileDialog(picked, (void *)(intptr_t)target, ui_window(), keys_filter, 2, start, false);
        }
    }
    igPopID();
    return changed;
}

// The coffee link: the whole line is the link
static void coffee_line(void) {
    static const char *url = "https://buymeacoffee.com/sometimes_steve";
    igTextColored(GOLD, "If you enjoy my work, consider buying me a coffee! %s", url);
    if (igIsItemHovered(0)) {
        ImVec2 mn, mx;
        igGetItemRectMin(&mn);
        igGetItemRectMax(&mx);
        igSetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList_AddLine(igGetWindowDrawList(), IM_VEC2(mn.x, mx.y), mx, igGetColorU32_Vec4(GOLD), 1.0f);
    }
    if (igIsItemClicked(0)) SDL_OpenURL(url);
}

// The braille art, dot by dot (the font has no braille): pinned at the left like the Build Helper's console
static void draw_art(ImVec2 pos, float maxw, float maxh) {
    static const int DX[8] = {0, 0, 0, 1, 1, 1, 0, 1}, DY[8] = {0, 1, 2, 0, 1, 2, 3, 3};
    float step = fminf(maxw / (ART_COLS * 2), maxh / (ART_ROWS * 4));
    if (step < 1.5f) return;
    float r = step * 0.42f;
    ImDrawList *dl = igGetWindowDrawList();
    ImU32 col = igGetColorU32_Vec4(MAGENTA);
    for (int y = 0; y < ART_ROWS; y++)
        for (int x = 0; x < ART_COLS; x++)
            for (int b = 0; b < 8; b++)
                if (ART[y][x] & (1 << b))
                    ImDrawList_AddCircleFilled(dl, IM_VEC2(pos.x + ((x * 2 + DX[b]) + 0.5f) * step, pos.y + ((y * 4 + DY[b]) + 0.5f) * step), r, col, 6);
}

// A section: a blue header bar over a bordered box. true: open (EndChild when done)
static bool section_begin(const char *id, const char *title) {
    igPushStyleColor_Vec4(ImGuiCol_Text, WHITE);
    bool open = igCollapsingHeader_TreeNodeFlags(title, ImGuiTreeNodeFlags_DefaultOpen);
    igPopStyleColor(1);
    // (NavFlattened as a child flag: the window flag of that name is obsolete and ImGui turns it into this one)
    if (open)
        igBeginChild_Str(id, IM_VEC2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                         0);
    return open;
}

static bool big_button(const char *label, bool enabled, float w, float h) {
    igBeginDisabled(!enabled);
    igPushStyleColor_Vec4(ImGuiCol_Button, GOLD);
    igPushStyleColor_Vec4(ImGuiCol_ButtonHovered, shade(GOLD, 0.25f));
    igPushStyleColor_Vec4(ImGuiCol_ButtonActive, shade(GOLD, -0.25f));
    igPushStyleColor_Vec4(ImGuiCol_Text, BLACK);
    igPushFont(g_font_big);
    bool r = igButton(label, IM_VEC2(w, h));
    igPopFont();
    igPopStyleColor(4);
    igEndDisabled();
    return r;
}

// ------------------------------------------------------------------ the screen
void launcher_run(const char *root) {
    snprintf(g_root, sizeof g_root, "%s", root);
    config_load();
    // suggestions for what isn't chosen yet: the keys and firmware an emulator already has
    {
        char ad[PATHLEN], cand[PATHLEN + 64];
        appdata_dir(ad, sizeof ad);
        if (!g_cfg.keys[0])
            for (int i = 0; i < 6; i++) {
                snprintf(cand, sizeof cand, "%s%s", i < 5 ? ad : home_dir(), i < 5 ? KEYS_SUFFIX[i] : KEYS_HOME_SUFFIX);
                if (file_exists(cand)) { snprintf(g_cfg.keys, sizeof g_cfg.keys, "%s", cand); break; }
            }
        if (!g_cfg.firmware[0])
            for (int i = 0; i < 3; i++) {
                snprintf(cand, sizeof cand, "%s%s", ad, FIRMWARE_SUFFIX[i]);
                if (dir_exists(cand)) { snprintf(g_cfg.firmware, sizeof g_cfg.firmware, "%s", cand); break; }
            }
    }
    launcher_update_check_start();
    // the auto-fill of a test run
    const char *autof = getenv("RECOMP_LAUNCHER_AUTO");
    if (autof) {
        strlist_clear(&g_cfg.game);
        size_t alen = strlen(autof);
        for (size_t i = 0; i <= alen;) {
            const char *bar = strchr(autof + i, '|');
            size_t e = bar ? (size_t)(bar - autof) : alen;
            char *kv = xdup_n(autof + i, e - i);
            char *eq = strchr(kv, '=');
            const char *k = kv, *v = "";
            if (eq) *eq = 0, v = eq + 1;
            if (!strcmp(k, "game")) strlist_push(&g_cfg.game, v);
            else if (!strcmp(k, "keys")) snprintf(g_cfg.keys, sizeof g_cfg.keys, "%s", v);
            else if (!strcmp(k, "firmware")) snprintf(g_cfg.firmware, sizeof g_cfg.firmware, "%s", v);
            else if (!strcmp(k, "ocw")) snprintf(g_cfg.ocw, sizeof g_cfg.ocw, "%s", v);
            free(kv);
            i = e + 1;
        }
    }
    const char *shots = getenv("RECOMP_LAUNCHER_SHOTS");

    ui_window_init();
    SDL_Window *win = ui_window();
    SDL_SetWindowTitle(win, launcher_needed(root, false) ? "smm2-dx - First-Time Setup" : GAME_TITLE);
    igDebugCheckVersionAndDataLayout("1.91.9", sizeof(ImGuiIO), sizeof(ImGuiStyle), sizeof(ImVec2), sizeof(ImVec4),
                                     sizeof(ImDrawVert), sizeof(ImDrawIdx));  // (IMGUI_CHECKVERSION)
    ImGuiContext *prev = igGetCurrentContext();  // (the overlay menu's, after Quit to Start Screen)
    ImGuiContext *ctx = igCreateContext(NULL);
    igSetCurrentContext(ctx);
    ImGuiIO *io = igGetIO();
    io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io->IniFilename = NULL;
    {
        int ww, wh;
        SDL_GetWindowSizeInPixels(win, &ww, &wh);
        float atlas = wh > 1080 ? 2.0f : 1.0f;  // (sharp text on big windows)
        ImFontConfig *cfg = ImFontConfig_ImFontConfig();
        cfg->OversampleH = 1;
        cfg->PixelSnapH = true;
        cfg->SizePixels = 17 * atlas;
        g_font = ImFontAtlas_AddFontDefault(io->Fonts, cfg);
        cfg->SizePixels = 24 * atlas;
        g_font_big = g_font_title = ImFontAtlas_AddFontDefault(io->Fonts, cfg);
        ImFontConfig_destroy(cfg);
        io->FontGlobalScale = 1 / atlas;
    }
    style_init();
    ImGui_ImplSDL3_InitForVulkan(win);
    vk_imgui_backend_init();
    ImGuiStyle base = *igGetStyle();
    float style_scale = -1;

    bool ready_at_start = !launcher_needed(root, false);  // (back here from the game: everything is there)
    bool play = false, quit = false;
    int frame = 0, shot_n = 0, last_shot_key = -1;
    double last_shot_t = 0;
    g_job = 0;
    char add_path[4096] = "";

    while (!play) {
        // picks from the file dialogs
        pthread_mutex_lock(&g_lock);
        Picked *picks = g_picked;
        int npicks = g_npicked;
        g_picked = NULL;
        g_npicked = 0;
        pthread_mutex_unlock(&g_lock);
        for (int i = 0; i < npicks; i++) {
            const char *pp = picks[i].path;
            if (picks[i].target == PICK_GAME) strlist_push(&g_cfg.game, pp);
            if (picks[i].target == PICK_KEYS) snprintf(g_cfg.keys, sizeof g_cfg.keys, "%s", pp);
            if (picks[i].target == PICK_FIRMWARE) snprintf(g_cfg.firmware, sizeof g_cfg.firmware, "%s", pp);
            if (picks[i].target == PICK_OCW) snprintf(g_cfg.ocw, sizeof g_cfg.ocw, "%s", pp);
            config_save();
            free(picks[i].path);
        }
        free(picks);
        menu_apply_window_requests(win);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) quit = true;
        }
        if (quit) {
            pthread_mutex_lock(&g_lock);
            g_cancel = true;
            if (g_child) plat_proc_kill(g_child), g_child = 0;
            pthread_mutex_unlock(&g_lock);
            LOG("window closed\n");
            exit(0);
        }
        if (g_job == 2) { play = true; break; }

        {
            // one frame per refresh: the swapchain presents without waiting, so this screen alone would spin
            static uint64_t next_frame;
            const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
            uint64_t period = (uint64_t)(1e9 / (dm && dm->refresh_rate > 1 ? dm->refresh_rate : 60.0f)), now = plat_now_ns();
            bool on_time = next_frame > now && next_frame - now <= period;
            if (on_time) plat_sleep_until_ns(next_frame);
            next_frame = (on_time ? next_frame : now) + period;
            static const float clear_col[4] = {12 / 255.0f, 12 / 255.0f, 12 / 255.0f, 1};
            if (!vk_frame_begin(clear_col)) { SDL_Delay(16); continue; }
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            igNewFrame();
            ImVec2 ds = io->DisplaySize;
            float s = ds.y / 720.0f;
            if (s < 0.6f) s = 0.6f;
            if (fabsf(style_scale - s) > 0.01f) {
                *igGetStyle() = base;
                ImGuiStyle_ScaleAllSizes(igGetStyle(), s);
                style_scale = s;
            }
            igSetNextWindowPos(IM_VEC2(0, 0), 0, IM_VEC2(0, 0));
            igSetNextWindowSize(ds, 0);
            igBegin("##launcher", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
            igSetWindowFontScale(s);
            int job = g_job;

            float footer = 146 * s;
            ImVec2 avail;
            igGetContentRegionAvail(&avail);
            float body_h = avail.y - footer;
            float art_w = ds.x < 760 * s ? 0 : fminf(ds.x * 0.27f, 330 * s);  // (a narrow window: no art)
            if (art_w > 0) {
                // the banner, like the Build Helper's: name and title, version, author; the art sits under it
                ImVec2 c0, sp;
                igGetCursorPos(&c0);
                igTextColored(GOLD, "smm2-dx");
                igSameLine(0, 16 * s);
                igTextColored(WHITE, "First-Time Setup");
                igTextDisabled("%s", RECOMP_VERSION);
                igTextColored(GOLD, "by sometimes_steve");
                float banner_h = igGetCursorPosY() - c0.y + 12 * s;
                igSetCursorPos(c0);
                igGetCursorScreenPos(&sp);
                draw_art(IM_VEC2(sp.x, sp.y + banner_h), art_w, body_h - banner_h);
                igSetCursorPosX(igGetCursorPosX() + art_w + igGetStyle()->ItemSpacing.x);
            }
            igBeginChild_Str("##body", IM_VEC2(0, body_h), ImGuiChildFlags_NavFlattened,
                             job == 0 ? 0 : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            Check *gchecks = (Check *)xalloc((size_t)(g_cfg.game.n ? g_cfg.game.n : 1) * sizeof(Check));
            int ngchecks = g_cfg.game.n;
            for (int i = 0; i < ngchecks; i++) {
                memset(&gchecks[i], 0, sizeof(Check));
                cached_check('g', g_cfg.game.v[i], check_game_source, &gchecks[i]);
            }
            Check keys, fw, ocwc;
            cached_check('k', g_cfg.keys, check_keys, &keys);
            cached_check('f', g_cfg.firmware, check_firmware, &fw);
            bool games_ok = ngchecks > 0;
            for (int i = 0; i < ngchecks; i++) games_ok &= gchecks[i].ok;
            cached_check('o', g_cfg.ocw, check_ocw, &ocwc);
            bool fonts_needed = !fonts_ok(), can_start = games_ok && ocwc.ok && (!fonts_needed || (keys.ok && fw.ok));
            bool files_ready = ready_at_start && g_cfg.game.n != 0 && ocwc.ok;
            if (job == 0) {
                // ---- the game: folders with the decrypted files
                if (section_begin("##c1", "Game files")) {
                    igTextDisabled("Super Mario Maker 2 3.0.3");
                    int remove_at = -1;
                    for (int i = 0; i < g_cfg.game.n; i++) {
                        // one line each: the check's mark, the name (the whole path on hover), what it is, Remove
                        igPushID_Int(i);
                        const Check *c = &gchecks[i];
                        igAlignTextToFramePadding();
                        igTextColored(c->ok ? (c->warn ? WARN : GOOD) : BAD, "%s", c->ok ? (c->warn ? "!" : "OK") : "X");
                        igSameLine(0, -1);
                        igTextUnformatted(base_name(g_cfg.game.v[i]), NULL);
                        if (igIsItemHovered(0)) igSetTooltip("%s", g_cfg.game.v[i]);
                        igSameLine(0, -1);
                        igTextColored(c->ok ? (c->warn ? WARN : GOOD) : BAD, "- %s", c->text);
                        float bw = 100 * s;
                        igSameLine(window_content_max_x() - bw, -1);
                        if (igButton("Remove", IM_VEC2(bw, 0))) remove_at = i;
                        for (int li = 0; li < c->nlines; li++) {  // what was found and what it was compared with
                            const CheckLine *l = &c->lines[li];
                            igSetCursorPosX(igGetCursorPosX() + 30 * s);
                            igTextColored(l->kind == 0 ? GOOD : l->kind == 1 ? WARN : DIM, "%s", l->text);
                        }
                        igPopID();
                    }
                    if (remove_at >= 0) strlist_remove(&g_cfg.game, remove_at), config_save();
                    if (g_cfg.game.n == 0) igTextColored(WARN, "Nothing chosen yet.");
                    if (igButton("Add folder...", IM_VEC2(0, 0))) {
                        char st[PATHLEN];
                        if (g_cfg.game.n == 0) snprintf(st, sizeof st, "%s", home_dir());
                        else dialog_start(g_cfg.game.v[g_cfg.game.n - 1], st, sizeof st);
                        SDL_ShowOpenFolderDialog(picked, (void *)(intptr_t)PICK_GAME, win, st, true);
                    }
                    igSameLine(0, -1);
                    if (igButton("Add NSP / XCI...", IM_VEC2(0, 0))) {
                        char st[PATHLEN];
                        if (g_cfg.game.n == 0) snprintf(st, sizeof st, "%s", home_dir());
                        else dialog_start(g_cfg.game.v[g_cfg.game.n - 1], st, sizeof st);
                        static const SDL_DialogFileFilter game_filter[] = {{"Game files", "nsp;xci"}, {"All files", "*"}};
                        SDL_ShowOpenFileDialog(picked, (void *)(intptr_t)PICK_GAME, win, game_filter, 2, st, true);
                    }
                    igSameLine(0, -1);
                    {
                        char buf[4096], trimmed[4096];
                        snprintf(buf, sizeof buf, "%s", add_path);
                        float aw = 90 * s;
                        ImVec2 av;
                        igGetContentRegionAvail(&av);
                        igSetNextItemWidth(av.x - aw - igGetStyle()->ItemSpacing.x);
                        if (ui_input_text("##add", buf, sizeof buf, 0, "or type / paste a folder path", "Game files"))
                            snprintf(add_path, sizeof add_path, "%s", buf);
                        igSameLine(0, -1);
                        trim_into(trimmed, sizeof trimmed, add_path);
                        igBeginDisabled(trimmed[0] == 0);
                        if (igButton("Add", IM_VEC2(aw, 0))) {
                            trim_into(trimmed, sizeof trimmed, add_path);
                            strlist_push(&g_cfg.game, trimmed), add_path[0] = 0, config_save();
                        }
                        igEndDisabled();
                    }
                    igEndChild();
                }
                igSpacing();

                // ---- keys and firmware: for the shared fonts
                if (section_begin("##c2", "Keys and firmware")) {
                    if (path_row("prod.keys", g_cfg.keys, sizeof g_cfg.keys, "prod.keys: its path", PICK_KEYS, false, s)) config_save();
                    status_line(&keys, 130 * s);
                    if (path_row("Firmware", g_cfg.firmware, sizeof g_cfg.firmware, "firmware: e.g. Ryujinx's bis/system/Contents/registered", PICK_FIRMWARE, true, s))
                        config_save();
                    status_line(&fw, 130 * s);
                    igEndChild();
                }
                igSpacing();

                // ---- OpenCourse World: Course World and every online feature of the game
                if (section_begin("##c3", "OpenCourse World")) {
                    igTextDisabled("The config exported from your opencourse.world account");
                    if (path_row("Config", g_cfg.ocw, sizeof g_cfg.ocw, "ocw-config.json: its path", PICK_OCW, false, s)) config_save();
                    status_line(&ocwc, 130 * s);
                    igEndChild();
                }
            } else {
                // ---- the log: what the Build Helper's console shows, in a window of its own
                pthread_mutex_lock(&g_lock);
                int nlines = g_nlog;
                LogLine *lines = (LogLine *)xalloc((size_t)(nlines ? nlines : 1) * sizeof(LogLine));
                for (int i = 0; i < nlines; i++) {
                    lines[i] = g_log[i];
                    lines[i].text = xdup(g_log[i].text);
                }
                pthread_mutex_unlock(&g_lock);
                igBeginChild_Str("##log", IM_VEC2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                                 0);
                bool at_end = igGetScrollY() >= igGetScrollMaxY() - 4 * s;
                for (int i = 0; i < nlines; i++) {
                    const LogLine *l = &lines[i];
                    igPushStyleColor_Vec4(ImGuiCol_Text, l->kind == 1 ? BLUE : l->kind == 2 ? GOOD : l->kind == 3 ? BAD : DIM);
                    igPushTextWrapPos(0);
                    if (l->kind == 1) igText("[%02u:%02u] %s", l->secs / 60, l->secs % 60, l->text);
                    else igText("  %s", l->text);
                    igPopTextWrapPos();
                    igPopStyleColor(1);
                }
                static int shown;
                if (at_end && nlines != shown) igSetScrollHereY(1.0f);
                shown = nlines;
                igEndChild();
                for (int i = 0; i < nlines; i++) free(lines[i].text);
                free(lines);
            }
            igEndChild();
            igSetCursorPosX(igGetStyle()->WindowPadding.x);

            // ---- footer: what is going on at the left, the button at the right (Setup, or Play when back from the game)
            igSpacing();
            float bh = 56 * s, bw = 220 * s;
            float right = window_content_max_x() - bw;
            ImVec2 isz;
            if (job == 0) {
                igBeginGroup();
                if (!can_start && !files_ready)
                    igTextColored(WARN, "%s", g_cfg.game.n == 0 ? "Add your game: its NSP / XCI, or the folder with the decrypted files."
                                              : !games_ok ? "One of the game files or folders can't be used."
                                              : !ocwc.ok ? "Choose your OpenCourse World config (ocw-config.json)."
                                                         : "Choose prod.keys and the firmware: the fonts come from there.");
                else if (g_built_wait)
                    igTextColored(GOOD, "The game code is ready. Press Play (on a Steam Deck, Gaming Mode is best).");
                else
                    igTextColored(GOOD, "%s", files_ready && !fonts_needed ? "Everything is set up."
                                              : "Ready to set up: the first time takes a while.");
                bool uc = g_cfg.update_check;
                if (igCheckbox("Check for updates", &uc)) {
                    g_cfg.update_check = uc, config_save();
                    if (uc) launcher_update_check_start();
                }
                igSameLine(0, 24 * s);
                igTextDisabled("%s", RECOMP_VERSION);
                const char *tag = launcher_update_tag();
                if (tag) {
                    igSameLine(0, 24 * s);
                    igTextColored(GOLD, "Version %s is out.", tag);
                    igSameLine(0, -1);
                    if (igSmallButton("Open the download page")) SDL_OpenURL(RELEASES_PAGE);
                }
                igEndGroup();
                igGetItemRectSize(&isz);
                float gh = isz.y;
                igSameLine(right, -1);
                igSetCursorPosY(igGetCursorPosY() + (gh > bh ? (gh - bh) / 2 : 0));
                const char *label = (files_ready || g_built_wait) && !fonts_needed ? "Play" : "Setup";
                bool go = big_button(label, can_start || files_ready, bw, bh);
                if (autof && frame == 45 && (can_start || files_ready)) go = true;  // (test: as if pressed)
                if (go) {
                    config_save();
                    g_cancel = false;
                    g_job = 1;
                    pthread_t th;
                    if (pthread_create(&th, NULL, job_thread, NULL) == 0) pthread_detach(th);
                    else g_job = 0;
                }
            } else if (job == 1) {
                char what[640] = "", bar[1400];
                pthread_mutex_lock(&g_lock);
                for (int i = 0; i < 3; i++)
                    if (g_steps[i].state == RUNNING) {
                        snprintf(what, sizeof what, "%s", g_steps[i].detail && *g_steps[i].detail ? g_steps[i].detail : g_steps[i].name);
                        break;
                    }
                pthread_mutex_unlock(&g_lock);
                text_bar(g_progress, what, bar, sizeof bar);
                igTextColored(BLUE, "%s", bar);
                igBeginGroup();
                igTextDisabled("Closing the window stops it; it starts over next time.");
                igEndGroup();
                igGetItemRectSize(&isz);
                float gh = isz.y;
                igSameLine(right, -1);
                igSetCursorPosY(igGetCursorPosY() + (gh > bh ? (gh - bh) / 2 : 0));
                igBeginDisabled(true);
                big_button("Setting up...", false, bw, bh);
                igEndDisabled();
            } else {
                igBeginGroup();
                igTextColored(BAD, "Setup stopped.");
                igEndGroup();
                igGetItemRectSize(&isz);
                float gh = isz.y;
                igSameLine(right, -1);
                igSetCursorPosY(igGetCursorPosY() + (gh > bh ? (gh - bh) / 2 : 0));
                if (big_button("Back", true, bw, bh)) g_job = 0;
            }
            // pinned to the very bottom edge of the window (never above the footer)
            float coffee_y = igGetWindowHeight() - igGetStyle()->WindowPadding.y - igGetTextLineHeight();
            if (coffee_y > igGetCursorPosY()) igSetCursorPosY(coffee_y);
            coffee_line();
            igEnd();
            osk_draw(g_font_big, s);
            igRender();
            for (int i = 0; i < ngchecks; i++) check_free(&gchecks[i]);
            free(gchecks);
            check_free(&keys), check_free(&fw), check_free(&ocwc);

            // screenshots: whenever what's shown changes, and every 30 s while a step runs
            const char *shot = NULL;
            char shot_path[1200];
            if (shots) {
                int key = job * 100;
                pthread_mutex_lock(&g_lock);
                for (int i = 0; i < 3; i++) key += g_steps[i].state * (i == 0 ? 1 : i == 1 ? 5 : 25);
                pthread_mutex_unlock(&g_lock);
                double tnow = SDL_GetTicks() / 1000.0;
                if (frame >= 30 && (key != last_shot_key || (job == 1 && tnow - last_shot_t > 30))) {
                    last_shot_key = key, last_shot_t = tnow;
                    snprintf(shot_path, sizeof shot_path, "%s/launcher_%02d_%s.png", shots, shot_n++,
                             job == 0 ? "choose" : job == 1 ? "running" : job == 3 ? "error" : "done");
                    shot = shot_path;
                }
            }
            ImGui_ImplVulkan_RenderDrawData(igGetDrawData(), (VkCommandBuffer)vk_frame_cmd(), VK_NULL_HANDLE);
            vk_frame_end(shot);
            if (shot) LOG("[launcher] screenshot %s\n", shot);
        }
        frame++;
    }
    LOG("[launcher] ready: starting the game\n");
    config_save();
    launcher_boot_env();
    vk_imgui_backend_shutdown();
    ImGui_ImplSDL3_Shutdown();
    igDestroyContext(ctx);
    igSetCurrentContext(prev);
    SDL_SetWindowTitle(win, GAME_TITLE);
}
