#include "numfont.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rt.h"
#include "../nvn/shader_ahead.h"

uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);  // runtime/nvn/vulkan/imgui_glue.c

#define FONT_FILE "Font/BmpFont_US.szs"
#define FONT_NAME "Mario_NumberFont2.bffnt"
enum { MAX_GLYPHS = 64, MAX_CHARS = 64 };

static struct {
    int state;                       // 0 not tried, 1 ready, 2 not available
    uint64_t tex;
    int sw, sh, cw, ch, cols;        // the sheet, a glyph's cell, the cells in a row
    struct { uint8_t left, glyph_w, advance; } gl[MAX_GLYPHS];
    struct { uint32_t code; int glyph; } map[MAX_CHARS];
    int nmap;
} g;

static uint32_t u16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | u16(p + 2) << 16; }

// one member of a SARC pack (little endian), by name: a pointer into d and its size
static const uint8_t *sarc_member(const uint8_t *d, size_t len, const char *name, size_t *size) {
    if (len < 0x20 || memcmp(d, "SARC", 4) || u16(d + 6) != 0xFEFF || memcmp(d + 0x14, "SFAT", 4)) return NULL;
    uint32_t data_off = u32(d + 0xC), count = u16(d + 0x1A);
    size_t sfnt = 0x20 + (size_t)count * 16;
    if (sfnt + 8 > len || memcmp(d + sfnt, "SFNT", 4)) return NULL;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *nd = d + 0x20 + (size_t)i * 16;
        size_t no = sfnt + 8 + (size_t)(u32(nd + 4) & 0xFFFFFF) * 4;
        uint32_t start = u32(nd + 8), end = u32(nd + 12);
        if (no >= len || end < start || (size_t)data_off + end > len) return NULL;
        if (strncmp((const char *)d + no, name, len - no)) continue;
        *size = end - start;
        return d + data_off + start;
    }
    return NULL;
}

// BC4: eight bytes -> 16 values
static void bc4(const uint8_t *b, uint8_t out[16]) {
    int a0 = b[0], a1 = b[1], t[8];
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    t[0] = a0, t[1] = a1;
    if (a0 > a1) for (int i = 1; i < 7; i++) t[1 + i] = ((7 - i) * a0 + i * a1) / 7;
    else {
        for (int i = 1; i < 5; i++) t[1 + i] = ((5 - i) * a0 + i * a1) / 5;
        t[6] = 0, t[7] = 255;
    }
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)t[(bits >> (3 * i)) & 7];
}

// The font's glyph sheet: a BNTX inside the font file, one BC5 texture (w x h) in the Switch's block-linear layout, block height 1 << bh_log2
// gobs. Decoded to RGBA, flipped the right way up: the picture is the red channel (the digit, white inside and black on its edge) and the
// alpha is the green channel (the shape with its edge), so a tint colours the inside and the outline stays black.
static uint8_t *sheet_rgba(const uint8_t *bn, size_t n, int *w_out, int *h_out) {
    const uint8_t *brti = NULL;
    for (size_t i = 0; i + 4 <= n && !brti; i++)
        if (!memcmp(bn + i, "BRTI", 4)) brti = bn + i;
    if (!brti || n < 0x1000) return NULL;
    uint32_t fmt = u32(brti + 0x1C), w = u32(brti + 0x24), h = u32(brti + 0x28), bh = 1u << (u32(brti + 0x34) & 7);
    if (fmt != 0x1e01 || !w || !h || (w & 3) || (h & 3) || w > 4096 || h > 4096) return NULL;  // (BC5)
    uint32_t wb = w / 4, hb = h / 4, pitch = wb * 16, gobs_w = (pitch + 63) / 64;
    size_t need = (size_t)0x1000 + (size_t)((hb + 8 * bh - 1) / (8 * bh)) * 512 * bh * gobs_w;
    if (need > n) return NULL;
    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba) return NULL;
    for (uint32_t y = 0; y < hb; y++) {
        for (uint32_t x = 0; x < wb; x++) {
            uint32_t xb = x * 16;
            size_t a = 0x1000 + (size_t)(y / (8 * bh)) * 512 * bh * gobs_w + (size_t)(xb / 64) * 512 * bh + (size_t)((y % (8 * bh)) / 8) * 512;
            uint32_t xx = xb % 64, yy = y % 8;
            a += (size_t)((xx % 64) / 32) * 256 + (size_t)((yy % 8) / 2) * 64 + (size_t)((xx % 32) / 16) * 32 + (size_t)(yy % 2) * 16 + (xx % 16);
            uint8_t r[16], gch[16];
            bc4(bn + a, r), bc4(bn + a + 8, gch);
            for (int k = 0; k < 16; k++) {
                uint32_t px = x * 4 + (uint32_t)(k % 4), py = h - 1 - (y * 4 + (uint32_t)(k / 4));  // (upside down in the file)
                uint8_t *o = rgba + ((size_t)py * w + px) * 4;
                o[0] = o[1] = o[2] = r[k], o[3] = gch[k];
            }
        }
    }
    *w_out = (int)w, *h_out = (int)h;
    return rgba;
}

