// Settings pages (see option_rows.h): two panes (rows on the left, the selected / hovered row's description on the right) or one
// (full-width rows, the description as a tooltip), with a sidebar of sections on pages that have several.
#include "ui/ig.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "option_rows.h"
const char *menu_clipboard(void);  // (menu.mm: the system clipboard's text)
#include "osk.h"
#include "theme_draw.h"

#define DESC_CAP 9000  // (GameBanana mod pages: runtime/ui/mods_page.c)
#define TXT_CAP 640

static float g_scale = 1;
static char g_desc_hover[DESC_CAP], g_desc_focus[DESC_CAP], g_desc_shown[DESC_CAP];  // this frame's hovered / selected row, and the last shown
static char g_desc_first[DESC_CAP];  // the page's first row (shown until a row is selected or hovered)
typedef struct { uint64_t tex; int w, h; } DescImage;
static DescImage g_img_hover, g_img_focus, g_img_shown;  // a picture for the description pane (opt_desc_image)

// the menu theme's accent / caption colours (runtime/ui/menu.cpp)
uint32_t menu_accent(int a);
uint32_t menu_muted(void);
float menu_row_round(void);   // the theme's row corner radius / outline width (px at 1080 p)
float menu_row_border(void);
float menu_row_notch(void);   // > 0: the NES theme (stepped pixel corners, pointer, pixel arrows) instead of rounded rows
#define kGold menu_accent(255)
static bool g_next_disabled;
void opt_disable_next(void) { g_next_disabled = true; }
static int g_indent;
static bool g_focus_req, g_take_first;  // (opt_focus_first: requested / this page's first row takes it)
void opt_focus_first(bool on) { g_focus_req = on; }
static bool g_held;  // the last row's left / right has been held a while (steppers go faster)
static bool g_touch;
void opt_touch(bool on) { g_touch = on; }
void opt_indent(int step) { g_indent = g_indent + step < 0 ? 0 : g_indent + step; }

// Defaults (issue #8): a row that differs from its default gets a dot; right-click (X on a controller) on it resets that row;
// the page's last row resets them all (asks once). g_page_rows / g_page_changed: this page's rows that have a default / differ.
static int g_page_rows, g_page_changed;
static bool g_reset_row;     // this frame: the row just drawn was asked to reset
static bool g_reset_page;    // this frame: every row with a default goes back to it
static bool g_reset_ask;     // the reset button was clicked once (waiting for the second click)
static char g_reset_ask_page[64];

static void set_desc(char *dst, const char *src) { snprintf(dst, DESC_CAP, "%s", src); }

static char g_cur_page[64];
static bool g_flat;  // a one-pane page: no description pane, a tooltip instead
// One-pane rows are as wide as the page needs, not the window: the widest label and the widest value of the page (measured
// in the page's last frame, in the font it is drawn in: the NES theme's wide letters get wider rows), the value column right
// after the labels. g_*_cur: this frame's widest; g_*_use: what the page's rows use (its last frame's).
static float g_labw_cur, g_valw_cur, g_labw_use, g_valw_use;
static struct { char id[64]; float labw, valw; } g_flat_cache[24];
static float flat_layout(float avail, float *vcol) {
    float pad = 16 * g_scale, fs = igGetFontSize(), ap = fmaxf(2.0f, floorf(fs * 0.15f));
    float lead = menu_row_notch() > 0 ? 8 * ap : 0;
    float vc = pad + lead + (g_labw_use > 0 ? g_labw_use : avail * 0.4f) + 2 * pad;
    // never narrower than the same minimum on every tab (the volume steppers on a short page looked small), the spare room
    // going between the labels and the values
    float w = fminf(avail, fmaxf(g_labw_use > 0 ? vc + g_valw_use + pad : 640 * g_scale, 580 * g_scale));
    if (g_labw_use > 0) vc = fmaxf(vc, w - pad - g_valw_use);
    if (vcol) *vcol = vc;
    return w;
}

