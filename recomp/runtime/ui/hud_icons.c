#include "hud_icons.h"

#include <stdlib.h>
#include <string.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "../../third_party/nanosvg/nanosvg.h"
#include "../../third_party/nanosvg/nanosvgrast.h"

uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);  // runtime/nvn/vulkan/imgui_glue.c

static const char deaths_svg[] = {
#embed "icons/deaths.svg"
    , 0};
static const char skip_svg[] = {
#embed "icons/skip.svg"
    , 0};

enum { SIZES = 6 };
static struct { int px; uint64_t tex; } made[2][SIZES];

uint64_t hud_icon(int which, int px) {
    if (which < 0 || which > 1 || px < 8) return 0;
    px = (px + 15) & ~15;
    int free_slot = -1;
    for (int i = 0; i < SIZES; i++) {
        if (made[which][i].px == px) return made[which][i].tex;
        if (!made[which][i].px && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0 || px > 512) return 0;  // (a window being dragged to every size: no more than a few are made)
    const char *src = which == HUD_ICON_DEATHS ? deaths_svg : skip_svg;
    char *copy = strdup(src);  // (nanosvg edits the text it parses)
    NSVGimage *img = copy ? nsvgParse(copy, "px", 96.0f) : NULL;
    NSVGrasterizer *r = img ? nsvgCreateRasterizer() : NULL;
    uint8_t *rgba = r ? calloc((size_t)px * px, 4) : NULL;
    uint64_t tex = 0;
    if (rgba && img->width > 0 && img->height > 0) {
        float s = (float)px / (img->width > img->height ? img->width : img->height);
        nsvgRasterize(r, img, 0, 0, s, rgba, px, px, px * 4);
        tex = vk_imgui_texture_rgba(rgba, px, px);
    }
    free(rgba);
    if (r) nsvgDeleteRasterizer(r);
    if (img) nsvgDelete(img);
    free(copy);
    made[which][free_slot].px = px, made[which][free_slot].tex = tex;
    return tex;
}
