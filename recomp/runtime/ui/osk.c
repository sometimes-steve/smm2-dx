// On-screen keyboard (see osk.h). A controller has no keys to type with, and ImGui only starts text entry on a
// gamepad's Y (A on a field did nothing): A on any of our text fields opens this keyboard instead.
//
// One keyboard at a time (only one ImGui UI runs: start screen or in-game overlay, one thread each). It is a modal
// popup at the root of the UI, so the menu behind sees "a popup is open" and B / Esc / L / R don't act on it.
#include "ui/ig.h"

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osk.h"
#include "../settings.h"

uint32_t menu_accent(int a);  // runtime/ui/menu.c: the theme's accent / caption colours
uint32_t menu_muted(void);
float menu_row_notch(void);   // > 0: the NES theme (square keycaps)
int input_pad_family(void);   // runtime/hle/input.c: 0 Nintendo, 1 Xbox, 2 PlayStation

static struct {
    int open;                        // a field's keyboard is up
    ImGuiID field;                   // the field it types for
    char *buf;                       // ... and its buffer (static in every caller: written on OK)
    size_t size;
    ImGuiInputTextFlags flags;
    char text[128], title[96];
    bool appearing, nav_kbd_off;
    ImGuiID done_field;              // answered: reported by that field's next ui_input_text
    int done, done_frame;            // 1 OK, -1 cancelled; when (a field gone by then never hears of it)
} s;
static atomic_bool s_cancel_req;
static bool s_cancelled_now;

bool osk_is_open(void) { return s.open != 0; }
void osk_cancel(void) { atomic_store(&s_cancel_req, true); }

bool ui_text_busy(void) {
    ImGuiContext *g = igGetCurrentContext();
    return s.open || (g && (g->IO.WantTextInput || g->WantTextInputNextFrame == 1));
}

bool ui_input_text_cancelled(void) { return s_cancelled_now; }

// (test) LARECOMP_FIELD_FOCUS=<label>:<n>[:edit]: the n-th time field <label> is drawn it gets the nav cursor (a
// controller's A - LARECOMP_HOST_INPUT sdlpad:east - then opens the keyboard); :edit starts typing in it instead.
static int test_focus(const char *label) {
    static const char *spec;
    static bool spec_read;
    static int count;
    if (!spec_read) spec = getenv("LARECOMP_FIELD_FOCUS"), spec_read = true;
    if (!spec) return 0;
    const char *c = strchr(spec, ':');
    if (!c || strncmp(spec, label, (size_t)(c - spec)) || label[c - spec]) return 0;
    if (++count != atoi(c + 1)) return 0;
    return strstr(c + 1, ":edit") ? 2 : 1;
}

bool ui_input_text(const char *label, char *buf, size_t size, ImGuiInputTextFlags flags, const char *hint, const char *title) {
    ImGuiContext *g = igGetCurrentContext();
    int test = test_focus(label);
    if (test == 2) igSetKeyboardFocusHere(0);
    bool r = hint ? igInputTextWithHint(label, hint, buf, size, flags, NULL, NULL) : igInputText(label, buf, size, flags, NULL, NULL);
    ImGuiID id = igGetItemID();
    if (test == 1) { igFocusItem(); igSetNavCursorVisible(true); }
    // (and in view for the screenshots for a few frames: the focus' own scroll misses on a NavFlattened page)
    static ImGuiID in_view_id;
    static int in_view;
    if (test) in_view_id = id, in_view = 10;
    if (in_view_id == id && in_view > 0 && in_view--) igSetScrollHereY(0.5f);
    if (test) fprintf(stderr, "[osk] test: %s field %s\n", test == 2 ? "typing in" : "cursor on", label);
    static char *last;  // (test runs: what the field holds, as it changes)
    const char *tf = getenv("LARECOMP_FIELD_FOCUS");
    if (tf && !strncmp(tf, label, strlen(label)) && tf[strlen(label)] == ':' && (!last || strcmp(last, buf))) {
        last = (char *)realloc(last, strlen(buf) + 1);
        if (last) strcpy(last, buf);
        fprintf(stderr, "[osk] test: %s = \"%s\"%s\n", label, buf, g->ActiveId == id ? " (typing)" : "");
    }
    s_cancelled_now = false;
    if (s.done && s.done_field == id && igGetFrameCount() - s.done_frame <= 1) {
        if (s.done > 0) r = true;
        else s_cancelled_now = true;
        s.done = 0, s.done_field = 0;
    }
    // activated from the controller: A (ImGui's "activate", which leaves a text field alone), Y (its "input"), or a
    // focus request (SetKeyboardFocusHere) while the controller drives the menu. A mouse click types directly.
    bool by_nav = g->NavActivateId == id || (igIsItemActivated() && g->ActiveIdSource != ImGuiInputSource_Mouse);
    if (by_nav && g->NavInputSource == ImGuiInputSource_Gamepad && !s.open && !(flags & ImGuiInputTextFlags_ReadOnly)) {
        if (g->ActiveId == id) igClearActiveID();
        s.open = 1, s.appearing = true, s.done = 0;
        s.field = id, s.buf = buf, s.size = size, s.flags = flags;
        snprintf(s.text, sizeof s.text, "%s", buf);
        snprintf(s.title, sizeof s.title, "%s", title ? title : hint ? hint : "Enter text");
        r = false;
    }
    return r;
}