// Title Case for labels, headings and choices: every lowercase word starts with a capital, except the small ones in the middle
// (to, for, the...); words that already have a capital (FPS, SMAA, OpenCourse) or start with a digit stay as written.
static void title_case(char *t) {
    static const char *const small[] = {"a", "an", "and", "as", "at", "but", "by", "for", "from", "in", "into", "of", "on", "or",
                                        "the", "to", "vs", "with"};
    size_t len = strlen(t);
    for (size_t i = 0; i < len;) {
        while (i < len && t[i] == ' ') i++;
        size_t s = i;
        while (i < len && t[i] != ' ') i++;
        size_t n = i - s;
        if (!n) break;
        bool keep = !(t[s] >= 'a' && t[s] <= 'z');
        for (size_t k = s + 1; k < s + n; k++)
            if (t[k] >= 'A' && t[k] <= 'Z') keep = true;
        bool first = s == 0, last = i >= len;
        if (!keep && !first && !last)
            for (size_t k = 0; k < sizeof small / sizeof small[0]; k++)
                if (strlen(small[k]) == n && !strncmp(t + s, small[k], n)) keep = true;
        if (!keep) t[s] = (char)toupper((unsigned char)t[s]);
        for (size_t k = s + 1; k + 1 < s + n; k++)  // (after a hyphen too: Three-Point)
            if (t[k] == '-' && t[k + 1] >= 'a' && t[k + 1] <= 'z') t[k + 1] = (char)toupper((unsigned char)t[k + 1]);
    }
}
// a row's description over the page, wrapped (one-pane pages)
static void flat_tooltip(const char *desc) {
    igBeginTooltip();
    igPushTextWrapPos(igGetFontSize() * 28);
    igTextUnformatted(desc, NULL);
    igPopTextWrapPos();
    igEndTooltip();
}

static float text_w(const char *s) {
    ImVec2 v;
    igCalcTextSize(&v, s, NULL, false, -1.0f);
    return v.x;
}

// t + "..." measured against room: drop characters from the end until it fits, never half a UTF-8 character
static void ellipsize(char *t, float room) {
    char tmp[TXT_CAP + 8];
    size_t n = strlen(t);
    for (;;) {
        snprintf(tmp, sizeof tmp, "%s...", t);
        if (!n || text_w(tmp) <= room) break;
        t[--n] = 0;
    }
    while (n && ((unsigned char)t[n - 1] & 0xC0) == 0x80) t[--n] = 0;  // (never half a UTF-8 character)
    if (n && (unsigned char)t[n - 1] >= 0xC0) t[--n] = 0;
    strcat(t, "...");
}

static void begin_page(const char *id, float scale, bool flat) {
    static char page[128];  // another page: its own first row's description, not the last page's
    if (strcmp(page, id)) snprintf(page, sizeof page, "%s", id), g_desc_shown[0] = 0;
    snprintf(g_cur_page, sizeof g_cur_page, "%s", id);
    // the pages pass fontSize / 34, and the NES theme's font (Press Start 2P, 20 px at 1080 p) is about half the default's
    // (Baloo 2, 39 px: runtime/ui/menu.c), so its minimum width and padding came out about half the size. Brought most of the
    // way up (1.4: the whole 39 / 20 made the boxes huge around the small pixel text; its rows stay 2.5 text heights tall).
    g_scale = menu_row_notch() > 0 ? scale * 1.4f : scale;
    g_take_first = g_focus_req;
    g_focus_req = false;
    if (g_take_first) igNavMoveRequestCancel();  // (this frame's Down goes to the first row instead)
    g_indent = 0;
    g_page_rows = g_page_changed = 0;
    if (strcmp(g_reset_ask_page, id)) g_reset_ask = false;  // (another page: the question is dropped)
    g_reset_row = false;
    g_desc_hover[0] = 0, g_desc_focus[0] = 0;
    g_img_hover = g_img_focus = (DescImage){0, 0, 0};
    ImVec2 avail;
    igGetContentRegionAvail(&avail);
    g_flat = flat;
    if (flat) {  // (this page's widths from its last frame)
        int k = 0;
        for (; k < 24 && strcmp(g_flat_cache[k].id, id); k++)
            if (!g_flat_cache[k].id[0]) break;
        if (k < 24 && !g_flat_cache[k].id[0]) snprintf(g_flat_cache[k].id, sizeof g_flat_cache[k].id, "%s", id);
        g_labw_use = k < 24 ? g_flat_cache[k].labw : 0, g_valw_use = k < 24 ? g_flat_cache[k].valw : 0;
        g_labw_cur = g_valw_cur = 0;
    }
    igPushID_Str(id);
    igBeginChild_Str("##rows", IM_VEC2(flat ? avail.x : avail.x * 0.48f, avail.y), ImGuiChildFlags_NavFlattened, 0);  // (was the obsolete ImGuiWindowFlags_NavFlattened)
    igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, IM_VEC2(igGetStyle()->ItemSpacing.x, (g_touch ? 8 : 4) * scale));  // (rows close)
    g_desc_first[0] = 0;
}
void opt_begin(const char *id, float scale) { begin_page(id, scale, false); }
void opt_flat_begin(const char *id, float scale) { begin_page(id, scale, true); }

