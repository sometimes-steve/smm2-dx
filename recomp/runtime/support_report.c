// The support report (support_report.h).
#include "support_report.h"

#include <windows.h>

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "game.h"
#include "net/netcfg.h"
#include "paths.h"
#include "platform.h"
#include "support_redact.h"
#include "usermods.h"

#define LOG_TAIL (120 * 1024)   // how much of each log goes in
#define CRASH_KEEP 5            // crash logs kept

// ---- files
static char *read_tail(const char *path, size_t max, size_t *len) {  // the end of a file, from the start of a line; NULL: not readable
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > (long)max ? size - (long)max : 0;
    fseek(f, from, SEEK_SET);
    char *buf = malloc((size_t)(size - from) + 1);
    size_t n = buf ? fread(buf, 1, (size_t)(size - from), f) : 0;
    fclose(f);
    if (!buf) return NULL;
    buf[n] = 0;
    if (from > 0) {
        char *q = memchr(buf, '\n', n);
        if (q) memmove(buf, q + 1, n - (size_t)(q + 1 - buf) + 1), n -= (size_t)(q + 1 - buf);
    }
    *len = n;
    return buf;
}

static bool file_has(const char *path, const char *needle) {  // (the end of it: a fatal error is the last thing a log says)
    size_t n;
    char *t = read_tail(path, 256 * 1024, &n);
    bool has = t && strstr(t, needle);
    free(t);
    return has;
}

