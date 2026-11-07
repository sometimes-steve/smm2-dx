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
        if (c >= 0x20 && c < 0x7f) osk_add(&o, (char)c);
    }
    io->InputQueueCharacters.Size = 0;
    if (igIsKeyPressed_Bool(ImGuiKey_Backspace, true)) osk_del(&o);
    if (igIsKeyPressed_Bool(ImGuiKey_Enter, false) || igIsKeyPressed_Bool(ImGuiKey_KeypadEnter, false)) r = 1;
    if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) r = -1;
    if (igIsKeyPressed_Bool(ImGuiKey_GamepadStart, false)) r = 1;
    if (igIsKeyPressed_InputFlags(ImGuiKey_GamepadFaceRight, ImGuiInputFlags_Repeat, own)) osk_del(&o);

    igPushFont(big);
    igText("%s_", text);
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
            if (igButton(lbl, IM_VEC2(k, k))) osk_add(&o, rows[row][i]);
        }
    }
    if (igButton(upper ? "abc" : "ABC", IM_VEC2(k * 2, k))) upper = !upper;
    igSameLine(0, -1);
    if (igButton("Space", IM_VEC2(k * 3, k)) && o.len > 0) osk_add(&o, ' ');
    igSameLine(0, -1);
    if (igButton("Delete", IM_VEC2(k * 2, k))) osk_del(&o);
    igSameLine(0, -1);
    if (igButton("Cancel", IM_VEC2(k * 2, k))) r = -1;
    igSameLine(0, -1);
    if (igButton("OK", IM_VEC2(k * 2, k))) r = 1;
    igTextDisabled("A  type      B  delete      Start / +  OK      (%d / %d)", osk_chars(&o), max_chars);
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