void opt_section(const char *title) {
    char t[TXT_CAP];
    snprintf(t, sizeof t, "%s", title);
    title_case(t);
    if (igGetCursorPosY() > igGetStyle()->WindowPadding.y + 1) igDummy(IM_VEC2(0, 10 * g_scale));
    igPushStyleColor_U32(ImGuiCol_Text, menu_muted());
    igTextUnformatted(t, NULL);
    igPopStyleColor(1);
    ImVec2 p, avail;  // (a hairline under the heading, as Ship of Harkinian's separator text)
    igGetCursorScreenPos(&p);
    igGetContentRegionAvail(&avail);
    ImDrawList_AddLine(igGetWindowDrawList(), IM_VEC2(p.x, p.y + 1), IM_VEC2(p.x + (g_flat ? flat_layout(avail.x, NULL) : avail.x), p.y + 1),
                       menu_accent(70), 1.0f);
    igDummy(IM_VEC2(0, 4 * g_scale));
}

// One row: draws it, handles A / click and left / right while selected. Returns 2 (A / click), -1 / +1 (left / right) or 0.
// arrows: ◀ value ▶ (a choice of several, or a stepper): left / right step it, and the arrows click too
// (callers pass upper = true, arrows = false unless they need otherwise)
static bool g_row_has_def;  // (set by the opt_* that take a default, just before row())
static int row(const char *label, const char *value, bool changed, const char *desc, bool upper, bool arrows) {
    const bool has_def = g_row_has_def;
    g_row_has_def = false;
    g_reset_row = false;
    igPushID_Str(label);
    if (g_indent) igSetCursorPosX(igGetCursorPosX() + g_indent * 28 * g_scale);  // (an option of the row above)
    ImVec2 p;
    igGetCursorScreenPos(&p);
    const bool nes = menu_row_notch() > 0;  // (the NES theme: pixel-art buttons, pointer and arrows)
    ImVec2 availv;
    igGetContentRegionAvail(&availv);
    float vcol = 0, w = availv.x, h = igGetFontSize() * (g_touch || nes ? 2.5f : 1.9f);
    if (g_flat) {  // (one-pane pages: as wide as the page needs)
        char t0[TXT_CAP], s0[TXT_CAP];
        snprintf(t0, sizeof t0, "%s", label);
        if (upper) title_case(t0);
        snprintf(s0, sizeof s0, "%s", value);
        if (arrows) title_case(s0);
        float fs0 = igGetFontSize(), ap0 = fmaxf(2.0f, floorf(fs0 * 0.15f)), aw0 = nes ? 5 * ap0 : fs0 * 0.42f, gap0 = fs0 * 0.55f;
        float lw = text_w(t0), vwid = s0[0] ? text_w(s0) + (arrows ? 2 * aw0 + 2 * gap0 : 0) : 0;
        if (lw > g_labw_cur) g_labw_cur = lw;
        if (vwid > g_valw_cur) g_valw_cur = vwid;
        w = flat_layout(availv.x, &vcol);
    }
    igPushStyleColor_Vec4(ImGuiCol_NavCursor, IM_VEC4(0, 0, 0, 0));  // (the row's own border shows the selection)
    bool clicked = igInvisibleButton("##row", IM_VEC2(w, h), ImGuiButtonFlags_EnableNav);
    igPopStyleColor(1);
    if (g_take_first) {  // (opt_focus_first)
        g_take_first = false;
        igSetFocusID(igGetItemID(), igGetCurrentWindow());
        igSetNavCursorVisible(true);
        igSetScrollHereY(0);
    }
    bool off = g_next_disabled;  // (greyed out: still selectable, so its description shows, but it can't change)
    g_next_disabled = false;
    bool hov = igIsItemHovered(0), foc = igIsItemFocused() && igGetCurrentContext()->NavCursorVisible;
    int step = clicked && !off ? 2 : 0;
    if (has_def) {
        g_page_rows++;
        if (changed) g_page_changed++;
        if (changed && !off && ((hov && igIsMouseClicked_Bool(ImGuiMouseButton_Right, false)) ||
                                (foc && igIsKeyPressed_Bool(ImGuiKey_GamepadFaceLeft, false))))
            g_reset_row = true;
    }
    g_held = false;
    if (foc && !off) {
        if (igIsKeyPressed_Bool(ImGuiKey_GamepadDpadLeft, true) || igIsKeyPressed_Bool(ImGuiKey_GamepadLStickLeft, true) ||
            igIsKeyPressed_Bool(ImGuiKey_LeftArrow, true))
            step = -1;
        if (igIsKeyPressed_Bool(ImGuiKey_GamepadDpadRight, true) || igIsKeyPressed_Bool(ImGuiKey_GamepadLStickRight, true) ||
            igIsKeyPressed_Bool(ImGuiKey_RightArrow, true))
            step = 1;
        static const ImGuiKey held_keys[] = {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickLeft,
                                             ImGuiKey_GamepadLStickRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow};
        for (size_t i = 0; i < sizeof held_keys / sizeof held_keys[0]; i++)
            if (igGetKeyData_Key(held_keys[i])->DownDuration > 1.0f) g_held = true;
        // left / right belong to the row (navigation would otherwise try to move sideways)
        static const ImGuiKey own_keys[] = {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow,
                                            ImGuiKey_RightArrow};
        for (size_t i = 0; i < sizeof own_keys / sizeof own_keys[0]; i++) igSetKeyOwner(own_keys[i], igGetItemID(), 0);
    }
    if (foc) set_desc(g_desc_focus, desc ? desc : "");
    if (hov) set_desc(g_desc_hover, desc ? desc : "");
    if (!g_desc_first[0] && desc) set_desc(g_desc_first, desc);
    if (g_flat && (hov || foc) && desc && desc[0]) flat_tooltip(desc);

    ImDrawList *dl = igGetWindowDrawList();
    const bool plain = g_flat && !nes;  // (one-pane rows: no box until pointed at, square corners)
    float r = plain ? 0 : menu_row_round() * g_scale, pad = 16 * g_scale, fs = igGetFontSize();
    ImU32 fill = hov || foc ? IM_COL32(255, 255, 255, 22) : plain ? 0 : IM_COL32(0, 0, 0, 40);
    ImU32 edge = hov || foc ? (off ? menu_accent(120) : kGold) : plain ? 0 : IM_COL32(255, 255, 255, off ? 18 : 38);
    float bw = (hov || foc ? 2.0f * menu_row_border() : menu_row_border()) * (menu_row_border() > 1 ? g_scale : 1);
    const bool lit = (hov || foc) && !off;
    const float ap = fmaxf(2.0f, floorf(fs * 0.15f));  // (a pixel of the pointer / arrows)
    if (nes) {  // NES.css's buttons: stepped corners, an inset shadow along the bottom and right, blue when pointed at
        ImU32 nfill = off ? NES_RGB(27, 30, 34) : lit ? NES_BLUE : NES_RGB(47, 52, 58);
        ImU32 nshadow = off ? NES_RGB(20, 22, 25) : lit ? NES_BLUE_D : NES_RGB(26, 29, 32);
        ImU32 nedge = off ? NES_RGB(58, 63, 70) : lit ? NES_WHITE : NES_RGB(92, 97, 107);
        nes_box(dl, p, IM_VEC2(p.x + w, p.y + h), nfill, nedge, 3 * g_scale, nshadow);
    } else {
        ImDrawList_AddRectFilled(dl, p, IM_VEC2(p.x + w, p.y + h), fill, r, 0);
        ImDrawList_AddRect(dl, p, IM_VEC2(p.x + w, p.y + h), edge, r, 0, bw);
    }
    char t[TXT_CAP];
    snprintf(t, sizeof t, "%s", label);
    if (upper) title_case(t);
    float ty = p.y + (h - fs) * 0.5f;
    ImU32 label_col = off ? IM_COL32(110, 110, 102, 255) : nes ? NES_WHITE : IM_COL32(240, 236, 224, 255);  // (greyed out: clearly dim)
    float lead = nes ? 8 * ap : 0;  // (the NES pointer's room before the label)
    if (nes && lit) {  // the pointer: NES.css's selection arrow
        const ImU32 pc[] = {NES_WHITE};
        nes_icon(dl, IM_VEC2(p.x + pad, p.y + (h - 9 * ap) * 0.5f), kNesArrow, NES_ROWS(kNesArrow), ap, pc, false);
    }
    {  // a label that would run into the value is cut short with "..." (long names: mods, courses)
        float vw0 = !value[0] ? 0 : text_w(value);
        float vspace = !value[0] ? 0 : (vw0 < w * 0.6f ? vw0 : w * 0.6f) + fs * (arrows ? 2.4f : 0.9f);
        float room = g_flat ? vcol - 2 * pad - lead : w - 2 * pad - vspace - lead;  // (one-pane: up to the value column)
        if (room > fs && text_w(t) > room) ellipsize(t, room);
    }
    ImDrawList_AddText_Vec2(dl, IM_VEC2(p.x + pad + lead, ty), label_col, t, NULL);
    if (has_def && changed && !off) {  // differs from its default: a small accent dot before the label
        float dr = fmaxf(2.5f * g_scale, fs * 0.13f), dx = p.x + pad * 0.5f + (nes ? dr : 0), dy = p.y + h * 0.5f;
        if (nes) ImDrawList_AddRectFilled(dl, IM_VEC2(dx - dr, dy - dr), IM_VEC2(dx + dr, dy + dr), NES_WHITE, 0, 0);
        else ImDrawList_AddCircleFilled(dl, IM_VEC2(dx, dy), dr, kGold, 0);
    }
    float right = p.x + w - pad;  // (the value's right edge)
    if (arrows && value[0]) {  // ◀ value ▶, drawn (the fonts have no arrow glyphs); a click on one steps
        float aw = nes ? 5 * ap : fs * 0.42f, gap = fs * 0.55f, cy = ty + fs * 0.52f;
        if (g_flat) right = p.x + vcol + aw + gap + text_w(value) + gap + aw;  // (one-pane: the value starts beside the label)
        float rx = right - aw, lx = rx - gap - text_w(value) - gap - aw;
        ImU32 ac = off ? IM_COL32(110, 110, 100, 255) : nes ? (lit ? NES_WHITE : NES_GREY) : hov || foc ? kGold : IM_COL32(170, 170, 160, 255);
        if (nes) {  // stepped pixel arrows
            const ImU32 pc[] = {ac};
            nes_icon(dl, IM_VEC2(lx, p.y + (h - 9 * ap) * 0.5f), kNesArrow, NES_ROWS(kNesArrow), ap, pc, true);
            nes_icon(dl, IM_VEC2(rx, p.y + (h - 9 * ap) * 0.5f), kNesArrow, NES_ROWS(kNesArrow), ap, pc, false);
        } else {
            ImDrawList_AddTriangleFilled(dl, IM_VEC2(lx, cy), IM_VEC2(lx + aw, cy - aw * 0.75f), IM_VEC2(lx + aw, cy + aw * 0.75f), ac);
            ImDrawList_AddTriangleFilled(dl, IM_VEC2(rx + aw, cy), IM_VEC2(rx, cy - aw * 0.75f), IM_VEC2(rx, cy + aw * 0.75f), ac);
        }
        if (clicked && !off) {
            float mx = igGetIO()->MousePos.x, hit = g_touch ? gap * 2.2f : gap;  // (a fingertip: wider)
            if (mx >= lx - hit && mx <= lx + aw + hit) step = -1;
            else if (mx >= rx - hit && mx <= rx + aw + hit) step = 1;
        }
        right = rx - gap;
    }
    char shown[TXT_CAP];  // (a value wider than most of the row is cut short too)
    snprintf(shown, sizeof shown, "%s", value);
    if (arrows) title_case(shown);  // (a choice's values: Match Display, Original 16:9)
    if (shown[0] && text_w(shown) > w * 0.6f) ellipsize(shown, w * 0.6f);
    if (shown[0]) {
        float vw = text_w(shown), vx = g_flat && !arrows ? p.x + vcol : right - vw;
        ImDrawList_AddText_Vec2(dl, IM_VEC2(vx, ty), off ? IM_COL32(110, 110, 102, 255) : nes ? (lit || changed ? NES_WHITE : NES_GREY)
                                    : changed ? IM_COL32(255, 255, 255, 255) : IM_COL32(205, 205, 195, 255), shown, NULL);
    }
    igPopID();
    if (g_reset_row) step = 0;  // (the wrappers reset it instead of stepping)
    return step;
}