static bool load(void) {
    char path[600];
    snprintf(path, sizeof path, "%s/" FONT_FILE, g_romfs_dir ? g_romfs_dir : "extracted/romfs");
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *packed = n > 0 && n < (64 << 20) ? malloc((size_t)n) : NULL;
    bool ok = packed && fread(packed, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    size_t raw_n = 0, ff_n = 0;
    uint8_t *raw = ok ? yaz0_unpack(packed, (size_t)n, &raw_n) : NULL;
    free(packed);
    const uint8_t *ff = raw ? sarc_member(raw, raw_n, FONT_NAME, &ff_n) : NULL;
    uint8_t *rgba = NULL;
    ok = false;
    if (ff && ff_n > 0x40 && !memcmp(ff, "FFNT", 4) && u16(ff + 4) == 0xFEFF) {
        // the blocks: FINF, TGLP (the sheet), CWDH (widths), CMAP (characters)
        size_t pos = u16(ff + 6), tglp = 0, cwdh = 0, cmap = 0;
        for (unsigned i = 0, nb = u16(ff + 0x10); i < nb && pos + 8 <= ff_n; i++) {
            uint32_t sz = u32(ff + pos + 4);
            if (!memcmp(ff + pos, "TGLP", 4)) tglp = pos;
            else if (!memcmp(ff + pos, "CWDH", 4)) cwdh = pos;
            else if (!memcmp(ff + pos, "CMAP", 4)) cmap = pos;
            if (sz < 8) break;
            pos += sz;
        }
        if (tglp && cwdh && cmap && tglp + 0x20 <= ff_n && cwdh + 16 <= ff_n && cmap + 0x20 <= ff_n) {
            g.cw = ff[tglp + 8], g.ch = ff[tglp + 9], g.cols = (int)u16(ff + tglp + 0x14);
            uint32_t ssize = u32(ff + tglp + 0x0C), off = u32(ff + tglp + 0x1C);
            uint32_t first = u16(ff + cwdh + 8), last = u16(ff + cwdh + 10);
            uint32_t typ = u16(ff + cmap + 0x10), nm = u32(ff + cmap + 0x18);
            if (g.cw && g.ch && g.cols && (size_t)off + ssize <= ff_n && first == 0 && last < MAX_GLYPHS && cwdh + 16 + 3 * (last + 1) <= ff_n &&
                typ == 2 && nm <= MAX_CHARS && cmap + 0x1c + 8 * (size_t)nm <= ff_n && !u32(ff + cmap + 0x14)) {
                for (uint32_t i = 0; i <= last; i++)
                    g.gl[i].left = ff[cwdh + 16 + 3 * i], g.gl[i].glyph_w = ff[cwdh + 17 + 3 * i], g.gl[i].advance = ff[cwdh + 18 + 3 * i];
                for (uint32_t i = 0; i < nm; i++) {
                    g.map[i].code = u32(ff + cmap + 0x1c + 8 * i), g.map[i].glyph = (int)u32(ff + cmap + 0x20 + 8 * i);
                    if (g.map[i].glyph > (int)last) g.map[i].glyph = 0;
                }
                g.nmap = (int)nm;
                rgba = sheet_rgba(ff + off, ssize, &g.sw, &g.sh);
                ok = rgba != NULL;
            }
        }
    }
    if (ok) {
        g.tex = vk_imgui_texture_rgba(rgba, g.sw, g.sh);
        ok = g.tex != 0;
    }
    free(rgba), free(raw);
    return ok;
}

bool numfont_ready(void) {
    if (!g.state) g.state = load() ? 1 : 2;
    return g.state == 1;
}

static int glyph_of(unsigned char c) {
    uint32_t code = c == 'x' ? 0xD7 : c;  // (a plain x is the font's multiplication sign)
    for (int i = 0; i < g.nmap; i++)
        if (g.map[i].code == code) return g.map[i].glyph;
    return -1;
}

float numfont_width(const char *s, float h) {
    if (!numfont_ready()) return 0;
    float k = h / (float)g.ch, w = 0;
    for (; *s; s++) {
        int gi = glyph_of((unsigned char)*s);
        if (gi >= 0) w += g.gl[gi].advance * k;
    }
    return w;
}

float numfont_draw(ImDrawList *dl, float x, float y, float h, const char *s, ImU32 fill) {
    if (!numfont_ready()) return 0;
    float k = h / (float)g.ch, x0 = x;
    for (; *s; s++) {
        int gi = glyph_of((unsigned char)*s);
        if (gi < 0) continue;
        int col = gi % g.cols, row = gi / g.cols;
        float sx = (float)(col * (g.cw + 1)), sy = (float)(row * (g.ch + 1));
        ImVec2 a = IM_VEC2(x + (int8_t)g.gl[gi].left * k, y), b = IM_VEC2(a.x + g.cw * k, y + g.ch * k);
        ImDrawList_AddImage(dl, (ImTextureID)g.tex, a, b, IM_VEC2(sx / g.sw, sy / g.sh), IM_VEC2((sx + g.cw) / g.sw, (sy + g.ch) / g.sh), fill);
        x += g.gl[gi].advance * k;
    }
    return x - x0;
}