// ---- keycap hints (osk.h)
void ui_hint_names(bool pad, const char **confirm, const char **back, const char **start) {
    int fam = g_settings.prompts ? (g_settings.prompts - 1) % 3 : input_pad_family() % 3;  // 0 Nintendo, 1 Xbox, 2 PlayStation
    *confirm = pad ? (fam == 2 ? "Cross" : "A") : "Enter";
    *back = pad ? (fam == 2 ? "Circle" : "B") : "Esc";
    *start = pad ? (fam == 0 ? "+" : fam == 1 ? "Menu" : "Options") : "Enter";
}

void ui_hint_row(const char *const *keys, const char *const *what, int n, const char *right) {
    ImVec2 p, avail;
    igGetCursorScreenPos(&p);
    igGetContentRegionAvail(&avail);
    ImDrawList *dl = igGetWindowDrawList();
    const bool square = menu_row_notch() > 0;
    float fs = igGetFontSize(), ch = fs * 1.45f, cy = p.y + ch * 0.5f + fs * 0.15f, x = p.x + fs * 0.1f;
    for (int i = 0; i < n; i++) {
        ImVec2 kw, ww;
        igCalcTextSize(&kw, keys[i], NULL, false, -1.0f);
        igCalcTextSize(&ww, what[i], NULL, false, -1.0f);
        float cw = kw.x + fs * 0.9f;
        ImVec2 a = IM_VEC2(x, cy - ch * 0.5f), b = IM_VEC2(x + cw, cy + ch * 0.5f);
        ImDrawList_AddRectFilled(dl, a, b, IM_COL32(255, 255, 255, 24), square ? 0 : ch * 0.3f, 0);
        ImDrawList_AddRect(dl, a, b, menu_accent(200), square ? 0 : ch * 0.3f, 0, square ? 2.0f : 1.5f);
        ImDrawList_AddText_Vec2(dl, IM_VEC2(x + fs * 0.45f, cy - kw.y * 0.5f), menu_accent(255), keys[i], NULL);
        x += cw + fs * 0.4f;
        ImDrawList_AddText_Vec2(dl, IM_VEC2(x, cy - ww.y * 0.5f), menu_muted(), what[i], NULL);
        x += ww.x + fs * 1.4f;
    }
    if (right && right[0]) {
        ImVec2 rw;
        igCalcTextSize(&rw, right, NULL, false, -1.0f);
        ImDrawList_AddText_Vec2(dl, IM_VEC2(p.x + avail.x - rw.x - fs * 0.1f, cy - rw.y * 0.5f), menu_muted(), right, NULL);
    }
    igDummy(IM_VEC2(1, ch + fs * 0.4f));
}