bool opt_toggle(const char *label, bool *v, bool def, const char *desc) {
    g_row_has_def = true;
    int st = row(label, *v ? "On" : "Off", *v != def, desc, true, false);
    if ((g_reset_row || g_reset_page) && *v != def) return *v = def, true;
    if (!st) return false;  // (any step flips it)
    *v = !*v;
    return true;
}

bool opt_choice(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap) {
    int cur = *v >= 0 && *v < n ? *v : 0;
    g_row_has_def = true;
    int step = row(label, names[cur], cur != def, desc, true, true);
    if ((g_reset_row || g_reset_page) && cur != def) return *v = def, true;
    if (!step) return false;
    int nv = cur + (step == 2 ? 1 : step);
    nv = wrap ? (nv + n) % n : nv < 0 ? 0 : nv >= n ? n - 1 : nv;
    if (nv == cur) return false;
    *v = nv;
    return true;
}

bool opt_slider(const char *label, float *v, float lo, float hi, float step, float def, const char *fmt, const char *desc) {
    char buf[32];
    snprintf(buf, sizeof buf, fmt, *v);
    g_row_has_def = true;
    int s = row(label, buf, fabsf(*v - def) > step * 0.01f, desc, true, true);
    if ((g_reset_row || g_reset_page) && fabsf(*v - def) > step * 0.01f) return *v = def, true;
    if (!s) return false;
    float eps = step * 0.01f, n = roundf(*v / step) * step + (s == 2 ? step : s * step);
    if (s == 2 && n > hi + eps) n = lo;  // (A / click past the top wraps to the bottom; left / right stop at the ends)
    *v = n < lo ? lo : n > hi ? hi : n;
    return true;
}

