#include "pad_icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../third_party/nanosvg/nanosvg.h"
#include "../../third_party/nanosvg/nanosvgrast.h"
#include "../input_bind.h"

uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);  // runtime/nvn/vulkan/imgui_glue.c

typedef struct { const char *key, *svg; } PadSvg;
typedef struct { const char *label, *key; } PadAlias;
#include "pad_icons.inc"

enum { CELL = 96, COLS = 8, FAMS = 3 };
static const PadSvg *const k_icons[FAMS] = {k_nin_icons, k_xb_icons, k_ps_icons};
static const int k_nicons[FAMS] = {sizeof k_nin_icons / sizeof k_nin_icons[0], sizeof k_xb_icons / sizeof k_xb_icons[0],
                                   sizeof k_ps_icons / sizeof k_ps_icons[0]};
static const char *const *const k_hosts[FAMS] = {k_nin_hosts, k_xb_hosts, k_ps_hosts};
static const PadAlias *const k_labels[FAMS] = {k_nin_labels, k_xb_labels, k_ps_labels};
static const int k_nlabels[FAMS] = {sizeof k_nin_labels / sizeof k_nin_labels[0], sizeof k_xb_labels / sizeof k_xb_labels[0],
                                    sizeof k_ps_labels / sizeof k_ps_labels[0]};

static uint64_t g_sheet[FAMS];
static bool g_tried[FAMS];

static uint64_t sheet(int fam) {
    if (g_sheet[fam] || g_tried[fam]) return g_sheet[fam];
    g_tried[fam] = true;
    int n = k_nicons[fam], rows = (n + COLS - 1) / COLS, w = COLS * CELL, h = rows * CELL;
    uint8_t *rgba = calloc((size_t)w * h, 4), *cell = malloc((size_t)CELL * CELL * 4);
    NSVGrasterizer *r = nsvgCreateRasterizer();
    if (rgba && cell && r) {
        for (int i = 0; i < n; i++) {
            char *copy = strdup(k_icons[fam][i].svg);  // (nanosvg edits the text it parses)
            NSVGimage *img = copy ? nsvgParse(copy, "px", 96.0f) : NULL;
            if (img && img->width > 0 && img->height > 0) {
                memset(cell, 0, (size_t)CELL * CELL * 4);
                nsvgRasterize(r, img, 0, 0, (float)CELL / (img->width > img->height ? img->width : img->height), cell, CELL, CELL, CELL * 4);
                for (int y = 0; y < CELL; y++)
                    memcpy(rgba + ((size_t)((i / COLS) * CELL + y) * w + (i % COLS) * CELL) * 4, cell + (size_t)y * CELL * 4, CELL * 4);
            }
            if (img) nsvgDelete(img);
            free(copy);
        }
        g_sheet[fam] = vk_imgui_texture_rgba(rgba, w, h);
    }
    if (r) nsvgDeleteRasterizer(r);
    free(rgba), free(cell);
    return g_sheet[fam];
}

static bool icon_by_key(int fam, const char *key, uint64_t *tex, ImVec2 *uv0, ImVec2 *uv1) {
    if (fam < 0 || fam >= FAMS || !key) return false;
    for (int i = 0; i < k_nicons[fam]; i++) {
        if (strcmp(k_icons[fam][i].key, key)) continue;
        uint64_t t = sheet(fam);
        if (!t) return false;
        int rows = (k_nicons[fam] + COLS - 1) / COLS;
        float cw = 1.0f / COLS, ch = 1.0f / rows, inset = 0.5f / (COLS * CELL);  // (half a texel in: no neighbour's edge)
        *tex = t;
        *uv0 = IM_VEC2((i % COLS) * cw + inset, (i / COLS) * ch + inset);
        *uv1 = IM_VEC2((i % COLS + 1) * cw - inset, (i / COLS + 1) * ch - inset);
        return true;
    }
    return false;
}

bool pad_icon_host(int fam, int host, uint64_t *tex, ImVec2 *uv0, ImVec2 *uv1) {
    if (fam < 0 || fam >= FAMS || host < 0 || host >= (int)(sizeof k_nin_hosts / sizeof k_nin_hosts[0])) return false;
    return icon_by_key(fam, k_hosts[fam][host], tex, uv0, uv1);
}

bool pad_icon_slot(int slot, uint64_t *tex, ImVec2 *uv0, ImVec2 *uv1) {
    static const char *const key[BIND_PAD_COUNT] = {
        [BIND_A] = "A", [BIND_B] = "B", [BIND_X] = "X", [BIND_Y] = "Y", [BIND_LSTICK] = "LStick", [BIND_RSTICK] = "RStick",
        [BIND_L] = "L", [BIND_R] = "R", [BIND_ZL] = "ZL", [BIND_ZR] = "ZR", [BIND_PLUS] = "Plus", [BIND_MINUS] = "Minus",
        [BIND_LEFT] = "DLeft", [BIND_UP] = "DUp", [BIND_RIGHT] = "DRight", [BIND_DOWN] = "DDown",
    };
    return slot >= 0 && slot < BIND_PAD_COUNT && icon_by_key(0, key[slot], tex, uv0, uv1);
}

float pad_label_draw(ImDrawList *dl, int fam, const char *label, ImVec2 at, float h, bool draw) {
    if (fam < 0 || fam >= FAMS || !label) return 0;
    char buf[64];
    snprintf(buf, sizeof buf, "%s", label);
    const float gap = h * 0.25f;
    float x = at.x;
    for (char *part = buf; part;) {  // "L / R": two pictures
        char *sep = strstr(part, " / "), *next = NULL;
        if (sep) *sep = 0, next = sep + 3;
        const char *key = part;
        for (int i = 0; i < k_nlabels[fam]; i++)
            if (!strcmp(part, k_labels[fam][i].label)) key = k_labels[fam][i].key;
        uint64_t tex;
        ImVec2 a, b;
        if (!icon_by_key(fam, key, &tex, &a, &b)) return 0;
        if (draw) ImDrawList_AddImage(dl, (ImTextureID)tex, IM_VEC2(x, at.y), IM_VEC2(x + h, at.y + h), a, b, IM_COL32(255, 255, 255, 255));
        x += h + (next ? gap : 0);
        part = next;
    }
    return x - at.x;
}
