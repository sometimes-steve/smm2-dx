// Time played per character (character.h).
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "../game_rows.h"
#include "character.h"
#include "playtime.h"

#define FILE_NAME "character.txt"  // (relative: the data root is the working directory) lines of "<name> = <seconds>"

static const char *const NAMES[CHARACTER_COUNT] = {"Mario", "Luigi", "Toad", "Toadette"};
static double g_secs[CHARACTER_COUNT];
static double g_last_save;
static bool g_loaded, g_dirty;

const char *character_name(int id) { return id >= 0 && id < CHARACTER_COUNT ? NAMES[id] : ""; }

static void save(void) {
    if (!g_dirty) return;
    FILE *f = fopen(FILE_NAME, "w");
    if (!f) return;
    for (int i = 0; i < CHARACTER_COUNT; i++) fprintf(f, "%s = %.0f\n", NAMES[i], g_secs[i]);
    fclose(f);
    g_dirty = false;
}

static void load(void) {
    g_loaded = true;
    FILE *f = fopen(FILE_NAME, "r");
    if (f) {
        char name[32];
        double v;
        while (fscanf(f, " %31[A-Za-z] = %lf", name, &v) == 2)
            for (int i = 0; i < CHARACTER_COUNT; i++)
                if (!strcmp(name, NAMES[i]) && v > 0 && v < 1e9) g_secs[i] = v;
        fclose(f);
    }
    atexit(save);
}

void character_tick(void) {
    if (!g_loaded) load();
    double dt = playtime_last_frame();
    if (dt <= 0) return;
    int id = game_current_character();
    if (id < 0 || id >= CHARACTER_COUNT) return;  // (not in a level, or not known)
    g_secs[id] += dt, g_dirty = true;
    double now = SDL_GetTicks() / 1000.0;
    if (now - g_last_save > 60) g_last_save = now, save();
}

double character_seconds(int id) {
    if (!g_loaded) load();
    return id >= 0 && id < CHARACTER_COUNT ? g_secs[id] : 0;
}

bool character_most_played(char *name, unsigned long name_cap, char *time, unsigned long time_cap) {
    if (!g_loaded) load();
    int best = -1;
    for (int i = 0; i < CHARACTER_COUNT; i++)
        if (g_secs[i] >= 1 && (best < 0 || g_secs[i] > g_secs[best])) best = i;
    if (best < 0) return false;
    snprintf(name, name_cap, "%s", NAMES[best]);
    playtime_format(g_secs[best], time, time_cap);
    return true;
}
