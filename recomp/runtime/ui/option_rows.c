// Two-pane settings pages (see option_rows.h): rows on the left, the selected / hovered row's description on the right.
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

#define DESC_CAP 2400
#define TXT_CAP 640

static float g_scale = 1;
static char g_desc_hover[DESC_CAP], g_desc_focus[DESC_CAP], g_desc_shown[DESC_CAP];  // this frame's hovered / selected row, and the last shown
static char g_desc_first[DESC_CAP];  // the page's first row (shown until a row is selected or hovered)

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

static void set_desc(char *dst, const char *src) { snprintf(dst, DESC_CAP, "%s", src); }

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

void opt_begin(const char *id, float scale) {
    static char page[128];  // another page: its own first row's description, not the last page's
    if (strcmp(page, id)) snprintf(page, sizeof page, "%s", id), g_desc_shown[0] = 0;
    g_scale = scale;
    g_take_first = g_focus_req;
    g_focus_req = false;
    if (g_take_first) igNavMoveRequestCancel();  // (this frame's Down goes to the first row instead)
    g_indent = 0;
    g_desc_hover[0] = 0, g_desc_focus[0] = 0;
    ImVec2 avail;
    igGetContentRegionAvail(&avail);
    igPushID_Str(id);
    igBeginChild_Str("##rows", IM_VEC2(avail.x * 0.48f, avail.y), ImGuiChildFlags_NavFlattened, 0);  // (was the obsolete ImGuiWindowFlags_NavFlattened)
    igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, IM_VEC2(igGetStyle()->ItemSpacing.x, (g_touch ? 8 : 4) * scale));  // (rows close)
    g_desc_first[0] = 0;
}

void opt_section(const char *title) {
    char t[TXT_CAP];
    snprintf(t, sizeof t, "%s", title);
    for (char *c = t; *c; c++) *c = (char)toupper((unsigned char)*c);
    if (igGetCursorPosY() > igGetStyle()->WindowPadding.y + 1) igDummy(IM_VEC2(0, 10 * g_scale));
    igPushStyleColor_U32(ImGuiCol_Text, menu_muted());
    igTextUnformatted(t, NULL);
    igPopStyleColor(1);
}

// One row: draws it, handles A / click and left / right while selected. Returns 2 (A / click), -1 / +1 (left / right) or 0.
// arrows: ◀ value ▶ (a choice of several, or a stepper): left / right step it, and the arrows click too
// (callers pass upper = true, arrows = false unless they need otherwise)
static int row(const char *label, const char *value, bool changed, const char *desc, bool upper, bool arrows) {
    igPushID_Str(label);
    if (g_indent) igSetCursorPosX(igGetCursorPosX() + g_indent * 28 * g_scale);  // (an option of the row above)
    ImVec2 p;
    igGetCursorScreenPos(&p);
    const bool nes = menu_row_notch() > 0;  // (the NES theme: pixel-art buttons, pointer and arrows)
    ImVec2 availv;
    igGetContentRegionAvail(&availv);
    float w = availv.x, h = igGetFontSize() * (g_touch || nes ? 2.5f : 1.9f);
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

    ImDrawList *dl = igGetWindowDrawList();
    float r = menu_row_round() * g_scale, pad = 16 * g_scale, fs = igGetFontSize();
    ImU32 fill = hov || foc ? IM_COL32(255, 255, 255, 22) : IM_COL32(0, 0, 0, 40);
    ImU32 edge = hov || foc ? (off ? menu_accent(120) : kGold) : IM_COL32(255, 255, 255, off ? 18 : 38);
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
    if (upper)
        for (char *c = t; *c; c++) *c = (char)toupper((unsigned char)*c);
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
        float room = w - 2 * pad - vspace - lead;
        if (room > fs && text_w(t) > room) ellipsize(t, room);
    }
    ImDrawList_AddText_Vec2(dl, IM_VEC2(p.x + pad + lead, ty), label_col, t, NULL);
    float right = p.x + w - pad;  // (the value's right edge)
    if (arrows && value[0]) {  // ◀ value ▶, drawn (the fonts have no arrow glyphs); a click on one steps
        float aw = nes ? 5 * ap : fs * 0.42f, gap = fs * 0.55f, cy = ty + fs * 0.52f;
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
    if (shown[0] && text_w(shown) > w * 0.6f) ellipsize(shown, w * 0.6f);
    if (shown[0]) {
        float vw = text_w(shown), vx = right - vw;
        ImDrawList_AddText_Vec2(dl, IM_VEC2(vx, ty), off ? IM_COL32(110, 110, 102, 255) : nes ? (lit || changed ? NES_WHITE : NES_GREY)
                                    : changed ? IM_COL32(255, 255, 255, 255) : IM_COL32(205, 205, 195, 255), shown, NULL);
    }
    igPopID();
    return step;
}

bool opt_toggle(const char *label, bool *v, bool def, const char *desc) {
    if (!row(label, *v ? "On" : "Off", *v != def, desc, true, false)) return false;  // (any step flips it)
    *v = !*v;
    return true;
}

bool opt_choice(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap) {
    int cur = *v >= 0 && *v < n ? *v : 0;
    int step = row(label, names[cur], cur != def, desc, true, true);
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
    int s = row(label, buf, fabsf(*v - def) > step * 0.01f, desc, true, true);
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
    int s = row(label, buf, *v != def, desc, true, true);
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
    igPopStyleVar(1);
    const char *scroll = getenv("LARECOMP_OPT_SCROLL");
    if (scroll) igSetScrollY_Float(igGetScrollMaxY() * (float)atof(scroll));  // (test)
    igEndChild();
    // the description: the hovered row (mouse) wins, else the selected one (controller), else the last shown
    if (g_desc_hover[0]) set_desc(g_desc_shown, g_desc_hover);
    else if (g_desc_focus[0]) set_desc(g_desc_shown, g_desc_focus);
    else if (!g_desc_shown[0]) set_desc(g_desc_shown, g_desc_first);
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
    igPushTextWrapPos(0);
    igTextUnformatted(g_desc_shown, NULL);
    igPopTextWrapPos();
    igSetWindowFontScale(1.0f);
    igEndChild();
    igPopID();
}