// ---- a growing text
typedef struct {
    char *p;
    size_t n, cap;
} Text;
static void tput(Text *t, const char *s, size_t len) {
    if (t->n + len + 1 > t->cap) {
        t->cap = (t->n + len + 1) * 2;
        t->p = realloc(t->p, t->cap);
        if (!t->p) abort();
    }
    memcpy(t->p + t->n, s, len);
    t->n += len;
    t->p[t->n] = 0;
}
static void tputs(Text *t, const char *s) { tput(t, s, strlen(s)); }
static void tf(Text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tf(Text *t, const char *fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    tputs(t, b);
}

// ---- what is taken out
typedef struct {
    RedactLiteral v[64];
    char store[64][600];  // (the strings the literals point at)
    int n;
} Lits;

static void add_lit(Lits *l, const char *find, const char *with) {
    if (l->n >= 64 || !find || strlen(find) < 3 || strlen(find) >= 600) return;  // (shorter than three: it would hit ordinary words)
    snprintf(l->store[l->n], sizeof l->store[l->n], "%s", find);
    l->v[l->n].find = l->store[l->n];
    l->v[l->n].with = with;
    l->n++;
}
// a path in both slash styles
static void add_path(Lits *l, const char *path, const char *with) {
    char a[600];
    snprintf(a, sizeof a, "%s", path);
    add_lit(l, a, with);
    for (char *p = a; *p; p++) if (*p == '\\') *p = '/';
    add_lit(l, a, with);
}
// every string value of 6 or more characters in a JSON file (an OpenCourse World config: its domain, address and token)
static void add_json_values(Lits *l, const char *path, const char *with) {
    size_t n;
    char *t = read_tail(path, 64 * 1024, &n);
    if (!t) return;
    for (size_t i = 0; i < n; i++) {
        if (t[i] != '"') continue;
        size_t j = i + 1;
        while (j < n && t[j] != '"' && t[j] != '\n') j += t[j] == '\\' ? 2 : 1;
        if (j < n && t[j] == '"') {
            size_t k = j + 1;
            while (k < n && (t[k] == ' ' || t[k] == '\t')) k++;
            bool is_key = k < n && t[k] == ':';  // (a key: not a secret)
            if (!is_key && j - i - 1 >= 6 && j - i - 1 < 500) {
                char v[520];
                snprintf(v, sizeof v, "%.*s", (int)(j - i - 1), t + i + 1);
                add_lit(l, v, with);
            }
        }
        i = j;
    }
    free(t);
}
static void parent_dir(char *p) {  // "a\b\c" -> "a\b"
    char *s = strrchr(p, '\\');
    char *f = strrchr(p, '/');
    if (f > s) s = f;
    if (s) *s = 0;
}

static void build_lits(Lits *l) {
    memset(l, 0, sizeof *l);
    char data[600];
    snprintf(data, sizeof data, "%s", rt_data_root());
    for (char *p = data; *p; p++) if (*p == '/') *p = '\\';
    size_t dl = strlen(data);
    if (dl && data[dl - 1] == '\\') data[dl - 1] = 0;
    // the OpenCourse World account: the domain, what the config holds, the maker code
    char host[300] = "";
    if (net_ocw_host(host, sizeof host)) add_lit(l, host, "<ocw domain>");
    const char *cfg = getenv("RECOMP_OCW_CONFIG");
    add_json_values(l, cfg && *cfg ? cfg : "ocw-config.json", "<ocw config value>");
    {   // the setup's own choice of a config elsewhere: <data>/launcher.ini "ocw=<path>"
        FILE *f = fopen("launcher.ini", "r");
        char line[1200];
        while (f && fgets(line, sizeof line, f)) {
            if (strncmp(line, "ocw=", 4)) continue;
            line[strcspn(line, "\r\n")] = 0;
            add_json_values(l, line + 4, "<ocw config value>");
        }
        if (f) fclose(f);
    }
    {   // ocw-maker.txt: a fingerprint and the maker code
        size_t n;
        char *t = read_tail("ocw-maker.txt", 1024, &n);
        if (t) {
            for (char *w = strtok(t, " \r\n\t"); w; w = strtok(NULL, " \r\n\t")) add_lit(l, w, "<ocw maker>");
            free(t);
        }
    }
    // the folders (longest first: the data folder is inside the program's, which is inside the user's)
    add_path(l, data, "<data>");
    char app[600];
    snprintf(app, sizeof app, "%s", data);
    parent_dir(app);
    if (app[0]) add_path(l, app, "<app>");
    const char *prof = getenv("USERPROFILE");
    if (prof && *prof) add_path(l, prof, "%USERPROFILE%");
    const char *user = getenv("USERNAME");
    const char *pc = getenv("COMPUTERNAME");
    if (pc && *pc) add_lit(l, pc, "<computer>");
    if (user && *user) add_lit(l, user, "<user>");
}

// ---- the PC
static void system_section(Text *t, const char *cur_log) {
    OSVERSIONINFOEXW ov = {sizeof ov};
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOEXW *);
    RtlGetVersionFn rv = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rv && rv(&ov) == 0) tf(t, "Windows: %lu.%lu build %lu\n", ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber);
    char brand[49] = "";
    {
        int r[4];
        unsigned *b = (unsigned *)brand;
        for (unsigned leaf = 0; leaf < 3; leaf++) {
            __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0x80000002u + leaf), "c"(0));
            memcpy(b + leaf * 4, r, 16);
        }
        brand[48] = 0;
        size_t bl = strlen(brand);
        while (bl && brand[bl - 1] == ' ') brand[--bl] = 0;  // (the CPU pads its name)
        char *lead = brand;
        while (*lead == ' ') lead++;
        memmove(brand, lead, strlen(lead) + 1);
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    MEMORYSTATUSEX ms = {sizeof ms};
    GlobalMemoryStatusEx(&ms);
    tf(t, "CPU: %s, %lu logical processors\n", brand[0] ? brand : "(unknown)", si.dwNumberOfProcessors);
    tf(t, "Memory: %.1f GB, %.1f GB free\n", (double)ms.ullTotalPhys / (1 << 30), (double)ms.ullAvailPhys / (1 << 30));
    // the graphics: what the session's log says (the Vulkan device, its API version, the swapchain)
    size_t n;
    char *log = read_tail(cur_log, LOG_TAIL * 4, &n);
    int shown = 0;
    if (log) {
        for (char *line = strtok(log, "\n"); line && shown < 8; line = strtok(NULL, "\n")) {
            if (strstr(line, "[vk] gpu ") || strstr(line, "[vk] using ") || strstr(line, "swapchain ") || strstr(line, "driver")) {
                tf(t, "%s\n", line);
                shown++;
            }
        }
        free(log);
    }
    if (!shown) tputs(t, "Graphics: (the log has no device line)\n");
}

// ---- the settings that matter: an allowlist of the settings file's keys (graphics, sound, input, start-up, language)
static const char *const SETTING_KEYS[] = {
    "aspect", "fullscreen", "hide_cursor", "volume", "mute", "mute_in_background", "reverb", "movie_audio", "skip_intro",
    "skip_mode_select", "skip_mode_select_to", "keep_obj_cache", "show_fps", "show_clock", "show_memory", "menu_scale", "ahead_fast",
    "theme", "upscaler", "frame_gen", "antialias", "resolution", "inv_keys", "dpad_move", "pads_from_p2", "move_360", "prompts",
    "mirror_mode", "language"};

