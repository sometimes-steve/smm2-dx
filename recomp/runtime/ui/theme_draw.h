// Pixel-art drawing for the NES menu theme (the look of https://nostalgic-css.github.io/NES.css/): notched boxes with
// inset shadows, and small icons drawn pixel by pixel. Used by menu.mm and option_rows.mm.
#pragma once
#include <math.h>
#include <string.h>

#include "ui/ig.h"

#define NES_RGB(r, g, b) IM_COL32(r, g, b, 255)
// NES.css's palette
#define NES_WHITE   NES_RGB(255, 255, 255)
#define NES_BLACK   NES_RGB(33, 37, 41)      // #212529
#define NES_GREY    NES_RGB(173, 175, 188)   // #adafbc
#define NES_BLUE    NES_RGB(32, 156, 238)    // #209cee  (primary)
#define NES_BLUE_D  NES_RGB(0, 107, 179)     // #006bb3
#define NES_GREEN   NES_RGB(146, 204, 65)    // #92cc41  (success)
#define NES_GREEN_D NES_RGB(74, 165, 46)     // #4aa52e
#define NES_YELLOW  NES_RGB(247, 213, 29)    // #f7d51d  (warning)
#define NES_ORANGE  NES_RGB(229, 148, 0)     // #e59400
#define NES_RED     NES_RGB(231, 110, 85)    // #e76e55  (error)
#define NES_RED_D   NES_RGB(140, 32, 34)     // #8c2022

// A bitmap: n rows of '.' (clear) or a digit 1-9 picking pal[digit - 1]; each pixel px screen pixels square.
static inline void nes_icon(ImDrawList *dl, ImVec2 p, const char *const *rows, size_t n, float px, const ImU32 *pal,
                            bool flip) {
    for (size_t y = 0; y < n; y++) {
        int w = (int)strlen(rows[y]);
        for (int x = 0; x < w; x++) {
            if (rows[y][x] == '.') continue;
            int xx = flip ? w - 1 - x : x;
            ImDrawList_AddRectFilled(dl, IM_VEC2(floorf(p.x + xx * px), floorf(p.y + (float)y * px)),
                                     IM_VEC2(floorf(p.x + (xx + 1) * px), floorf(p.y + (float)(y + 1) * px)),
                                     pal[rows[y][x] - '1'], 0, 0);
        }
    }
}
// (for an array of row strings)
#define NES_ROWS(a) ((size_t)(sizeof(a) / sizeof((a)[0])))
#define nes_icon_w(rows) ((int)strlen((rows)[0]))
#define nes_icon_h(rows) ((int)NES_ROWS(rows))

static const char *const kNesArrow[] = {"1....", "11...", "111..", "1111.", "11111", "1111.", "111..", "11...", "1...."};  // points right
static const char *const kNesCross[] = {"11...11", "111.111", ".11111.", "..111..", ".11111.", "111.111", "11...11"};

// The NES.css container: a box whose corners are one step (u px) - the corner pixel missing, the border a stair - with an
// optional inset shadow along the bottom and right (the buttons' look). Fill / shadow alpha 0 skips them.
static inline void nes_box(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 fill, ImU32 border, float u, ImU32 shadow) {
    u = floorf(u < 1 ? 1 : u);
    a = IM_VEC2(floorf(a.x), floorf(a.y));
    b = IM_VEC2(floorf(b.x), floorf(b.y));
    ImDrawList_AddRectFilled(dl, IM_VEC2(a.x + 2 * u, a.y + u), IM_VEC2(b.x - 2 * u, b.y - u), fill, 0, 0);
    ImDrawList_AddRectFilled(dl, IM_VEC2(a.x + u, a.y + 2 * u), IM_VEC2(b.x - u, b.y - 2 * u), fill, 0, 0);
    if (shadow & IM_COL32_A_MASK) {
        ImDrawList_AddRectFilled(dl, IM_VEC2(a.x + u, b.y - 2 * u), IM_VEC2(b.x - u, b.y - u), shadow, 0, 0);
        ImDrawList_AddRectFilled(dl, IM_VEC2(b.x - 2 * u, a.y + 2 * u), IM_VEC2(b.x - u, b.y - u), shadow, 0, 0);
    }
    ImDrawList_AddRectFilled(dl, IM_VEC2(a.x + u, a.y), IM_VEC2(b.x - u, a.y + u), border, 0, 0);          // top
    ImDrawList_AddRectFilled(dl, IM_VEC2(a.x + u, b.y - u), IM_VEC2(b.x - u, b.y), border, 0, 0);          // bottom
    ImDrawList_AddRectFilled(dl, IM_VEC2(a.x, a.y + u), IM_VEC2(a.x + u, b.y - u), border, 0, 0);          // left
    ImDrawList_AddRectFilled(dl, IM_VEC2(b.x - u, a.y + u), IM_VEC2(b.x, b.y - u), border, 0, 0);          // right
    ImDrawList_AddRectFilled(dl, IM_VEC2(a.x + u, a.y + u), IM_VEC2(a.x + 2 * u, a.y + 2 * u), border, 0, 0);  // the step in each corner
    ImDrawList_AddRectFilled(dl, IM_VEC2(b.x - 2 * u, a.y + u), IM_VEC2(b.x - u, a.y + 2 * u), border, 0, 0);
    ImDrawList_AddRectFilled(dl, IM_VEC2(a.x + u, b.y - 2 * u), IM_VEC2(a.x + 2 * u, b.y - u), border, 0, 0);
    ImDrawList_AddRectFilled(dl, IM_VEC2(b.x - 2 * u, b.y - 2 * u), IM_VEC2(b.x - u, b.y - u), border, 0, 0);
}