// ---- the Course ID / Maker ID rules (osk.h)
static bool g_id_mode;
void osk_set_id_mode(bool on) { g_id_mode = on; }
bool osk_id_char(int c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z' && strchr("BCDFGHJKLMNPQRSTVWXY", c));
}
void osk_id_compact(const char *in, char out[10]) {
    int n = 0;
    for (; *in && n < 9; in++)
        if (osk_id_char((unsigned char)*in)) out[n++] = (char)toupper((unsigned char)*in);
    out[n] = 0;
}
void osk_id_dashed(const char *raw, char out[16]) {
    char c[10];
    osk_id_compact(raw, c);
    int o = 0;
    for (int i = 0; c[i]; i++) {
        if (i == 3 || i == 6) out[o++] = '-';  // (only when a character follows: the last group's dash is not left hanging)
        out[o++] = c[i];
    }
    out[o] = 0;
}

// the text being edited by osk_keys (what the lambdas of the C++ version captured)
typedef struct {
    char *text;
    size_t cap;
    int len, max_chars;
} OskText;

// (len: bytes; the limit and the count are characters: a name being edited may hold UTF-8 the keys don't type)
static int osk_chars(const OskText *o) {
    int n = 0;
    for (int i = 0; i < o->len; i++) n += ((unsigned char)o->text[i] & 0xc0) != 0x80;
    return n;
}
static void osk_add(OskText *o, char ch) {
    if (osk_chars(o) < o->max_chars && o->len + 1 < (int)o->cap) o->text[o->len++] = ch, o->text[o->len] = 0;
}
static void osk_del(OskText *o) {  // (a whole character)
    while (o->len > 0 && ((unsigned char)o->text[o->len - 1] & 0xc0) == 0x80) o->len--;
    if (o->len > 0) o->len--;
    o->text[o->len] = 0;
}

int osk_keys(char *text, size_t cap, int max_chars, bool appearing, float scale, ImFont *big, bool upper_first) {
    static bool upper = true;
    if (appearing) upper = upper_first;
    ImGuiIO *io = igGetIO();
    // B deletes: owning it keeps ImGui's nav cancel (which drops the cursor off the keys) from seeing it
    ImGuiID own = igGetID_Str("##oskkeys");
    igSetKeyOwner(ImGuiKey_GamepadFaceRight, own, 0);
    if (max_chars <= 0 || max_chars > (int)cap - 1) max_chars = (int)cap - 1;
    int r = 0;
    OskText o = {text, cap, (int)strlen(text), max_chars};

    // typed on a keyboard while it's up: straight into the text
    for (int i = 0; i < io->InputQueueCharacters.Size; i++) {
        ImWchar c = io->InputQueueCharacters.Data[i];
        if (g_id_mode) { if (osk_id_char(c)) osk_add(&o, (char)toupper(c)); }  // (capital, and only what an ID can hold)
        else if (c >= 0x20 && c < 0x7f) osk_add(&o, (char)c);
    }
    io->InputQueueCharacters.Size = 0;
    if (igIsKeyPressed_Bool(ImGuiKey_Backspace, true)) osk_del(&o);
    if (igIsKeyPressed_Bool(ImGuiKey_Enter, false) || igIsKeyPressed_Bool(ImGuiKey_KeypadEnter, false)) r = 1;
    if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) r = -1;
    if (igIsKeyPressed_Bool(ImGuiKey_GamepadStart, false)) r = 1;
    if (igIsKeyPressed_InputFlags(ImGuiKey_GamepadFaceRight, ImGuiInputFlags_Repeat, own)) osk_del(&o);

    igPushFont(big);
    if (g_id_mode) {
        char shown[16];
        osk_id_dashed(text, shown);
        igText("%s_", shown);
    } else {
        igText("%s_", text);
    }
    igPopFont();
    static const char *rows_u[] = {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-.'!", "0123456789"};
    static const char *rows_l[] = {"abcdefghij", "klmnopqrst", "uvwxyz-.'!", "0123456789"};
    const char **rows = upper ? rows_u : rows_l;
    float k = 70 * scale;
    for (int row = 0; row < 4; row++) {
        for (int i = 0; rows[row][i]; i++) {
            if (i) igSameLine(0, -1);
            char lbl[8] = {rows[row][i], 0};
            if (appearing && row == 0 && i == 0) {  // (cursor shown: else the first A only reveals it)
                igSetKeyboardFocusHere(0);
                igSetNavCursorVisible(true);
            }
            bool off = g_id_mode && !osk_id_char((unsigned char)rows[row][i]);  // (an ID has no vowels, no Z, no punctuation)
            igBeginDisabled(off);
            if (igButton(lbl, IM_VEC2(k, k))) osk_add(&o, rows[row][i]);
            igEndDisabled();
        }
    }
    if (g_id_mode) upper = true;  // (capitals only)
    igBeginDisabled(g_id_mode);
    if (igButton(upper ? "abc" : "ABC", IM_VEC2(k * 2, k))) upper = !upper;
    igSameLine(0, -1);
    if (igButton("Space", IM_VEC2(k * 3, k)) && o.len > 0) osk_add(&o, ' ');
    igEndDisabled();
    igSameLine(0, -1);
    if (igButton("Delete", IM_VEC2(k * 2, k))) osk_del(&o);
    igSameLine(0, -1);
    if (igButton("Cancel", IM_VEC2(k * 2, k))) r = -1;
    igSameLine(0, -1);
    if (igButton("OK", IM_VEC2(k * 2, k))) r = 1;
    {
        const char *ok, *back, *start;
        ui_hint_names(true, &ok, &back, &start);
        const char *keys[] = {ok, back, start}, *what[] = {"Type", "Delete", "OK"};
        char count[32];
        snprintf(count, sizeof count, "%d / %d", osk_chars(&o), max_chars);
        ui_hint_row(keys, what, 3, count);
    }
    return r;
}

