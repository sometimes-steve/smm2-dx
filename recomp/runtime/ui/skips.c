// Endless Challenge skips (skips.h).
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "skips.h"

#define FILE_NAME "endless-skips.txt"  // (relative: the data root is the working directory)

static _Atomic long g_total;
static _Atomic int g_session;
static _Atomic int g_loaded;

static void save(void) {
    FILE *f = fopen(FILE_NAME, "w");
    if (!f) return;
    fprintf(f, "total_skips = %ld%s", (long)g_total, "\n");
    fclose(f);
}

static void load(void) {
    if (atomic_exchange(&g_loaded, 1)) return;
    FILE *f = fopen(FILE_NAME, "r");
    if (f) {
        long v = 0;
        if (fscanf(f, " total_skips = %ld", &v) == 1 && v > 0 && v < 1000000000L) g_total = v;
        fclose(f);
    }
    atexit(save);
}

void skips_note(void) {
    load();
    g_session++, g_total++;
    save();  // (a few bytes, once per skip: nothing is lost if the game is closed hard)
}

int skips_session(void) { return g_session; }
long skips_total(void) {
    load();
    return g_total;
}
