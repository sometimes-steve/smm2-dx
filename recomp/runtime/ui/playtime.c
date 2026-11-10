// Play time (playtime.h).
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "playtime.h"

#define FILE_NAME "playtime.txt"  // (relative: the data root is the working directory)

static double g_total, g_session;
static double g_last_tick, g_last_save;
static bool g_loaded, g_dirty;

static void save(void) {
    if (!g_dirty) return;
    FILE *f = fopen(FILE_NAME, "w");
    if (!f) return;
    fprintf(f, "total_seconds = %.0f%s", g_total, "\n");
    fclose(f);
    g_dirty = false;
}

static void load(void) {
    g_loaded = true;
    FILE *f = fopen(FILE_NAME, "r");
    if (f) {
        double v = 0;
        if (fscanf(f, " total_seconds = %lf", &v) == 1 && v > 0 && v < 1e9) g_total = v;
        fclose(f);
    }
    atexit(save);
}

void playtime_tick(bool playing) {
    if (!g_loaded) load();
    double now = SDL_GetTicks() / 1000.0;
    double dt = g_last_tick > 0 ? now - g_last_tick : 0;
    g_last_tick = now;
    if (!playing) return;
    if (dt > 0.25) dt = 0.25;  // (a stall, or the first frame after a pause: not a quarter of a second more than it was)
    g_total += dt, g_session += dt, g_dirty = true;
    if (now - g_last_save > 60) g_last_save = now, save();
}

double playtime_session(void) { return g_session; }
double playtime_total(void) {
    if (!g_loaded) load();
    return g_total;
}

void playtime_format(double seconds, char *out, unsigned long cap) {
    long m = (long)(seconds / 60);
    if (m < 1) snprintf(out, cap, "Under 1 min");
    else if (m < 60) snprintf(out, cap, "%ld min", m);
    else snprintf(out, cap, "%ld h %ld min", m / 60, m % 60);
}
