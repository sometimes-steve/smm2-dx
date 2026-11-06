// The setup screen (start screen), shown in place of the game until it is set up: nothing of the game ships with the
// app, so the player points it at their own dump, and the app prepares everything from it on their machine, then runs
// the game. Modelled on melee-pc's launcher (DECISIONS.md, "Releases: a launcher plus recompiler per platform"); the
// window, render loop and update check follow LAS Recomp's start screen (UPSTREAM.md).
//
//   1. Your game: Super Mario Maker 2 with the 3.0.3 update - folders of the decrypted, extracted ExeFS and RomFS
//      (no NSP / XCI: those need keys). games/smm2/setup.py checks the version (build ids and hashes of the player's
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
// The update check asks GitHub for the project's latest release (a background curl, which macOS, Windows 10+ and
// Linux distributions ship) and only says that there is one, with a link to its page: nothing is downloaded.
//
// Tests: RECOMP_LAUNCHER_AUTO="game=<path>|game=<path>|keys=<file>|firmware=<dir>" fills the page in and
// presses Set up after a moment (as the player would); RECOMP_LAUNCHER_SHOTS=<dir> saves the window as PNG whenever
// the screen changes (and every 30 s while a step runs); RECOMP_NO_UPDATE_CHECK=1; RECOMP_UPDATE_TAGS=<tag>: pretend
// that release exists (no network).
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "game.h"
#include "paths.h"
}

#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__ANDROID__)
// Mobile builds carry the game code in the app (made on a desktop from the player's files): no setup screen.
extern "C" bool launcher_needed(const char *, bool) { return false; }
extern "C" void launcher_run(const char *) {}
extern "C" void launcher_boot_env(void) {}
extern "C" void launcher_update_check_start(void) {}
extern "C" const char *launcher_update_tag(void) { return NULL; }
extern "C" bool launcher_settings_rows(void) { return false; }
extern "C" bool launcher_has_settings(void) { return false; }
#else

#ifdef __APPLE__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <sys/xattr.h>
#endif
#include <SDL3/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
#endif
#include "launcher_art.h"
#include "option_rows.h"
#include "osk.h"

extern "C" {
#include "gamebuild.h"
#include "gamelib.h"
#include "platform.h"
void rt_log(const char *fmt, ...);  // runtime/rt.h LOG
void ui_window_init(void);
SDL_Window *ui_window(void);
void menu_apply_window_requests(void *sdl_window);
#ifdef __APPLE__
void *ui_metal_layer(void);
void *ui_metal_device(void);
#else
bool vk_frame_begin(const float clear[4]);
void vk_frame_end(const char *shot_path);
void *vk_frame_cmd(void);
bool vk_imgui_backend_init(void);
void vk_imgui_backend_shutdown(void);
#endif
}

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

#define LOG(...) rt_log(__VA_ARGS__)
#ifndef RECOMP_VERSION
#define RECOMP_VERSION "dev"
#endif
#define RELEASES_API "https://api.github.com/repos/sometimes-steve/smm2-dx/releases/latest"
#define RELEASES_PAGE "https://github.com/sometimes-steve/smm2-dx/releases"

static const char MAIN_303[] = "c2dc405ac414c37c8b1c50219c7a0f0c";  // (games/smm2/versions.py: v3.0.3 main build id)
static const char MAIN_100[] = "3ebb1eb2fffb029a8b87579b961a7b";    // (v1.0.0: the base game without the update)
static const char *FONT_STD = "fonts" SEP "nintendo_ext_003.bfttf";  // (the button glyph font: the one the game cannot do without)

static bool file_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
static bool dir_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
static std::string home_dir(void) {
#ifdef _WIN32
    const char *h = getenv("USERPROFILE");
#else
    const char *h = getenv("HOME");
#endif
    return h ? h : "";
}
static std::string lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static bool ends_with(const std::string &s, const char *e) {
    size_t n = strlen(e);
    return s.size() >= n && lower(s.substr(s.size() - n)) == e;
}
static std::string base_name(const std::string &p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

// The build id of an NSO (hex, trailing zero bytes dropped), "" if it isn't one
static std::string nso_build_id(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return "";
    uint8_t h[0x60] = {0};
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n != sizeof h || memcmp(h, "NSO0", 4)) return "";
    int end = 0x60;
    while (end > 0x40 && !h[end - 1]) end--;
    std::string s;
    char b[3];
    for (int i = 0x40; i < end; i++) snprintf(b, sizeof b, "%02x", h[i]), s += b;
    return s;
}
static bool starts_with(const std::string &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }

// ------------------------------------------------------------------ what's ready
static std::string g_root;  // <data>/extracted

static bool game_files_ok(const std::string &root) {
    return starts_with(nso_build_id(root + SEP "exefs" SEP "main"), MAIN_303) && dir_exists(root + SEP "romfs") &&
           file_exists(root + SEP "exefs" SEP "main.npdm");
}
static bool fonts_ok(void) { return file_exists(FONT_STD); }  // (relative: the data root is the working directory)

static bool ocw_config_ready(void);  // (below: a usable OpenCourse World config is chosen or in the data folder)

extern "C" bool launcher_needed(const char *root, bool asked) {
    if (getenv("RECOMP_NO_LAUNCHER") && atoi(getenv("RECOMP_NO_LAUNCHER"))) return false;
    if (getenv("LARECOMP_REBOOT_AT")) asked = false;  // (reboot tests: straight back into the game, as before)
    if (asked || getenv("RECOMP_LAUNCHER_AUTO")) return true;
    // (a source checkout started in its data folder: its own fonts or none, as before)
    return !game_files_ok(root) || !gamelib_present(root) || (!fonts_ok() && !rt_dev_mode()) ||
           (!ocw_config_ready() && !rt_dev_mode());
}

// ------------------------------------------------------------------ launcher.ini (paths only, never key contents)
struct Config {
    std::vector<std::string> game;  // folders with the decrypted game
    std::string keys, firmware;
    std::string ocw;  // the ocw-config.json exported from opencourse.world (read where it is)
    bool update_check = true;
};
static Config g_cfg;
static const char *INI = "launcher.ini";

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    return s.substr(i);
}
static void config_load(void) {
    g_cfg = Config();
    FILE *f = fopen(INI, "r");
    if (!f) return;
    char line[4200];
    while (fgets(line, sizeof line, f)) {
        std::string l = line;
        size_t eq = l.find('=');
        if (l[0] == '#' || eq == std::string::npos) continue;
        std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
        if (k == "game" && !v.empty()) g_cfg.game.push_back(v);
        else if (k == "keys") g_cfg.keys = v;
        else if (k == "firmware") g_cfg.firmware = v;
        else if (k == "ocw") g_cfg.ocw = v;
        else if (k == "update_check") g_cfg.update_check = atoi(v.c_str()) != 0;
    }
    fclose(f);
}
static void config_save(void) {
    FILE *f = fopen(INI, "w");
    if (!f) return;
    fprintf(f, "# " GAME_TITLE " setup screen: where your own files are (paths only)\n");
    for (auto &g : g_cfg.game) fprintf(f, "game=%s\n", g.c_str());
    fprintf(f, "keys=%s\nfirmware=%s\nocw=%s\nupdate_check=%d\n", g_cfg.keys.c_str(), g_cfg.firmware.c_str(),
            g_cfg.ocw.c_str(), g_cfg.update_check ? 1 : 0);
    fclose(f);
}