static void settings_section(Text *t) {
    const char *path = getenv("LARECOMP_SETTINGS");
    FILE *f = fopen(path && *path ? path : GAME_SETTINGS, "r");
    if (!f) {
        tputs(t, "(no settings file: the program's defaults)\n");
        return;
    }
    char line[400];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        size_t kl = (size_t)(eq - line);
        while (kl && line[kl - 1] == ' ') kl--;
        for (size_t k = 0; k < sizeof SETTING_KEYS / sizeof *SETTING_KEYS; k++)
            if (strlen(SETTING_KEYS[k]) == kl && !strncmp(line, SETTING_KEYS[k], kl)) {
                line[strcspn(line, "\r\n")] = 0;
                tf(t, "%s\n", line);
                n++;
                break;
            }
    }
    fclose(f);
    if (!n) tputs(t, "(none of the listed settings are in the file)\n");
}

static void mods_section(Text *t) {
    usermods_scan();
    int n = usermods_count(), on = 0;
    for (int i = 0; i < n; i++) {
        UserMod m;
        if (!usermods_get(i, &m) || !m.enabled) continue;
        tf(t, "%d. %s %s (%s%s)\n", ++on, m.name, m.version, m.gb_id ? "GameBanana " : "by hand", m.gb_id ? m.id : "");
    }
    if (!on) tputs(t, "(no mods on)\n");
    if (usermods_crash_note()[0]) tf(t, "The last run stopped with these on, so they were turned off at this start: %s\n", usermods_crash_note());
}

