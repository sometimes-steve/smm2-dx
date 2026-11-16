// The Mii editor's preview (mii_render.h): Arian's Mii Renderer (https://mii-unsecure.ariankordi.net/), as the owner's
// mii-editor uses it: GET /miis/image.png with the Mii's 88 bytes as hex.
#include "mii_render.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../net/http.h"
#include "../platform.h"
#include "../rt.h"
#include "../../third_party/stb/stb_image.h"

#define SETTLE_NS 450000000ull  // a change waits this long before it is sent

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_started, g_set, g_busy, g_failed;
static uint8_t g_want[0x58], g_done[0x58];
static uint64_t g_want_ns;
#define MAX_SIDE 512
static uint8_t *g_px[2];  // two pictures: the one shown stays whole while the next is written into the other
static int g_cur = -1, g_w, g_h;
static unsigned g_seq;
static char g_why[96];

static void *worker(void *unused) {
    (void)unused;
    for (;;) {
        plat_sleep_until_ns(plat_now_ns() + 100000000ull);
        pthread_mutex_lock(&g_lock);
        bool go = g_set && memcmp(g_want, g_done, 0x58) && plat_now_ns() - g_want_ns >= SETTLE_NS;
        uint8_t d[0x58];
        if (go) memcpy(d, g_want, 0x58), g_busy = true;
        pthread_mutex_unlock(&g_lock);
        if (!go) continue;
        char url[320], why[96] = "";
        int n = snprintf(url, sizeof url, "https://mii-unsecure.ariankordi.net/miis/image.png?data=");
        for (int i = 0; i < 0x58; i++) n += snprintf(url + n, sizeof url - (size_t)n, "%02x", d[i]);
        snprintf(url + n, sizeof url - (size_t)n, "&type=face&expression=0&characterYRotate=0&width=256&shaderType=0");
        HttpResponse r;
        int st = http_request("GET", url, "Accept: image/png\r\n", NULL, 0, 20000, &r);
        uint8_t *px = NULL;
        int w = 0, h = 0, c;
        if (st == 200 && r.body) {
            px = stbi_load_from_memory((const uint8_t *)r.body, (int)r.len, &w, &h, &c, 4);
            if (!px || w > MAX_SIDE || h > MAX_SIDE) snprintf(why, sizeof why, "the picture couldn't be read"), free(px), px = NULL;
        } else if (st) snprintf(why, sizeof why, "the renderer answered %d", st);
        else snprintf(why, sizeof why, "couldn't reach the renderer");
        http_free(&r);
        pthread_mutex_lock(&g_lock);
        memcpy(g_done, d, 0x58);
        g_busy = false;
        g_failed = px == NULL;
        if (px) {
            int next = g_cur == 0 ? 1 : 0;
            if (!g_px[next]) g_px[next] = malloc((size_t)MAX_SIDE * MAX_SIDE * 4);
            if (g_px[next]) memcpy(g_px[next], px, (size_t)w * h * 4), g_cur = next, g_w = w, g_h = h, g_seq++;
            stbi_image_free(px);
        }
        if (!px) snprintf(g_why, sizeof g_why, "%s", why);
        pthread_mutex_unlock(&g_lock);
        LOG("[mii] preview: %s\n", px ? "drawn" : why);
    }
    return NULL;
}

void mii_render_want(const uint8_t d[0x58]) {
    pthread_mutex_lock(&g_lock);
    if (!g_set || memcmp(g_want, d, 0x58)) memcpy(g_want, d, 0x58), g_want_ns = plat_now_ns(), g_set = true;
    bool start = !g_started;
    g_started = true;
    pthread_mutex_unlock(&g_lock);
    if (start) {
        pthread_t t;
        if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
    }
}

const uint8_t *mii_render_get(int *w, int *h, unsigned *seq, int *state, char *why) {
    pthread_mutex_lock(&g_lock);
    bool same = g_set && !memcmp(g_want, g_done, 0x58);
    *state = same ? (g_failed ? 2 : 0) : 1;
    snprintf(why, 96, "%s", g_why);
    *w = g_w, *h = g_h, *seq = g_seq;
    const uint8_t *px = g_cur >= 0 ? g_px[g_cur] : NULL;
    pthread_mutex_unlock(&g_lock);
    return px;  // (replaced only by the worker, and then only after the next picture is in: the menu copies it to the GPU this frame)
}