// Before every boot (the setup keeps no other paths to hand on): the OpenCourse World config chosen here is the one
// the game reads, where it is (games/smm2/mods/ocw.c: RECOMP_OCW_CONFIG), so its token is never copied
extern "C" void launcher_boot_env(void) {
    config_load();
    if (!g_cfg.ocw.empty() && !getenv("RECOMP_OCW_CONFIG")) setenv("RECOMP_OCW_CONFIG", g_cfg.ocw.c_str(), 0);
}

// Where emulators keep the keys and the firmware they were given (suggestions; the player can choose others)
static std::vector<std::string> keys_candidates(void) {
    std::string h = home_dir();
    std::vector<std::string> v;
#ifdef _WIN32
    const char *ad = getenv("APPDATA");
    std::string a = ad ? ad : h + "\\AppData\\Roaming";
    v = {a + "\\Ryujinx\\system\\prod.keys", a + "\\yuzu\\keys\\prod.keys", a + "\\suyu\\keys\\prod.keys",
         a + "\\eden\\keys\\prod.keys", a + "\\citron\\keys\\prod.keys", h + "\\.switch\\prod.keys"};
#elif defined(__APPLE__)
    std::string a = h + "/Library/Application Support";
    v = {a + "/Ryujinx/system/prod.keys", a + "/yuzu/keys/prod.keys", a + "/eden/keys/prod.keys", h + "/.switch/prod.keys"};
#else
    const char *xc = getenv("XDG_CONFIG_HOME"), *xd = getenv("XDG_DATA_HOME");
    std::string c = xc && *xc ? xc : h + "/.config", d = xd && *xd ? xd : h + "/.local/share";
    v = {c + "/Ryujinx/system/prod.keys", d + "/yuzu/keys/prod.keys", d + "/suyu/keys/prod.keys",
         d + "/eden/keys/prod.keys", h + "/.var/app/io.github.ryubing.Ryujinx/config/Ryujinx/system/prod.keys",
         h + "/.switch/prod.keys"};
#endif
    return v;
}
static std::vector<std::string> firmware_candidates(void) {
    std::string h = home_dir();
#ifdef _WIN32
    const char *ad = getenv("APPDATA");
    std::string a = ad ? ad : h + "\\AppData\\Roaming";
    return {a + "\\Ryujinx\\bis\\system\\Contents\\registered", a + "\\yuzu\\nand\\system\\Contents\\registered",
            a + "\\eden\\nand\\system\\Contents\\registered"};
#elif defined(__APPLE__)
    std::string a = h + "/Library/Application Support";
    return {a + "/Ryujinx/bis/system/Contents/registered", a + "/yuzu/nand/system/Contents/registered"};
#else
    const char *xc = getenv("XDG_CONFIG_HOME"), *xd = getenv("XDG_DATA_HOME");
    std::string c = xc && *xc ? xc : h + "/.config", d = xd && *xd ? xd : h + "/.local/share";
    return {c + "/Ryujinx/bis/system/Contents/registered", d + "/yuzu/nand/system/Contents/registered",
            d + "/eden/nand/system/Contents/registered"};
#endif
}

// ------------------------------------------------------------------ checks of what was chosen (cheap, no keys shown)
struct Check {
    bool ok = false, warn = false;
    std::string text;
    struct Line { int kind; std::string text; };  // kind: 0 matched (green), 1 warning, 2 detail (grey)
    std::vector<Line> lines;                      // the verbose part, shown under the folder's line
};

// A source of game files: a folder with the decrypted game (the version of an ExeFS in it, if there is one near the top)
static Check check_game_source(const std::string &p) {
    Check c;
    if (file_exists(p)) {
        c.text = "a file: add the folder with the decrypted game (exefs and romfs)";
        return c;
    }
    if (!dir_exists(p)) { c.text = "not found"; return c; }
    c.ok = true;
    // the folder itself, then the folders directly inside it: the first one with an ExeFS main
    std::vector<std::string> roots = {""};
    if (DIR *d = opendir(p.c_str())) {
        while (struct dirent *e = readdir(d))
            if (e->d_name[0] != '.' && dir_exists(p + SEP + e->d_name)) roots.push_back(e->d_name);
        closedir(d);
    }
    const char *places[] = {"", SEP "exefs", SEP "ExeFS"};
    for (const std::string &sub : roots) {
        std::string root = sub.empty() ? p : p + SEP + sub;
        for (const char *pl : places) {
            std::string main_path = root + pl + SEP "main", id = nso_build_id(main_path);
            if (id.empty()) continue;
            std::string where = (sub.empty() ? std::string() : sub + SEP) + (*pl ? pl + 1 : "") + (*pl ? SEP : "") + "main";
            bool has_romfs = dir_exists(root + SEP "romfs") || dir_exists(root + SEP "RomFS");
            c.lines.push_back({2, "folder:  " + root});
            c.lines.push_back({2, "game code:  " + where + "  (build id " + id + ")"});
            if (starts_with(id, MAIN_303)) {
                c.text = "Super Mario Maker 2 3.0.3 - matched";
                c.lines.push_back({0, "OK  build id matches the 3.0.3 release (" + std::string(MAIN_303) + ")"});
            } else if (starts_with(id, MAIN_100)) {
                c.text = "version 1.0.0 - also add the 3.0.3 update", c.warn = true;
                c.lines.push_back({1, "!  this is the 1.0.0 base game; 3.0.3 expects build id " + std::string(MAIN_303)});
            } else {
                c.text = "another version - 3.0.3 is needed", c.warn = true;
                c.lines.push_back({1, "!  build id doesn't match the 3.0.3 release (" + std::string(MAIN_303) + ")"});
            }
            c.lines.push_back(has_romfs ? Check::Line{0, "OK  romfs folder found next to it"}
                                        : Check::Line{1, "!  no romfs folder next to it"});
            if (!has_romfs) c.warn = true;
            return c;
        }
    }
    c.text = "searched for exefs and romfs when set up";
    c.lines.push_back({2, "no exefs/main found here or one folder down; setup will search deeper"});
    return c;
}

static Check check_keys(const std::string &p) {
    Check c;
    if (p.empty()) { c.text = "needed for the fonts"; return c; }
    FILE *f = fopen(p.c_str(), "r");
    if (!f) { c.text = "not found"; return c; }
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
    c.ok = header && master;
    c.text = c.ok ? "keys file found (read in place, never copied)" : "doesn't look like a prod.keys (no header_key / master keys)";
    return c;
}

static Check check_firmware(const std::string &p) {
    Check c;
    if (p.empty()) { c.text = "needed: the console's fonts come from it"; return c; }
    DIR *d = opendir(p.c_str());
    if (!d) { c.text = "not found"; return c; }
    int n = 0;
    while (struct dirent *e = readdir(d)) n += ends_with(e->d_name, ".nca");
    closedir(d);
    c.ok = n > 0;
    char b[200];
    snprintf(b, sizeof b, n ? "%d system files" : "no NCA files in this folder (choose the folder with the firmware's .nca files)", n);
    c.text = b;
    return c;
}

static std::string read_text(const std::string &p);

// The value of a "key": "value" string in the ocw-config.json text ("" if there is none)
static std::string ocw_field(const std::string &text, const char *key) {
    size_t k = text.find(std::string("\"") + key + "\"");
    if (k == std::string::npos) return "";
    size_t c = text.find(':', k), q1 = c == std::string::npos ? c : text.find('"', c);
    size_t q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 1);
    return q2 == std::string::npos ? "" : text.substr(q1 + 1, q2 - q1 - 1);
}