bool opt_int(const char *label, int *v, int lo, int hi, int step, int def, const char *fmt, const char *desc,
             const char *lo_name) {
    char buf[48];
    if (lo_name && *v == lo) snprintf(buf, sizeof buf, "%s", lo_name);
    else snprintf(buf, sizeof buf, fmt, *v);
    g_row_has_def = true;
    int s = row(label, buf, *v != def, desc, true, true);
    if ((g_reset_row || g_reset_page) && *v != def) return *v = def, true;
    if (!s) return false;
    int d = s == 2 ? step : s * step * (g_held && hi - lo > 40 * step ? 10 : 1);
    int n = *v + d;
    if (s == 2 && n > hi) n = lo;  // (A / click past the top wraps; left / right stop at the ends)
    n = n < lo ? lo : n > hi ? hi : n;
    if (n == *v) return false;
    *v = n;
    return true;
}

bool opt_button(const char *label, const char *desc) { return row(label, "", false, desc, true, false) == 2; }

void opt_desc_image(uint64_t tex, int w, int h) {
    if (igIsItemHovered(0)) g_img_hover = (DescImage){tex, w, h};
    if (igIsItemFocused() && igGetCurrentContext()->NavCursorVisible) g_img_focus = (DescImage){tex, w, h};
}

bool opt_radio(const char *label, bool selected, const char *desc) {
    bool off = g_next_disabled;
    bool chosen = row(label, "", false, desc, true, false) == 2;
    ImVec2 a, b;
    igGetItemRectMin(&a);
    igGetItemRectMax(&b);
    float r = igGetFontSize() * 0.32f, cx = b.x - 16 * g_scale - r, cy = (a.y + b.y) * 0.5f;
    ImDrawList *dl = igGetWindowDrawList();
    ImU32 col = off ? IM_COL32(150, 150, 140, 255) : selected ? kGold : IM_COL32(205, 205, 195, 255);
    if (menu_row_notch() > 0) {  // NES.css's radio: a stepped box, with a pixel star in it when chosen
        float u = 2 * g_scale, sz = r * 2.6f;
        nes_box(dl, IM_VEC2(cx - sz * 0.5f, cy - sz * 0.5f), IM_VEC2(cx + sz * 0.5f, cy + sz * 0.5f), NES_RGB(21, 24, 27),
                off ? NES_RGB(110, 110, 102) : NES_WHITE, u, 0);
        if (selected) {  // a filled square inside
            float m = floorf(sz * 0.28f);
            ImDrawList_AddRectFilled(dl, IM_VEC2(floorf(cx - sz * 0.5f + m), floorf(cy - sz * 0.5f + m)),
                                     IM_VEC2(floorf(cx + sz * 0.5f - m), floorf(cy + sz * 0.5f - m)),
                                     off ? NES_RGB(110, 110, 102) : NES_GREEN, 0, 0);
        }
        return chosen && !selected;
    }
    ImDrawList_AddCircle(dl, IM_VEC2(cx, cy), r, col, 0, 2.0f);
    if (selected) ImDrawList_AddCircleFilled(dl, IM_VEC2(cx, cy), r * 0.55f, col, 0);
    return chosen && !selected;
}