// ---- crash logs kept
static void crash_dir_list(const char *dir, char names[CRASH_KEEP + 8][64], int *count) {
    char pat[600];
    snprintf(pat, sizeof pat, "%scrash-*.log", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    *count = 0;
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (*count < CRASH_KEEP + 8) snprintf(names[(*count)++], 64, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
static int cmp_names(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

void support_crash_keep(const char *log, const char *dir) {
    if (!log || !dir || !file_has(log, "FATAL: ")) return;
    WIN32_FILE_ATTRIBUTE_DATA ad;
    time_t when = time(NULL);
    if (GetFileAttributesExA(log, GetFileExInfoStandard, &ad)) {  // (when the session ended: the log's last write)
        ULARGE_INTEGER u = {{ad.ftLastWriteTime.dwLowDateTime, ad.ftLastWriteTime.dwHighDateTime}};
        when = (time_t)((u.QuadPart - 116444736000000000ull) / 10000000ull);
    }
    struct tm tmv;
    localtime_s(&tmv, &when);
    char name[700];
    snprintf(name, sizeof name, "%scrash-%04d%02d%02d-%02d%02d%02d.log", dir, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min,
             tmv.tm_sec);
    CopyFileA(log, name, FALSE);
    char names[CRASH_KEEP + 8][64];
    int n;
    crash_dir_list(dir, names, &n);
    qsort(names, (size_t)n, 64, cmp_names);
    for (int i = 0; i + CRASH_KEEP < n; i++) {  // (the oldest go)
        char old[700];
        snprintf(old, sizeof old, "%s%s", dir, names[i]);
        remove(old);
    }
}

static const char *prev_log_path(char *buf, size_t cap) {
    const char *cur = plat_log_file();
    if (!cur || !*cur) return NULL;
    size_t n = strlen(cur);
    if (n < 4 || n + 16 > cap) return NULL;
    snprintf(buf, cap, "%.*s.previous.log", (int)(n - 4), cur);
    return buf;
}

bool support_last_run_failed(void) {
    char p[1100];
    return prev_log_path(p, sizeof p) && file_has(p, "FATAL: ");
}

// ---- the report
static void log_section(Text *t, const char *title, const char *path, const RedactLiteral *lits, int nl) {
    size_t n;
    char *raw = read_tail(path, LOG_TAIL, &n);
    tf(t, "\n== %s ==\n", title);
    if (!raw) {
        tputs(t, "(not there)\n");
        return;
    }
    size_t cap = n * 2 + 64;
    char *red = malloc(cap);
    if (red) {
        support_redact(raw, n, red, cap, lits, nl);
        tputs(t, red);
        free(red);
    }
    free(raw);
}

char *support_report_build(const char *version) {
    Text t = {0};
    Lits *lits = malloc(sizeof *lits);
    if (!lits) abort();
    build_lits(lits);
    time_t now = time(NULL);
    struct tm tmv;
    localtime_s(&tmv, &now);
    char stamp[64];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tmv);

    // The sections that carry free text are redacted as a whole at the end (the header, the settings and the PC are built from the
    // program's own values and the log lines): so the report is built first, then every line of it goes through the redaction.
    Text raw = {0};
    tf(&raw, "== The program ==\n%s %s\nGame: Super Mario Maker 2 3.0.3\nGenerated: %s\n", GAME_TITLE, version ? version : "?", stamp);
    tputs(&raw, "\n== The PC ==\n");
    system_section(&raw, plat_log_file());
    tputs(&raw, "\n== Settings (graphics, sound, input, start-up, language) ==\n");
    settings_section(&raw);
    tputs(&raw, "\n== Mods that are on, in order (a later one's files win) ==\n");
    mods_section(&raw);

    char logdir[600] = "", prev[1100];
    plat_log_dir(logdir, sizeof logdir);
    tputs(&raw, "\n== The last run ==\n");
    tf(&raw, "Ended on a fatal error: %s\n", support_last_run_failed() ? "yes" : "no");
    char names[CRASH_KEEP + 8][64];
    int nc = 0;
    if (logdir[0]) crash_dir_list(logdir, names, &nc);
    qsort(names, (size_t)nc, 64, cmp_names);
    tf(&raw, "Crash logs kept: %d%s\n", nc, nc ? " (newest first below)" : "");
    for (int i = nc - 1; i >= 0; i--) tf(&raw, "  %s\n", names[i]);
    tputs(&raw, "Performance: no sample (the frame-time report is not part of this version)\n");

    // the report so far, redacted; then the logs, each redacted as they go in
    size_t cap = raw.n * 2 + 64;
    char *red = malloc(cap);
    if (red) {
        support_redact(raw.p, raw.n, red, cap, lits->v, lits->n);
        tputs(&t, "smm2-dx support report\n======================\n");
        tputs(&t, "Made on this PC and not sent anywhere. Names, folders, account details and tokens are taken out: look it over before you send it.\n\n");
        tputs(&t, red);
        free(red);
    }
    free(raw.p);
    if (nc) {  // the newest crash log's end
        char p[700];
        snprintf(p, sizeof p, "%s%s", logdir, names[nc - 1]);
        char title[160];
        snprintf(title, sizeof title, "The newest crash log (%s), the end of it", names[nc - 1]);
        log_section(&t, title, p, lits->v, lits->n);
    }
    log_section(&t, "This session's log, the end of it", plat_log_file(), lits->v, lits->n);
    if (prev_log_path(prev, sizeof prev)) log_section(&t, "The last session's log, the end of it", prev, lits->v, lits->n);
    tputs(&t, "\n== Never included ==\nprod.keys and the firmware, the game's files and its compiled code, saves, OpenCourse World's token and configuration, "
              "the Miis and courses, launcher.ini's paths.\n");
    free(lits);
    return t.p ? t.p : strdup("");
}

bool support_report_copy(const char *version, char *msg, size_t cap) {
    char *r = support_report_build(version);
    bool ok = SDL_SetClipboardText(r);
    if (ok) snprintf(msg, cap, "Support report copied (%zu KB). Paste it where you are reporting the problem.", strlen(r) / 1024 + 1);
    else snprintf(msg, cap, "The clipboard could not be written (%s). Use Export Support Report instead.", SDL_GetError());
    free(r);
    return ok;
}

bool support_report_export(const char *version, char *path, size_t path_cap, char *msg, size_t cap) {
    char dir[600];
    if (!plat_log_dir(dir, sizeof dir)) {
        snprintf(msg, cap, "There is no log folder to write the report in.");
        return false;
    }
    time_t now = time(NULL);
    struct tm tmv;
    localtime_s(&tmv, &now);
    snprintf(path, path_cap, "%ssupport-report-%04d%02d%02d-%02d%02d%02d.txt", dir, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
             tmv.tm_min, tmv.tm_sec);
    char *r = support_report_build(version);
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(r, 1, strlen(r), f) == strlen(r);
    if (f && fclose(f) != 0) ok = false;
    free(r);
    if (ok) snprintf(msg, cap, "Support report saved: %s", path);
    else snprintf(msg, cap, "The report could not be written to %s (the folder may be read-only or the disk full).", dir);
    return ok;
}