// The OpenCourse World config exported from opencourse.world: { "domain": "...", "token": "<22 characters>" }. Only
// the domain is ever shown; the token is counted and dropped (games/smm2/mods/ocw.c holds the rules: a domain of at
// most 29 characters, a token of exactly 22)
static Check check_ocw(const std::string &p) {
    Check c;
    if (p.empty()) { c.text = "needed: the config exported from opencourse.world"; return c; }
    if (!file_exists(p)) { c.text = dir_exists(p) ? "a folder: choose the ocw-config.json file" : "not found"; return c; }
    std::string text = read_text(p);
    if (text.empty() || text.size() > 2000) { c.text = "doesn't look like an OpenCourse World config"; return c; }
    std::string domain = ocw_field(text, "domain"), token = ocw_field(text, "token");
    std::fill(token.begin(), token.end(), 'x');  // (only its length is looked at below)
    if (domain.empty() || token.empty()) { c.text = "no domain or no token in it: export the config again from opencourse.world"; return c; }
    if (domain.size() > 29) { c.text = "the domain is longer than the game's own slot allows (29 characters)"; return c; }
    if (token.size() != 22) { c.text = "the token isn't 22 characters: export the config again from opencourse.world"; return c; }
    c.ok = true;
    c.text = "config found: " + domain + ", token set";
    return c;
}

// Is there a usable config for the game to read: the one RECOMP_OCW_CONFIG names, the one chosen on this screen, or
// ocw-config.json in the data folder (where the game looks without either)
static bool ocw_config_ready(void) {
    if (getenv("RECOMP_OCW_CONFIG") && *getenv("RECOMP_OCW_CONFIG")) return check_ocw(getenv("RECOMP_OCW_CONFIG")).ok;
    config_load();
    if (!g_cfg.ocw.empty() && check_ocw(g_cfg.ocw).ok) return true;
    return check_ocw("ocw-config.json").ok;
}

// The checks of what's chosen, at most every 2 s per path (the screen draws 60 times a second; files can appear)
static Check cached_check(char kind, const std::string &p, Check (*fn)(const std::string &)) {
    struct Entry { double t; Check c; };
    static std::vector<std::pair<std::string, Entry>> cache;
    std::string key = std::string(1, kind) + p;
    double now = SDL_GetTicks() / 1000.0;
    for (auto &e : cache)
        if (e.first == key) {
            if (now - e.second.t > 2) e.second = {now, fn(p)};
            return e.second.c;
        }
    if (cache.size() > 64) cache.clear();
    cache.push_back({key, {now, fn(p)}});
    return cache.back().second.c;
}

// ------------------------------------------------------------------ the tools
struct Tools {
    std::string python, setup, sysdata, hactool;
};
static bool find_tools(Tools &t, std::string &why) {
    char b[1100];
    if (!gamebuild_python(b, sizeof b)) { why = "the Python that comes with this program is missing"; return false; }
    t.python = b;
    if (!rt_resource("tools" SEP "games" SEP "smm2" SEP "setup.py", "games/smm2/setup.py", b, sizeof b)) {
        why = "tools/games/smm2/setup.py is missing";
        return false;
    }
    t.setup = b;
    if (!rt_resource("tools" SEP "games" SEP "smm2" SEP "sysdata.py", "games/smm2/sysdata.py", b, sizeof b)) {
        why = "tools/games/smm2/sysdata.py is missing";
        return false;
    }
    t.sysdata = b;
    if (rt_resource("hactool" SEP "hactool" PLAT_EXE_SUFFIX, NULL, b, sizeof b)) t.hactool = b;
    else t.hactool.clear();  // (a source checkout: the scripts look on PATH and in ~/smm-work/tools/bin)
    return true;
}

// ------------------------------------------------------------------ the setup job (background thread)
enum StepState { PENDING, RUNNING, DONE, SKIPPED, FAILED };
struct Step {
    const char *name;
    StepState state;
    std::string detail;
};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Step g_steps[3] = {{"Check and lay out your game files", PENDING, ""},
                          {"Copy the fonts and system data from your firmware", PENDING, ""},
                          {"Recompile the game code", PENDING, ""}};
static std::atomic<float> g_progress{0};
static std::atomic<int> g_job{0};  // 0 idle, 1 running, 2 done (play), 3 failed
static std::atomic<bool> g_built_wait{false};  // the game code was just built on a small Linux machine: Play, not a start
static std::string g_err_what, g_err_hint, g_err_log;
static std::vector<std::string> g_found;
static std::atomic<bool> g_cancel{false};
static PlatProc g_child;

// The log window's lines, like the Build Helper's console: a step's name in blue, what it does in grey, a finished
// step in green, an error in red (kind 0 / 1 / 2 / 3), each with the time since the setup started.
struct LogLine {
    unsigned secs;
    int kind;
    std::string text;
};
static std::vector<LogLine> g_log;
static std::atomic<unsigned> g_job_t0{0};
static void add_log(int kind, const std::string &text) {
    pthread_mutex_lock(&g_lock);
    g_log.push_back({(unsigned)((SDL_GetTicks() - g_job_t0) / 1000), kind, text});
    pthread_mutex_unlock(&g_lock);
}

static void set_step(int i, StepState s, const std::string &detail = "") {
    pthread_mutex_lock(&g_lock);
    StepState old = g_steps[i].state;
    std::string old_detail = g_steps[i].detail;
    g_steps[i].state = s;
    if (!detail.empty() || s != RUNNING) g_steps[i].detail = detail;
    pthread_mutex_unlock(&g_lock);
    if (s == RUNNING && old != RUNNING) add_log(1, g_steps[i].name);
    if (!detail.empty() && detail != old_detail) add_log(s == DONE || s == SKIPPED ? 2 : 0, detail);
}
static void fail(int step, const std::string &what, const std::string &hint, const std::string &log = "") {
    pthread_mutex_lock(&g_lock);
    g_steps[step].state = FAILED;
    g_err_what = what, g_err_hint = hint, g_err_log = log;
    pthread_mutex_unlock(&g_lock);
    add_log(3, what);
    if (!hint.empty()) add_log(0, hint);
    if (!log.empty()) add_log(0, "Details: " + log);
    LOG("[launcher] %s failed: %s\n", g_steps[step].name, what.c_str());
    g_job = 3;
}

static std::string read_text(const std::string &p) {
    std::string s;
    if (FILE *f = fopen(p.c_str(), "rb")) {
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0 && s.size() < (4u << 20)) s.append(buf, n);
        fclose(f);
    }
    return s;
}

// Runs a setup script, following its "@" lines (step text, progress lo..hi, what it found, its error). true: it worked.
static bool run_script(int step, const std::vector<std::string> &args, const std::string &log, float lo, float hi) {
    std::vector<const char *> av;
    for (auto &a : args) av.push_back(a.c_str());
    av.push_back(NULL);
    PlatProc p = plat_spawn(av.data(), log.c_str(), false);
    if (!p) {
        fail(step, "The setup tool didn't start", "Reinstall " GAME_TITLE ": its tools folder may be damaged.", log);
        return false;
    }
    pthread_mutex_lock(&g_lock);
    g_child = p;
    pthread_mutex_unlock(&g_lock);
    int rc;
    size_t seen = 0;
    std::string err;
    for (;;) {
        rc = plat_proc_wait(p, false);
        std::string out = read_text(log);
        for (size_t nl; (nl = out.find('\n', seen)) != std::string::npos; seen = nl + 1) {
            std::string l = out.substr(seen, nl - seen);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (starts_with(l, "@step ")) set_step(step, RUNNING, l.substr(6));
            else if (starts_with(l, "@progress ")) g_progress = lo + (hi - lo) * (float)atof(l.c_str() + 10);
            else if (starts_with(l, "@found ")) {
                pthread_mutex_lock(&g_lock);
                g_found.push_back(l.substr(7));
                pthread_mutex_unlock(&g_lock);
                add_log(0, l.substr(7));
            } else if (starts_with(l, "@error ")) err = l.substr(7);
        }
        if (rc >= 0) break;
        SDL_Delay(100);
    }
    pthread_mutex_lock(&g_lock);
    g_child = 0;
    pthread_mutex_unlock(&g_lock);
    if (rc == 0 && err.empty()) return true;
    if (g_cancel) return false;
    std::string what = "It stopped with an error", hint = "The log has the details.";
    if (!err.empty()) {  // "<code>\t<what>\t<what to do>"
        size_t a = err.find('\t'), b = a == std::string::npos ? a : err.find('\t', a + 1);
        if (a != std::string::npos) what = err.substr(a + 1, b == std::string::npos ? b : b - a - 1);
        if (b != std::string::npos) hint = err.substr(b + 1);
    } else {  // (a Python error: its last line; the scripts never print key values)
        std::string out = read_text(log);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        size_t nl = out.find_last_of('\n');
        what = out.substr(nl == std::string::npos ? 0 : nl + 1);
        if (what.size() > 300) what = what.substr(0, 300);
    }
    fail(step, what, hint, log);
    return false;
}

