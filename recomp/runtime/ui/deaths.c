// Deaths per course (deaths.h).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "deaths.h"

#define FILE_NAME "course-deaths.txt"  // (relative: the data root is the working directory)
#define MAX_COURSES 100000

static struct { uint32_t id; unsigned n; } *g_rows;
static int g_count, g_cap;
static int g_loaded;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int find(uint32_t id) {
    for (int i = 0; i < g_count; i++)
        if (g_rows[i].id == id) return i;
    return -1;
}

static int add_row(uint32_t id, unsigned n) {
    if (g_count >= MAX_COURSES) return -1;
    if (g_count == g_cap) {
        int cap = g_cap ? g_cap * 2 : 256;
        void *p = realloc(g_rows, (size_t)cap * sizeof *g_rows);
        if (!p) return -1;
        g_rows = p, g_cap = cap;
    }
    g_rows[g_count].id = id, g_rows[g_count].n = n;
    return g_count++;
}

static void load_locked(void) {
    if (g_loaded) return;
    g_loaded = 1;
    FILE *f = fopen(FILE_NAME, "r");
    if (!f) return;
    unsigned id, n;
    while (fscanf(f, "%u %u", &id, &n) == 2)
        if (id && n && find(id) < 0) add_row(id, n);
    fclose(f);
}

static void save_locked(void) {
    FILE *f = fopen(FILE_NAME, "w");
    if (!f) return;
    for (int i = 0; i < g_count; i++) fprintf(f, "%u %u\n", g_rows[i].id, g_rows[i].n);
    fclose(f);
}

void deaths_add(uint32_t course) {
    if (!course) return;
    pthread_mutex_lock(&g_lock);
    load_locked();
    int i = find(course);
    if (i < 0) i = add_row(course, 0);
    if (i >= 0) g_rows[i].n++;
    save_locked();
    pthread_mutex_unlock(&g_lock);
}

unsigned deaths_get(uint32_t course) {
    pthread_mutex_lock(&g_lock);
    load_locked();
    int i = find(course);
    unsigned n = i < 0 ? 0 : g_rows[i].n;
    pthread_mutex_unlock(&g_lock);
    return n;
}