// close it (and give the keyboard back to ImGui's navigation)
static void finish(int result) {
    if (result > 0 && s.buf && s.size) {
        if (s.flags & ImGuiInputTextFlags_CharsUppercase)
            for (char *p = s.text; *p; p++) *p = (char)toupper((unsigned char)*p);
        snprintf(s.buf, s.size, "%s", s.text);
    }
    s.done = result, s.done_field = s.field, s.done_frame = igGetFrameCount();
    s.open = 0, s.buf = NULL;
    if (s.nav_kbd_off) igGetIO()->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    s.nav_kbd_off = false;
}

void osk_draw(ImFont *big, float scale) {
    bool cancel = atomic_exchange(&s_cancel_req, false);
    if (!s.open) return;
    ImGuiIO *io = igGetIO();
    ImGuiContext *g = igGetCurrentContext();
    if (!igIsPopupOpen_Str("##osk", 0)) {
        igOpenPopup_Str("##osk", 0);
        s.appearing = true;
        // keys typed while it's up are text: Space / Enter / arrows must not also press the focused key
        if (io->ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) { io->ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard; s.nav_kbd_off = true; }
    }
    g->WantTextInputNextFrame = 1;  // (typing stays on: the backends keep text input enabled; ui_text_busy)
    float w = fminf(io->DisplaySize.x * 0.9f, 1100 * scale);
    igSetNextWindowPos(IM_VEC2(io->DisplaySize.x * 0.5f, io->DisplaySize.y * 0.5f), ImGuiCond_Always, IM_VEC2(0.5f, 0.5f));
    igSetNextWindowSize(IM_VEC2(w, 0), ImGuiCond_Always);
    if (!igBeginPopupModal("##osk", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        finish(-1);
        return;
    }
    igPushFont(big);
    igTextColored(IM_VEC4(0.93f, 0.76f, 0.33f, 1), "%s", s.title);
    igPopFont();
    int max = (int)(s.size < sizeof s.text ? s.size : sizeof s.text) - 1;
    int r = cancel ? -1 : osk_keys(s.text, sizeof s.text, max, s.appearing, scale, big, true);
    s.appearing = false;
    if (r) {
        igCloseCurrentPopup();
        finish(r);
    }
    igEndPopup();
}