static void build_progress(double f, const char *what) {
    g_progress = 0.25f + 0.75f * (float)f;
    static const char *last;
    if (what != last) last = what, set_step(2, RUNNING, what);
}

static std::string setup_stamp(void) {  // what the game files were set up from (setup.json's sources)
    return read_text(g_root + SEP "setup.json");
}

static void *job_thread(void *) {
    Tools t;
    std::string why;
    std::string root = g_root;
    g_progress = 0;
    pthread_mutex_lock(&g_lock);
    for (auto &s : g_steps) s.state = PENDING, s.detail.clear();
    g_found.clear();
    g_log.clear();
    g_job_t0 = SDL_GetTicks();
    pthread_mutex_unlock(&g_lock);
    bool need_files = !game_files_ok(root), need_fonts = !fonts_ok();
    // chosen files other than those the game was set up from: set it up again
    if (!need_files) {
        std::string stamp = setup_stamp();
        for (auto &g : g_cfg.game) {
            std::string esc;  // (as setup.py wrote it: JSON, the path as given)
            for (char c : g) esc += c == '\\' || c == '"' ? std::string("\\") + c : std::string(1, c);
            if (!stamp.empty() && stamp.find("\"" + esc + "\"") == std::string::npos) need_files = true;
        }
    }
    if ((need_files || need_fonts) && !find_tools(t, why)) {
        fail(need_files ? 0 : 1, "The setup tools aren't there (" + why + ")",
             "Download " GAME_TITLE " again and unpack all of it.");
        return NULL;
    }
    std::string logs = std::string(rt_data_root()) + SEP "logs";
    mkdir(logs.c_str(), 0755);

    // 1. the game files
    if (need_files) {
        set_step(0, RUNNING);
        LOG("[launcher] setting up the game files from %zu source(s)\n", g_cfg.game.size());
        std::vector<std::string> a = {t.python, t.setup};
        for (auto &g : g_cfg.game) a.push_back("--source"), a.push_back(g);
        if (!g_cfg.keys.empty()) a.push_back("--keys"), a.push_back(g_cfg.keys);
        if (!t.hactool.empty()) a.push_back("--hactool"), a.push_back(t.hactool);
        a.push_back("--out"), a.push_back(root);
        if (!run_script(0, a, logs + SEP "setup.log", 0, 0.2f)) return NULL;
        if (!game_files_ok(root)) {
            fail(0, "The game files weren't set up", "Check the files you chose and try again.", logs + SEP "setup.log");
            return NULL;
        }
        set_step(0, DONE, "Super Mario Maker 2 3.0.3");
    } else {
        set_step(0, SKIPPED, "already set up: Super Mario Maker 2 3.0.3");
    }
    g_progress = 0.2f;

    // 2. fonts and system data
    if (need_fonts) {
        set_step(1, RUNNING);
        LOG("[launcher] copying the fonts and system data from the firmware\n");
        std::vector<std::string> a = {t.python, t.sysdata, "--firmware", g_cfg.firmware, "--keys", g_cfg.keys,
                                      "--data", rt_data_root()};
        if (!t.hactool.empty()) a.push_back("--hactool"), a.push_back(t.hactool);
        if (!run_script(1, a, logs + SEP "sysdata.log", 0.2f, 0.25f)) return NULL;
        if (!fonts_ok()) {
            fail(1, "The fonts weren't copied", "Check the firmware folder and prod.keys.", logs + SEP "sysdata.log");
            return NULL;
        }
        set_step(1, DONE, "shared fonts and system data copied");
    } else {
        set_step(1, SKIPPED, "already there");
    }
    g_progress = 0.25f;

    // 3. the game code (setup.py checked the ExeFS against the known 3.0.3 hashes: a library built from it before is
    // still the right one, wherever the files came from)
    if (!gamelib_present(root.c_str())) {
        set_step(2, RUNNING, "Starting...");
        if (!gamebuild_available()) {
            fail(2, "The recompiler isn't there", "Download " GAME_TITLE " again and unpack all of it (zig, tools, python).");
            return NULL;
        }
        int cores = SDL_GetNumLogicalCPUCores(), ram_gb = SDL_GetSystemRAM() / 1024;
        // (a compiler takes up to ~1 GB, and the generators, which run meanwhile, ~4 GB together)
        int jobs = cores < ram_gb - 6 ? cores : ram_gb - 6;
        if (getenv("RECOMP_BUILD_JOBS") && atoi(getenv("RECOMP_BUILD_JOBS")) > 0) jobs = atoi(getenv("RECOMP_BUILD_JOBS"));
        GameBuildOpts o = {build_progress, jobs > 1 ? jobs : 1};
        char w[512] = "";
        LOG("[launcher] recompiling the game code (%d jobs)\n", o.jobs);
        if (!gamebuild_run(root.c_str(), &o, w, sizeof w)) {
            if (g_cancel) return NULL;
            fail(2, std::string("The game code didn't build: ") + w,
                 "Make sure there are about 4 GB of disk space and 4 GB of memory free, then try again. If it keeps happening, report it on GitHub "
                 "with the log.", plat_log_file());
            return NULL;
        }
        set_step(2, DONE, "built");
#if defined(__linux__) && !defined(__ANDROID__)
        // A Steam Deck (16 GB) froze starting the game straight after the build in desktop mode (#13): the build and
        // the first start together are too much for it there. Back to the screen instead: Play (or Gaming Mode)
        if (ram_gb <= 16) {
            LOG("[launcher] built on a %d GB machine: the game starts with Play, not straight away\n", ram_gb);
            g_built_wait = true;
            g_job = 0;
            return NULL;
        }
#endif
    } else {
        set_step(2, SKIPPED, "already built");
    }
    g_progress = 1;
    g_job = 2;
    return NULL;
}

// ------------------------------------------------------------------ update check (notifies only)
static std::string g_update_tag;
static pthread_mutex_t g_update_lock = PTHREAD_MUTEX_INITIALIZER;