bool opt_action(const char *label, const char *value, const char *desc, bool upper) {
    return row(label, value ? value : "", false, desc, upper, false) == 2;
}

static ImGuiID g_text_editing;  // the text row turned into a field (one at a time)
bool opt_text_editing(const char *label) { return g_text_editing && g_text_editing == igGetID_Str(label); }

bool opt_text(const char *label, char *buf, size_t cap, const char *hint, const char *desc) {
    static int focus;        // frames left to give it the keyboard focus
    ImGuiID id = igGetID_Str(label);
    if (g_text_editing != id) {
        if (row(label, buf[0] ? buf : (hint ? hint : ""), false, desc, true, false) == 2) g_text_editing = id, focus = 2;
        return false;
    }
    igPushID_Str(label);
    // a Paste button beside the field: the clipboard's text in place of the field's (touch screens have no Cmd+V;
    // owner, 2026-10-30: an OpenCourse World token couldn't be pasted)
    const char *paste_label = "Paste";
    float pw = text_w(paste_label) + igGetStyle()->FramePadding.x * 2;
    igSetNextItemWidth(-(pw + igGetStyle()->ItemSpacing.x));
    if (focus > 0) igSetKeyboardFocusHere(0), focus--;
    bool done = ui_input_text("##text", buf, cap, ImGuiInputTextFlags_EnterReturnsTrue, hint, label);
    bool active = igIsItemActive();
    if (igIsItemFocused()) set_desc(g_desc_focus, desc ? desc : "");
    igSameLine(0, -1);
    bool paste = igButton(paste_label, IM_VEC2(0, 0));
    bool paste_hot = igIsItemHovered(0) || igIsItemActive();  // (pressing it takes the focus off the field)
    if (paste) {
        const char *t = menu_clipboard();
        while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r') t++;
        size_t n = strlen(t);
        while (n && (t[n - 1] == ' ' || t[n - 1] == '\t' || t[n - 1] == '\n' || t[n - 1] == '\r')) n--;
        if (n >= cap) n = cap - 1;
        memcpy(buf, t, n);
        buf[n] = 0;
        done = true;
    }
    if (done || ui_input_text_cancelled() || (focus == 0 && !active && !paste_hot && !osk_is_open())) g_text_editing = 0;
    igPopID();
    return done;
}

