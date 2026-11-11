// Sessions and play time by day, hour and game style (history.h).
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../game_rows.h"
#include "history.h"
#include "playtime.h"

#define FILE_NAME "history.txt"  // (relative: the data root is the working directory) lines of "<key> = <values>"
#define KEEP_DAYS 30

static const char *const STYLES[HISTORY_STYLES] = {"Super Mario Bros.", "Super Mario Bros. 3", "Super Mario World",
                                                   "New Super Mario Bros. U", "Super Mario 3D World"};
static struct {
    double longest;
    double session_sum;  // seconds in the sessions before this run
    int sessions, days, streak, best_streak;
    long last_day;  // local calendar day number of the last day played
    struct { long day; double secs; } daily[KEEP_DAYS];  // oldest first
    int ndaily;
    double heat[7][24];
    double style[HISTORY_STYLES];
} g;
static bool g_loaded, g_dirty, g_counted;  // g_counted: this run has been counted as a session
static double g_last_save;

static long civil_days(long y, long m, long d) {  // days since 1970-01-01 (days_from_civil)
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static struct tm local_now(void) {
    time_t t = time(NULL);
    struct tm l;
    localtime_s(&l, &t);
    return l;
}

static long day_number(void) {
    struct tm l = local_now();
    return civil_days(l.tm_year + 1900, l.tm_mon + 1, l.tm_mday);
}

static void save(void) {
    FILE *f = fopen(FILE_NAME, "w");
    if (!f) return;
    fprintf(f, "longest = %.0f\nsession_sum = %.0f\nsessions = %d\ndays = %d\nstreak = %d\nbest_streak = %d\nlast_day = %ld\n",
            g.longest, g.session_sum + (g_counted ? playtime_session() : 0), g.sessions, g.days, g.streak, g.best_streak,
            g.last_day);
    for (int i = 0; i < g.ndaily; i++) fprintf(f, "day = %ld %.0f\n", g.daily[i].day, g.daily[i].secs);
    fprintf(f, "heat =");
    for (int w = 0; w < 7; w++)
        for (int h = 0; h < 24; h++) fprintf(f, " %.0f", g.heat[w][h]);
    fprintf(f, "\nstyle =");
    for (int i = 0; i < HISTORY_STYLES; i++) fprintf(f, " %.0f", g.style[i]);
    fprintf(f, "\n");
    fclose(f);
    g_dirty = false;
}

static void save_if_dirty(void) {
    if (g_dirty) save();
}

static void load(void) {
    g_loaded = true;
    FILE *f = fopen(FILE_NAME, "r");
    if (f) {
        static char line[2048];
        while (fgets(line, sizeof line, f)) {
            double d;
            long n, day;
            if (sscanf(line, "longest = %lf", &d) == 1) g.longest = d;
            else if (sscanf(line, "session_sum = %lf", &d) == 1) g.session_sum = d;
            else if (sscanf(line, "sessions = %ld", &n) == 1) g.sessions = (int)n;
            else if (sscanf(line, "days = %ld", &n) == 1) g.days = (int)n;
            else if (sscanf(line, "streak = %ld", &n) == 1) g.streak = (int)n;
            else if (sscanf(line, "best_streak = %ld", &n) == 1) g.best_streak = (int)n;
            else if (sscanf(line, "last_day = %ld", &n) == 1) g.last_day = n;
            else if (sscanf(line, "day = %ld %lf", &day, &d) == 2) {
                if (g.ndaily < KEEP_DAYS) g.daily[g.ndaily].day = day, g.daily[g.ndaily++].secs = d;
            } else if (!strncmp(line, "heat =", 6)) {
                char *p = line + 6;
                for (int i = 0; i < 168; i++) {
                    char *e;
                    double v = strtod(p, &e);
                    if (e == p) break;
                    g.heat[i / 24][i % 24] = v, p = e;
                }
            } else if (!strncmp(line, "style =", 7)) {
                char *p = line + 7;
                for (int i = 0; i < HISTORY_STYLES; i++) {
                    char *e;
                    double v = strtod(p, &e);
                    if (e == p) break;
                    g.style[i] = v, p = e;
                }
            }
        }
        fclose(f);
    }
    atexit(save_if_dirty);
}

void history_tick(void) {
    if (!g_loaded) load();
    double dt = playtime_last_frame();
    if (dt <= 0) return;
    struct tm l = local_now();
    long today = civil_days(l.tm_year + 1900, l.tm_mon + 1, l.tm_mday);
    if (!g_counted) {  // the first counted frame of this run: a session, and a day played
        g_counted = true;
        g.sessions++;
        if (today != g.last_day) {
            g.days++;
            g.streak = today == g.last_day + 1 ? g.streak + 1 : 1;
            if (g.streak > g.best_streak) g.best_streak = g.streak;
            g.last_day = today;
        }
    }
    if (playtime_session() > g.longest) g.longest = playtime_session();
    if (!g.ndaily || g.daily[g.ndaily - 1].day != today) {
        if (g.ndaily == KEEP_DAYS) memmove(g.daily, g.daily + 1, (KEEP_DAYS - 1) * sizeof g.daily[0]), g.ndaily--;
        g.daily[g.ndaily].day = today, g.daily[g.ndaily++].secs = 0;
    }
    g.daily[g.ndaily - 1].secs += dt;
    g.heat[l.tm_wday][l.tm_hour] += dt;
    int style = game_current_style();
    if (style >= 0 && style < HISTORY_STYLES) g.style[style] += dt;
    g_dirty = true;
    double now = SDL_GetTicks() / 1000.0;
    if (now - g_last_save > 60) g_last_save = now, save_if_dirty();
}

double history_longest_session(void) {
    if (!g_loaded) load();
    return g.longest;
}
double history_average_session(void) {
    if (!g_loaded) load();
    return g.sessions ? (g.session_sum + (g_counted ? playtime_session() : 0)) / g.sessions : 0;
}
int history_sessions(void) {
    if (!g_loaded) load();
    return g.sessions;
}
int history_days_played(void) {
    if (!g_loaded) load();
    return g.days;
}
int history_best_streak(void) {
    if (!g_loaded) load();
    return g.best_streak;
}
int history_streak(void) {
    if (!g_loaded) load();
    long today = day_number();
    return g.last_day && (g.last_day == today || g.last_day == today - 1) ? g.streak : 0;  // (broken once a day was missed)
}

double history_day_seconds(int days_ago) {
    if (!g_loaded) load();
    long want = day_number() - days_ago;
    for (int i = 0; i < g.ndaily; i++)
        if (g.daily[i].day == want) return g.daily[i].secs;
    return 0;
}
int history_weekday(int days_ago) {
    long d = day_number() - days_ago;
    return (int)(((d + 4) % 7 + 7) % 7);  // (1970-01-01 was a Thursday)
}
double history_heat(int weekday, int hour) {
    if (!g_loaded) load();
    return weekday >= 0 && weekday < 7 && hour >= 0 && hour < 24 ? g.heat[weekday][hour] : 0;
}
const char *history_style_name(int id) { return id >= 0 && id < HISTORY_STYLES ? STYLES[id] : ""; }
double history_style_seconds(int id) {
    if (!g_loaded) load();
    return id >= 0 && id < HISTORY_STYLES ? g.style[id] : 0;
}