static long long tag_rank(const std::string &t) {  // v<major>.<minor>.<patch> (anything after: the same release)
    int a, b, c;
    if (sscanf(t.c_str(), "v%d.%d.%d", &a, &b, &c) == 3 && a >= 0 && b >= 0 && c >= 0 && b < 4096 && c < 4096)
        return ((long long)a << 24) + ((long long)b << 12) + c;
    return -1;  // (dev builds: any release is newer)
}
extern "C" void menu_notice(const char *text, double seconds);  // (menu.mm: a notice over the game)
static void *update_thread(void *) {
    std::string body;
    if (const char *t = getenv("RECOMP_UPDATE_TAGS")) {
        body = std::string("\"tag_name\": \"") + t + "\"";
    } else {
#ifdef _WIN32
        const char *sr = getenv("SystemRoot");
        std::string curl = std::string(sr ? sr : "C:\\Windows") + "\\System32\\curl.exe";
#else
        std::string curl = file_exists("/usr/bin/curl") ? "/usr/bin/curl" : "/bin/curl";
#endif
        std::string dir = std::string(rt_data_root()) + SEP "cache";
        mkdir(dir.c_str(), 0755);
        std::string out = dir + SEP "latest-release.json";
        const char *av[] = {curl.c_str(), "-fsSL", "--max-time", "15", "-H", "Accept: application/vnd.github+json",
                            "-A", "smm2-dx/" RECOMP_VERSION, RELEASES_API, NULL};
        PlatProc p = file_exists(curl) ? plat_spawn(av, out.c_str(), false) : 0;
        if (!p || plat_proc_wait(p, true) != 0) { remove(out.c_str()); return NULL; }
        body = read_text(out);
        remove(out.c_str());
    }
    size_t at = body.find("\"tag_name\"");
    if (at == std::string::npos) return NULL;
    size_t q1 = body.find('"', body.find(':', at) + 1), q2 = q1 == std::string::npos ? q1 : body.find('"', q1 + 1);
    if (q2 == std::string::npos) return NULL;
    std::string tag = body.substr(q1 + 1, q2 - q1 - 1);
    if (tag_rank(tag) > tag_rank(RECOMP_VERSION)) {
        LOG("[launcher] a newer release is out: %s (this is %s): %s\n", tag.c_str(), RECOMP_VERSION, RELEASES_PAGE);
        pthread_mutex_lock(&g_update_lock);
        g_update_tag = tag;
        pthread_mutex_unlock(&g_update_lock);
        // said over the game too, once it's past its start logos: the Mods tab's row alone went unseen (issue #25)
        while (SDL_GetTicks() < 25000) SDL_Delay(500);
        char note[200];
        snprintf(note, sizeof note, "Version %s is out (this is %s). Menu (F1) > Online: New version.",
                 tag.c_str(), RECOMP_VERSION);
        menu_notice(note, 12);
    }
    return NULL;
}
extern "C" void launcher_update_check_start(void) {
    static bool started;
    if (started || (getenv("RECOMP_NO_UPDATE_CHECK") && !getenv("RECOMP_UPDATE_TAGS"))) return;
    config_load();
    if (!g_cfg.update_check) return;
    started = true;
    pthread_t th;
    if (pthread_create(&th, NULL, update_thread, NULL) == 0) pthread_detach(th);
}
extern "C" const char *launcher_update_tag(void) {
    static std::string copy;
    pthread_mutex_lock(&g_update_lock);
    copy = g_update_tag;
    pthread_mutex_unlock(&g_update_lock);
    return copy.empty() ? NULL : copy.c_str();
}

// The overlay menu's Mods tab, its app section (runtime/ui/menu.cpp tab_mods): updates and the game files. true: drawn.
extern "C" void rt_request_reboot(int kind);
extern "C" void menu_toggle(void);
extern "C" bool launcher_has_settings(void) { return true; }
extern "C" bool launcher_settings_rows(void) {
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
    if (const char *tag = launcher_update_tag()) {
        char d[300];
        snprintf(d, sizeof d, "Version %s is out (this is %s). Opens its download page in your browser.", tag, RECOMP_VERSION);
        if (opt_action("New version available", tag, d)) SDL_OpenURL(RELEASES_PAGE);
    }
    if (opt_action("Game files", "Set up...", "Back to the setup screen: choose other game files, keys or "
                   "firmware. Progress since your last save is lost."))
        rt_request_reboot(2 /* RT_REBOOT_TITLE */), menu_toggle();
    return true;
}

// ------------------------------------------------------------------ file dialogs (SDL calls back on any thread)
enum PickTarget { PICK_GAME, PICK_KEYS, PICK_FIRMWARE, PICK_OCW };
static std::vector<std::pair<int, std::string>> g_picked;
static void SDLCALL picked(void *target, const char *const *files, int) {
    if (!files) return;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; files[i]; i++) g_picked.push_back({(int)(intptr_t)target, files[i]});
    pthread_mutex_unlock(&g_lock);
}
static std::string dialog_start(const std::string &p) {
    if (dir_exists(p)) return p;
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? home_dir() : p.substr(0, s);
}