void opt_end(void) {
    g_reset_page = false;  // (this frame's rows have gone back to their defaults)
    if (g_page_rows > 0) {  // the page's last row: everything back to default, asked once
        igDummy(IM_VEC2(0, 10 * g_scale));
        if (!g_page_changed) g_next_disabled = true, g_reset_ask = false;
        bool ask = g_reset_ask && g_page_changed;
        if (opt_button(ask ? "Click Again to Confirm" : "Reset Page to Defaults",
                       "Puts every setting on this page back to its default. Click twice to confirm. Right-click a changed "
                       "setting (X on a controller) to reset just that one.")) {
            if (ask) g_reset_page = true, g_reset_ask = false;  // (the rows apply it on the next frame)
            else g_reset_ask = true, snprintf(g_reset_ask_page, sizeof g_reset_ask_page, "%s", g_cur_page);
        }
    }
    igPopStyleVar(1);
    const char *scroll = getenv("LARECOMP_OPT_SCROLL");
    if (scroll) igSetScrollY_Float(igGetScrollMaxY() * (float)atof(scroll));  // (test)
    igEndChild();
    if (g_flat) {  // (one pane: the descriptions were tooltips)
        for (int k = 0; k < 24; k++)
            if (g_flat_cache[k].id[0] && !strcmp(g_flat_cache[k].id, g_cur_page)) {
                g_flat_cache[k].labw = g_labw_cur, g_flat_cache[k].valw = g_valw_cur;
                break;
            }
        igPopID();
        return;
    }
    // the description: the hovered row (mouse) wins, else the selected one (controller), else the last shown
    if (g_desc_hover[0]) set_desc(g_desc_shown, g_desc_hover), g_img_shown = g_img_hover;
    else if (g_desc_focus[0]) set_desc(g_desc_shown, g_desc_focus), g_img_shown = g_img_focus;
    else if (!g_desc_shown[0]) set_desc(g_desc_shown, g_desc_first), g_img_shown = (DescImage){0, 0, 0};
    igSameLine(0, 0);
    ImVec2 p, avail;
    igGetCursorScreenPos(&p);
    igGetContentRegionAvail(&avail);
    float gap = 24 * g_scale;
    const bool nes = menu_row_notch() > 0;
    if (nes) {  // the description in its own NES.css container (the child's own background is left clear)
        nes_box(igGetWindowDrawList(), IM_VEC2(p.x + gap, p.y), IM_VEC2(p.x + avail.x, p.y + avail.y),
                igGetColorU32_Col(ImGuiCol_ChildBg, 1.0f), NES_GREY, 3 * g_scale, 0);
        igPushStyleColor_Vec4(ImGuiCol_ChildBg, IM_VEC4(0, 0, 0, 0));
    } else {
        ImDrawList_AddLine(igGetWindowDrawList(), IM_VEC2(p.x + gap * 0.5f, p.y), IM_VEC2(p.x + gap * 0.5f, p.y + avail.y),
                           menu_accent(110), 1.0f);
    }
    igSetCursorScreenPos(IM_VEC2(p.x + gap, p.y));
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, nes ? IM_VEC2(26 * g_scale, 20 * g_scale) : IM_VEC2(14 * g_scale, 6 * g_scale));
    igBeginChild_Str("##desc", IM_VEC2(0, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
    igPopStyleVar(1);
    if (nes) igPopStyleColor(1);
    igSetWindowFontScale(nes ? 1.0f : 1.3f);  // (the description reads at a glance)
    if (g_img_shown.w != 0) {  // a picture, or its place while it loads (w < 0: it won't come): a fixed 16:9 box
        ImVec2 room, p0;
        igGetContentRegionAvail(&room);
        igGetCursorScreenPos(&p0);
        float bw = fminf(room.x, room.y * 0.5f * 16 / 9), bh = bw * 9 / 16;
        ImVec2 p1 = IM_VEC2(p0.x + bw, p0.y + bh);
        ImDrawList *dl = igGetWindowDrawList();
        ImDrawList_AddRectFilled(dl, p0, p1, IM_COL32(0, 0, 0, 110), 0, 0);
        if (g_img_shown.tex && g_img_shown.w > 0) {  // (fitted inside the box)
            float s = fminf(bw / g_img_shown.w, bh / g_img_shown.h), iw = g_img_shown.w * s, ih = g_img_shown.h * s;
            ImVec2 q = IM_VEC2(p0.x + (bw - iw) / 2, p0.y + (bh - ih) / 2);
            ImDrawList_AddImage(dl, (ImTextureID)g_img_shown.tex, q, IM_VEC2(q.x + iw, q.y + ih), IM_VEC2(0, 0), IM_VEC2(1, 1),
                                IM_COL32(255, 255, 255, 255));
        } else {
            const char *t = g_img_shown.w < 0 ? "No screenshot" : "Loading screenshot...";
            ImVec2 ts;
            igCalcTextSize(&ts, t, NULL, false, -1.0f);
            ImDrawList_AddText_Vec2(dl, IM_VEC2(p0.x + (bw - ts.x) / 2, p0.y + (bh - ts.y) / 2), menu_muted(), t, NULL);
        }
        igDummy(IM_VEC2(bw, bh));
        igSpacing();
    }
    igPushTextWrapPos(0);
    igTextUnformatted(g_desc_shown, NULL);
    igPopTextWrapPos();
    igSetWindowFontScale(1.0f);
    igEndChild();
    igPopID();
}