// ------------------------------------------------------------------ screenshots (tests: RECOMP_LAUNCHER_SHOTS)
#ifdef __APPLE__
static void save_png(id<MTLTexture> tex, const char *path) {
    size_t w = tex.width, h = tex.height;
    std::vector<uint8_t> px(w * h * 4);
    [tex getBytes:px.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    if (dest) {
        CGImageDestinationAddImage(dest, img, NULL);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
    }
    CFRelease(url), CGImageRelease(img), CGContextRelease(ctx), CGColorSpaceRelease(cs);
}
#endif

// ------------------------------------------------------------------ drawing
static ImFont *g_font, *g_font_big, *g_font_title;
// The Build Helper console's colours, exactly as its terminal shows them (the SGR codes it prints, in Windows
// Terminal's default scheme): 36 cyan = the step lines and bar, 33 yellow = the name, 90 grey = details, 32 green = done,
// 31 red = errors, 35 magenta = the rule under the name, 97 white = the title, on the console's own black.
#define RGB8(r, g, b) ImVec4((r) / 255.0f, (g) / 255.0f, (b) / 255.0f, 1)
static const ImVec4 GOLD = RGB8(193, 156, 0), BLUE = RGB8(58, 150, 221), DIM = RGB8(118, 118, 118),
                    GOOD = RGB8(19, 161, 14), WARN = RGB8(193, 156, 0), BAD = RGB8(197, 15, 31),
                    MAGENTA = RGB8(136, 23, 152), WHITE = RGB8(242, 242, 242), TEXT = RGB8(204, 204, 204),
                    BLACK = RGB8(12, 12, 12);

static ImVec4 shade(const ImVec4 &c, float toward_white) {  // (a hover is the colour a quarter lighter, a press a quarter darker)
    return toward_white >= 0 ? ImVec4(c.x + (1 - c.x) * toward_white, c.y + (1 - c.y) * toward_white, c.z + (1 - c.z) * toward_white, 1)
                             : ImVec4(c.x * (1 + toward_white), c.y * (1 + toward_white), c.z * (1 + toward_white), 1);
}

// Square, flat, the console's black, cyan header bars, magenta-edged boxes, small "?" tooltips
static void style_init(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = s.FrameRounding = s.GrabRounding = s.ChildRounding = s.PopupRounding = s.ScrollbarRounding = 0;
    s.WindowPadding = ImVec2(10, 10), s.FramePadding = ImVec2(8, 5), s.ItemSpacing = ImVec2(8, 6);
    s.WindowBorderSize = 0, s.ChildBorderSize = 1, s.FrameBorderSize = 0, s.ScrollbarSize = 14;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = BLACK;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = BLACK;
    c[ImGuiCol_Border] = BLUE;
    c[ImGuiCol_Text] = TEXT;
    c[ImGuiCol_TextDisabled] = DIM;
    c[ImGuiCol_Header] = BLUE;
    c[ImGuiCol_HeaderHovered] = shade(BLUE, 0.25f);
    c[ImGuiCol_HeaderActive] = shade(BLUE, -0.25f);
    c[ImGuiCol_FrameBg] = ImVec4(DIM.x, DIM.y, DIM.z, 0.25f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(DIM.x, DIM.y, DIM.z, 0.40f);
    c[ImGuiCol_FrameBgActive] = ImVec4(DIM.x, DIM.y, DIM.z, 0.55f);
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

// A small "?" at the right edge of the line: what it is for, as a tooltip
static void help_marker(const char *text) {
    float w = ImGui::CalcTextSize("?").x + 2 * ImGui::GetStyle().FramePadding.x;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - w);
    ImGui::SmallButton("?");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// The Build Helper's bar: [#####---------------]  25%  what it is doing
static std::string text_bar(float f, const std::string &what) {
    int pct = (int)(f * 100 + 0.5f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int fill = pct / 5;
    char b[640];
    snprintf(b, sizeof b, "[%.*s%.*s] %3d%%  %s", fill, "####################", 20 - fill, "--------------------", pct,
             what.c_str());
    return b;
}

static void wrapped(const ImVec4 &col, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static void status_line(const Check &c, float indent) {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);  // (under the path field)
    ImGui::PushStyleColor(ImGuiCol_Text, c.ok ? (c.warn ? WARN : GOOD) : BAD);
    ImGui::TextUnformatted(c.ok ? (c.warn ? "!" : "OK") : "X");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    wrapped(c.ok ? DIM : BAD, "%s", c.text.c_str());
}

// A path row: the field (typed or pasted) and Browse. true when it changed.
static bool path_row(const char *id, std::string &path, const char *hint, PickTarget target, bool folder, float s) {
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(id);
    ImGui::SameLine(130 * s);
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", path.c_str());
    float bw = 150 * s;
    ImGui::SetNextItemWidth(-bw - ImGui::GetStyle().ItemSpacing.x);
    bool changed = false;
    if (ui_input_text("##path", buf, sizeof buf, 0, hint, id)) path = trim(buf), changed = true;
    ImGui::SameLine();
    if (ImGui::Button("Browse...", ImVec2(bw, 0))) {
        std::string start = dialog_start(path.empty() ? home_dir() : path);
        if (folder) SDL_ShowOpenFolderDialog(picked, (void *)(intptr_t)target, ui_window(), start.c_str(), false);
        else if (target == PICK_OCW) {
            static const SDL_DialogFileFilter ocw_filter[] = {{"OpenCourse World config", "json"}, {"All files", "*"}};
            SDL_ShowOpenFileDialog(picked, (void *)(intptr_t)target, ui_window(), ocw_filter, 2, start.c_str(), false);
        } else {
            static const SDL_DialogFileFilter keys_filter[] = {{"Keys", "keys;txt"}, {"All files", "*"}};
            SDL_ShowOpenFileDialog(picked, (void *)(intptr_t)target, ui_window(), keys_filter, 2, start.c_str(), false);
        }
    }
    ImGui::PopID();
    return changed;
}

// The coffee link: the whole line is the link
static void coffee_line(void) {
    static const char *url = "https://buymeacoffee.com/sometimes_steve";
    ImGui::TextColored(GOLD, "If you enjoy my work, consider buying me a coffee! %s", url);
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y),
                                            ImGui::GetItemRectMax(), ImGui::GetColorU32(GOLD));
    }
    if (ImGui::IsItemClicked()) SDL_OpenURL(url);
}

// The braille art, dot by dot (the font has no braille): pinned at the left like the Build Helper's console
static void draw_art(ImVec2 pos, float maxw, float maxh) {
    static const int DX[8] = {0, 0, 0, 1, 1, 1, 0, 1}, DY[8] = {0, 1, 2, 0, 1, 2, 3, 3};
    float step = fminf(maxw / (ART_COLS * 2), maxh / (ART_ROWS * 4));
    if (step < 1.5f) return;
    float r = step * 0.42f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 col = ImGui::GetColorU32(MAGENTA);
    for (int y = 0; y < ART_ROWS; y++)
        for (int x = 0; x < ART_COLS; x++)
            for (int b = 0; b < 8; b++)
                if (ART[y][x] & (1 << b))
                    dl->AddCircleFilled(ImVec2(pos.x + ((x * 2 + DX[b]) + 0.5f) * step, pos.y + ((y * 4 + DY[b]) + 0.5f) * step), r, col, 6);
}

// A section: a blue header bar over a bordered box. true: open (EndChild when done)
static bool section_begin(const char *id, const char *title) {
    ImGui::PushStyleColor(ImGuiCol_Text, WHITE);
    bool open = ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopStyleColor();
    if (open)
        ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NavFlattened);
    return open;
}

static bool big_button(const char *label, bool enabled, float w, float h) {
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, GOLD);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, shade(GOLD, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, shade(GOLD, -0.25f));
    ImGui::PushStyleColor(ImGuiCol_Text, BLACK);
    ImGui::PushFont(g_font_big);
    bool r = ImGui::Button(label, ImVec2(w, h));
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    return r;
}

// ------------------------------------------------------------------ the screen
extern "C" void launcher_run(const char *root) {
    g_root = root;
    config_load();
    // suggestions for what isn't chosen yet: the keys and firmware an emulator already has
    if (g_cfg.keys.empty())
        for (auto &k : keys_candidates()) if (file_exists(k)) { g_cfg.keys = k; break; }
    if (g_cfg.firmware.empty())
        for (auto &f : firmware_candidates()) if (dir_exists(f)) { g_cfg.firmware = f; break; }
    launcher_update_check_start();
    // the auto-fill of a test run
    const char *autof = getenv("RECOMP_LAUNCHER_AUTO");
    if (autof) {
        g_cfg.game.clear();
        std::string a = autof;
        for (size_t i = 0; i <= a.size();) {
            size_t e = a.find('|', i);
            if (e == std::string::npos) e = a.size();
            std::string kv = a.substr(i, e - i), k = kv.substr(0, kv.find('=')), v = kv.find('=') != std::string::npos ? kv.substr(kv.find('=') + 1) : "";
            if (k == "game") g_cfg.game.push_back(v);
            else if (k == "keys") g_cfg.keys = v;
            else if (k == "firmware") g_cfg.firmware = v;
            else if (k == "ocw") g_cfg.ocw = v;
            i = e + 1;
        }
    }
    const char *shots = getenv("RECOMP_LAUNCHER_SHOTS");

    ui_window_init();
    SDL_Window *win = ui_window();
    SDL_SetWindowTitle(win, launcher_needed(root, false) ? "smm2-dx - First-Time Setup" : GAME_TITLE);
#ifdef __APPLE__
    CAMetalLayer *layer = (__bridge CAMetalLayer *)ui_metal_layer();
    id<MTLDevice> dev = (__bridge id<MTLDevice>)ui_metal_device();
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    if (shots) layer.framebufferOnly = NO;
#endif
    IMGUI_CHECKVERSION();
    ImGuiContext *prev = ImGui::GetCurrentContext();  // (the overlay menu's, after Quit to Start Screen)
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;
    {
        int ww, wh;
        SDL_GetWindowSizeInPixels(win, &ww, &wh);
        float atlas = wh > 1080 ? 2.0f : 1.0f;  // (sharp text on big / Retina windows)
        ImFontConfig cfg;
        cfg.OversampleH = 1;
        cfg.PixelSnapH = true;
        cfg.SizePixels = 17 * atlas;
        g_font = io.Fonts->AddFontDefault(&cfg);
        cfg.SizePixels = 24 * atlas;
        g_font_big = g_font_title = io.Fonts->AddFontDefault(&cfg);
        io.FontGlobalScale = 1 / atlas;
    }
    style_init();
#ifdef __APPLE__
    ImGui_ImplSDL3_InitForMetal(win);
    ImGui_ImplMetal_Init(dev);
#else
    ImGui_ImplSDL3_InitForVulkan(win);
    vk_imgui_backend_init();
#endif
    ImGuiStyle base = ImGui::GetStyle();
    float style_scale = -1;

    bool ready_at_start = !launcher_needed(root, false);  // (back here from the game: everything is there)
    bool play = false, quit = false;
    int frame = 0, shot_n = 0, last_shot_key = -1;
    double last_shot_t = 0;
    g_job = 0;
    std::string add_path;
#ifdef __APPLE__
    bool quarantined = false;  // a downloaded app macOS hasn't cleared: its helpers may be refused (README: xattr)
    {
        char exe[1100];
        if (plat_exe_path(exe, sizeof exe)) quarantined = getxattr(exe, "com.apple.quarantine", NULL, 0, 0, 0) > 0;
    }
#endif

    while (!play) {
        // picks from the file dialogs
        pthread_mutex_lock(&g_lock);
        auto picks = g_picked;
        g_picked.clear();
        pthread_mutex_unlock(&g_lock);
        for (auto &p : picks) {
            if (p.first == PICK_GAME) g_cfg.game.push_back(p.second);
            if (p.first == PICK_KEYS) g_cfg.keys = p.second;
            if (p.first == PICK_FIRMWARE) g_cfg.firmware = p.second;
            if (p.first == PICK_OCW) g_cfg.ocw = p.second;
            config_save();
        }
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

#ifdef __APPLE__
        int pw, ph;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        layer.drawableSize = CGSizeMake(pw, ph);
        @autoreleasepool {
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (!drawable) { SDL_Delay(16); continue; }
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = drawable.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(12 / 255.0, 12 / 255.0, 12 / 255.0, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            ImGui_ImplMetal_NewFrame(rp);
#else
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
#endif
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ImVec2 ds = io.DisplaySize;
            float s = ds.y / 720.0f;
            if (s < 0.6f) s = 0.6f;
            if (fabsf(style_scale - s) > 0.01f) {
                ImGui::GetStyle() = base;
                ImGui::GetStyle().ScaleAllSizes(s);
                style_scale = s;
            }
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ds);
            ImGui::Begin("##launcher", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
            ImGui::SetWindowFontScale(s);
            int job = g_job;

            float footer = 146 * s;
            float body_h = ImGui::GetContentRegionAvail().y - footer;
            float art_w = ds.x < 760 * s ? 0 : fminf(ds.x * 0.27f, 330 * s);  // (a narrow window: no art)
            if (art_w > 0) {
                // the banner, like the Build Helper's: name and title, version, author; the art sits under it
                ImVec2 c0 = ImGui::GetCursorPos();
                ImGui::TextColored(GOLD, "smm2-dx");
                ImGui::SameLine(0, 16 * s);
                ImGui::TextColored(WHITE, "First-Time Setup");
                ImGui::TextDisabled("%s", RECOMP_VERSION);
                ImGui::TextColored(GOLD, "by sometimes_steve");
                float banner_h = ImGui::GetCursorPosY() - c0.y + 12 * s;
                ImGui::SetCursorPos(c0);
                ImVec2 sp = ImGui::GetCursorScreenPos();
                draw_art(ImVec2(sp.x, sp.y + banner_h), art_w, body_h - banner_h);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + art_w + ImGui::GetStyle().ItemSpacing.x);
            }
            ImGui::BeginChild("##body", ImVec2(0, body_h), ImGuiChildFlags_None,
                              job == 0 ? ImGuiWindowFlags_NavFlattened
                                       : ImGuiWindowFlags_NavFlattened | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            std::vector<Check> gchecks;
            for (auto &g : g_cfg.game) gchecks.push_back(cached_check('g', g, check_game_source));
            Check keys = cached_check('k', g_cfg.keys, check_keys), fw = cached_check('f', g_cfg.firmware, check_firmware);
            bool games_ok = !gchecks.empty();
            for (auto &c : gchecks) games_ok &= c.ok;
            Check ocwc = cached_check('o', g_cfg.ocw, check_ocw);
            bool fonts_needed = !fonts_ok(), can_start = games_ok && ocwc.ok && (!fonts_needed || (keys.ok && fw.ok));
            bool files_ready = ready_at_start && g_cfg.game.empty() == false && ocwc.ok;
            if (job == 0) {
                // ---- the game: folders with the decrypted files
                if (section_begin("##c1", "Game files")) {
                    ImGui::TextDisabled("Super Mario Maker 2 3.0.3");
                    help_marker("Folders with the decrypted game: the ExeFS and RomFS (e.g. an emulator's \"Extract RomFS / ExeFS\" "
                                "of the updated game, or the base game's and the 3.0.3 update's folders). NSP and XCI files aren't "
                                "used. The game version must be 3.0.3.");
                    int remove_at = -1;
                    for (size_t i = 0; i < g_cfg.game.size(); i++) {
                        // one line each: the check's mark, the name (the whole path on hover), what it is, Remove
                        ImGui::PushID((int)i);
                        const Check &c = gchecks[i];
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextColored(c.ok ? (c.warn ? WARN : GOOD) : BAD, "%s", c.ok ? (c.warn ? "!" : "OK") : "X");
                        ImGui::SameLine();
                        ImGui::TextUnformatted(base_name(g_cfg.game[i]).c_str());
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g_cfg.game[i].c_str());
                        ImGui::SameLine();
                        ImGui::TextColored(c.ok ? (c.warn ? WARN : GOOD) : BAD, "- %s", c.text.c_str());
                        float bw = 100 * s;
                        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - bw);
                        if (ImGui::Button("Remove", ImVec2(bw, 0))) remove_at = (int)i;
                        for (const Check::Line &l : c.lines) {  // what was found and what it was compared with
                            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 30 * s);
                            ImGui::TextColored(l.kind == 0 ? GOOD : l.kind == 1 ? WARN : DIM, "%s", l.text.c_str());
                        }
                        ImGui::PopID();
                    }
                    if (remove_at >= 0) g_cfg.game.erase(g_cfg.game.begin() + remove_at), config_save();
                    if (g_cfg.game.empty()) ImGui::TextColored(WARN, "Nothing chosen yet.");
                    if (ImGui::Button("Add folder...")) {
                        std::string st = g_cfg.game.empty() ? home_dir() : dialog_start(g_cfg.game.back());
                        SDL_ShowOpenFolderDialog(picked, (void *)(intptr_t)PICK_GAME, win, st.c_str(), true);
                    }
                    ImGui::SameLine();
                    {
                        char buf[4096];
                        snprintf(buf, sizeof buf, "%s", add_path.c_str());
                        float aw = 90 * s;
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - aw - ImGui::GetStyle().ItemSpacing.x);
                        if (ui_input_text("##add", buf, sizeof buf, 0, "or type / paste a folder path", "Game files")) add_path = buf;
                        ImGui::SameLine();
                        ImGui::BeginDisabled(trim(add_path).empty());
                        if (ImGui::Button("Add", ImVec2(aw, 0))) g_cfg.game.push_back(trim(add_path)), add_path.clear(), config_save();
                        ImGui::EndDisabled();
                    }
                    ImGui::EndChild();
                }
                ImGui::Spacing();

                // ---- keys and firmware: for the shared fonts
                if (section_begin("##c2", "Keys and firmware")) {
                    ImGui::TextDisabled("Only for the game's fonts%s", fonts_needed ? "" : " (already copied)");
                    help_marker("Your console's prod.keys and its firmware (the folder of its system .nca files): the game's "
                                "fonts come from the firmware, and it stops at the title screen without them. They're read "
                                "where they are and never copied; key values are never shown or written to the logs.");
                    if (path_row("prod.keys", g_cfg.keys, "prod.keys: its path", PICK_KEYS, false, s)) config_save();
                    status_line(keys, 130 * s);
                    if (path_row("Firmware", g_cfg.firmware, "firmware: e.g. Ryujinx's bis/system/Contents/registered", PICK_FIRMWARE, true, s))
                        config_save();
                    status_line(fw, 130 * s);
                    ImGui::EndChild();
                }
                ImGui::Spacing();

                // ---- OpenCourse World: Course World and every online feature of the game
                if (section_begin("##c3", "OpenCourse World")) {
                    ImGui::TextDisabled("The config exported from your opencourse.world account");
                    help_marker("The ocw-config.json that opencourse.world gives you: your server domain and your personal "
                                "token. The game's online features all go through OpenCourse World.");
                    if (path_row("Config", g_cfg.ocw, "ocw-config.json: its path", PICK_OCW, false, s)) config_save();
                    status_line(ocwc, 130 * s);
                    ImGui::EndChild();
                }
            } else {
                // ---- the log: what the Build Helper's console shows, in a window of its own
                std::vector<LogLine> lines;
                pthread_mutex_lock(&g_lock);
                lines = g_log;
                pthread_mutex_unlock(&g_lock);
                ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                                  ImGuiWindowFlags_NavFlattened);
                bool at_end = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4 * s;
                for (auto &l : lines) {
                    ImGui::PushStyleColor(ImGuiCol_Text, l.kind == 1 ? BLUE : l.kind == 2 ? GOOD : l.kind == 3 ? BAD : DIM);
                    ImGui::PushTextWrapPos(0);
                    if (l.kind == 1) ImGui::Text("[%02u:%02u] %s", l.secs / 60, l.secs % 60, l.text.c_str());
                    else ImGui::Text("  %s", l.text.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                }
                static size_t shown;
                if (at_end && lines.size() != shown) ImGui::SetScrollHereY(1.0f);
                shown = lines.size();
                ImGui::EndChild();
            }
            ImGui::EndChild();
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x);

            // ---- footer: what is going on at the left, the button at the right (Setup, or Play when back from the game)
            ImGui::Spacing();
            float bh = 56 * s, bw = 220 * s;
            float right = ImGui::GetWindowContentRegionMax().x - bw;
            if (job == 0) {
                ImGui::BeginGroup();
                if (!can_start && !files_ready)
                    ImGui::TextColored(WARN, "%s", g_cfg.game.empty() ? "Add the folder with your decrypted game."
                                                   : !games_ok ? "One of the game folders can't be used."
                                                   : !ocwc.ok ? "Choose your OpenCourse World config (ocw-config.json)."
                                                               : "Choose prod.keys and the firmware: the fonts come from there.");
                else if (g_built_wait)
                    ImGui::TextColored(GOOD, "The game code is ready. Press Play (on a Steam Deck, Gaming Mode is best).");
                else
                    ImGui::TextColored(GOOD, "%s", files_ready && !fonts_needed ? "Everything is set up."
                                                   : "Ready to set up: the first time takes a while.");
                bool uc = g_cfg.update_check;
                if (ImGui::Checkbox("Check for updates", &uc)) {
                    g_cfg.update_check = uc, config_save();
                    if (uc) launcher_update_check_start();
                }
                ImGui::SameLine(0, 24 * s);
                ImGui::TextDisabled("%s", RECOMP_VERSION);
                const char *tag = launcher_update_tag();
                if (tag) {
                    ImGui::SameLine(0, 24 * s);
                    ImGui::TextColored(GOLD, "Version %s is out.", tag);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Open the download page")) SDL_OpenURL(RELEASES_PAGE);
                }
#ifdef __APPLE__
                if (quarantined) {
                    ImGui::SameLine();
                    ImGui::TextColored(WARN, "macOS may block the recompiler: see README (xattr).");
                }
#endif
                ImGui::EndGroup();
                float gh = ImGui::GetItemRectSize().y;
                ImGui::SameLine(right);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (gh > bh ? (gh - bh) / 2 : 0));
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
                std::string what;
                pthread_mutex_lock(&g_lock);
                for (auto &st : g_steps)
                    if (st.state == RUNNING) { what = st.detail.empty() ? st.name : st.detail; break; }
                pthread_mutex_unlock(&g_lock);
                ImGui::TextColored(BLUE, "%s", text_bar(g_progress, what).c_str());
                ImGui::BeginGroup();
                ImGui::TextDisabled("Closing the window stops it; it starts over next time.");
                ImGui::EndGroup();
                float gh = ImGui::GetItemRectSize().y;
                ImGui::SameLine(right);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (gh > bh ? (gh - bh) / 2 : 0));
                ImGui::BeginDisabled(true);
                big_button("Setting up...", false, bw, bh);
                ImGui::EndDisabled();
            } else {
                ImGui::BeginGroup();
                ImGui::TextColored(BAD, "Setup stopped.");
                ImGui::EndGroup();
                float gh = ImGui::GetItemRectSize().y;
                ImGui::SameLine(right);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (gh > bh ? (gh - bh) / 2 : 0));
                if (big_button("Back", true, bw, bh)) g_job = 0;
            }
            // pinned to the very bottom edge of the window (never above the footer)
            float coffee_y = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - ImGui::GetTextLineHeight();
            if (coffee_y > ImGui::GetCursorPosY()) ImGui::SetCursorPosY(coffee_y);
            coffee_line();
            ImGui::End();
            osk_draw(g_font_big, s);
            ImGui::Render();

            // screenshots: whenever what's shown changes, and every 30 s while a step runs
            const char *shot = NULL;
            char shot_path[1200];
            if (shots) {
                int key = job * 100;
                pthread_mutex_lock(&g_lock);
                for (int i = 0; i < 3; i++) key += g_steps[i].state * (i == 0 ? 1 : i == 1 ? 5 : 25);
                pthread_mutex_unlock(&g_lock);
                double now = SDL_GetTicks() / 1000.0;
                if (frame >= 30 && (key != last_shot_key || (job == 1 && now - last_shot_t > 30))) {
                    last_shot_key = key, last_shot_t = now;
                    snprintf(shot_path, sizeof shot_path, "%s/launcher_%02d_%s.png", shots, shot_n++,
                             job == 0 ? "choose" : job == 1 ? "running" : job == 3 ? "error" : "done");
                    shot = shot_path;
                }
            }
#ifdef __APPLE__
            ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
            [enc endEncoding];
            id<MTLTexture> copy = nil;
            if (shot) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:drawable.texture.pixelFormat
                                             width:drawable.texture.width height:drawable.texture.height mipmapped:NO];
                td.storageMode = MTLStorageModeShared;
                copy = [dev newTextureWithDescriptor:td];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:drawable.texture toTexture:copy];
                [bl endEncoding];
            }
            [cb presentDrawable:drawable];
            [cb commit];
            if (copy) {
                [cb waitUntilCompleted];
                save_png(copy, shot);
                LOG("[launcher] screenshot %s\n", shot);
            }
#else
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)vk_frame_cmd());
            vk_frame_end(shot);
            if (shot) LOG("[launcher] screenshot %s\n", shot);
#endif
        }
        frame++;
    }
    LOG("[launcher] ready: starting the game\n");
    config_save();
    launcher_boot_env();
#ifdef __APPLE__
    ImGui_ImplMetal_Shutdown();
#else
    vk_imgui_backend_shutdown();
#endif
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
    SDL_SetWindowTitle(win, GAME_TITLE);
}
#endif
