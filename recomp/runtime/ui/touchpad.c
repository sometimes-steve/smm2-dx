// On-screen controller for touch devices: a left stick, A/B/X/Y, L/R/ZL/ZR, Plus/Minus and a
// button that opens the overlay menu, drawn over the game with Dear ImGui (runtime/ui/menu.c calls touchpad_draw).
// A finger that lands on one of them belongs to the controller until it lifts (it may slide between buttons); every
// other finger goes on to the console's touch screen (runtime/hle/touch.c), which the Course Maker is made for. The
// small button in the top left corner hides / shows the controller (it stays hidden while a gamepad is connected).
// Plain C (no Objective-C): the Vulkan builds compile it too; it shows there once a touch screen is touched.
#include "ui/ig.h"

#include <SDL3/SDL.h>
#include <float.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

void menu_toggle(void);                // runtime/ui/menu.c
bool menu_input_captured(void);        // runtime/ui/menu.c: menu or name entry up (it takes the touches)
bool input_gamepad_connected(void);    // runtime/hle/input.c

enum { B_A = 0, B_B, B_X, B_Y, B_L = 6, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS };  // nn::hid button bits (input.c BTN_*)
enum { C_STICK = 1, C_BUTTON, C_MENU, C_TOGGLE };

typedef struct Control { int kind, bit; float x, y, r; const char *label; } Control;  // x, y, r in units of the screen's height
// (x < 0: from the right edge). Landscape, laid out for 4:3 to 19.5:9.
static const Control CONTROLS[] = {
    {C_STICK, -1, 0.20f, 0.74f, 0.13f, ""},
    {C_BUTTON, B_A, -0.10f, 0.72f, 0.055f, "A"},
    {C_BUTTON, B_B, -0.20f, 0.84f, 0.055f, "B"},
    {C_BUTTON, B_X, -0.20f, 0.60f, 0.055f, "X"},
    {C_BUTTON, B_Y, -0.30f, 0.72f, 0.055f, "Y"},
    {C_BUTTON, B_L, 0.08f, 0.40f, 0.05f, "L"},
    {C_BUTTON, B_ZL, 0.08f, 0.26f, 0.05f, "ZL"},
    {C_BUTTON, B_R, -0.08f, 0.40f, 0.05f, "R"},
    {C_BUTTON, B_ZR, -0.08f, 0.26f, 0.05f, "ZR"},
    {C_BUTTON, B_MINUS, 0.30f, 0.06f, 0.035f, "-"},
    {C_BUTTON, B_PLUS, -0.30f, 0.06f, 0.035f, "+"},
    {C_MENU, -1, -0.06f, 0.06f, 0.035f, "="},
    {C_TOGGLE, -1, 0.06f, 0.06f, 0.035f, ""},
};
#define NCONTROLS ((int)(sizeof CONTROLS / sizeof *CONTROLS))

// The window's safe area: left and right insets in pixels, kept by
// the UI thread (safe-area events, touches) for both threads. The controls keep clear of them.
static _Atomic float g_inset_l, g_inset_r;
static void update_insets(SDL_Window *win) {
    SDL_Rect safe;
    int ww = 0, wh = 0, pw = 0, ph = 0;
    if (!win || !SDL_GetWindowSafeArea(win, &safe) || !SDL_GetWindowSize(win, &ww, &wh) ||
        !SDL_GetWindowSizeInPixels(win, &pw, &ph) || ww <= 0)
        return;
    float k = (float)pw / (float)ww;
    g_inset_l = (float)safe.x * k, g_inset_r = (float)(ww - safe.x - safe.w) * k;
}

static float cx(const Control *c, float w, float h) {
    return c->x < 0 ? w - atomic_load(&g_inset_r) + c->x * h : atomic_load(&g_inset_l) + c->x * h;
}

static atomic_bool g_touch_device;  // set by the first finger
static atomic_bool g_hidden;
static _Atomic uint64_t g_buttons;
static _Atomic int32_t g_lx, g_ly;

// (LARECOMP_TOUCHPAD_SHOW=1, tests: shown with a gamepad connected too, e.g. the Simulator's)
static bool force_shown(void) { static int f = -1; if (f < 0) f = getenv("LARECOMP_TOUCHPAD_SHOW") != NULL; return f; }
static bool shown(void) {
    return atomic_load(&g_touch_device) && (force_shown() || !input_gamepad_connected()) && !menu_input_captured();
}
bool touchpad_visible(void) { return atomic_load(&g_touch_device) && (force_shown() || !input_gamepad_connected()); }

// The fingers on the controller (UI thread only)
typedef struct Finger { SDL_FingerID id; int control; } Finger;  // id 0: free
static Finger fingers[10];

static int hit(float px, float py, float w, float h, bool hidden) {
    for (int i = 0; i < NCONTROLS; i++) {
        const Control *c = &CONTROLS[i];
        if (hidden && c->kind != C_TOGGLE) continue;
        float dx = px - cx(c, w, h), dy = py - c->y * h, r = c->r * h * (c->kind == C_STICK ? 1.3f : 1.25f);
        if (dx * dx + dy * dy <= r * r) return i;
    }
    return -1;
}

static void recompute(float w, float h, float (*pos)[2]) {
    uint64_t b = 0;
    int32_t lx = 0, ly = 0;
    for (int i = 0; i < 10; i++) {
        if (!fingers[i].id || fingers[i].control < 0) continue;
        const Control *c = &CONTROLS[fingers[i].control];
        if (c->kind == C_BUTTON) b |= 1ull << c->bit;
        if (c->kind == C_STICK) {
            float dx = (pos[i][0] - cx(c, w, h)) / (c->r * h), dy = (pos[i][1] - c->y * h) / (c->r * h);
            float m = sqrtf(dx * dx + dy * dy);
            if (m > 1) dx /= m, dy /= m;
            lx = (int32_t)(dx * 32767), ly = (int32_t)(-dy * 32767);
        }
    }
    g_buttons = b, g_lx = lx, g_ly = ly;
}

// UI thread: true when the event was the controller's (the touch screen and the game's other input don't see it)
bool touchpad_handle_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_WINDOW_SAFE_AREA_CHANGED || e->type == SDL_EVENT_WINDOW_RESIZED) {
        update_insets(SDL_GetWindowFromID(e->window.windowID));
        return false;
    }
    if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION && e->type != SDL_EVENT_FINGER_UP &&
        e->type != SDL_EVENT_FINGER_CANCELED)
        return false;
    g_touch_device = true;
    static float pos[10][2];
    int w = 1, h = 1;
    SDL_Window *win = SDL_GetWindowFromID(e->tfinger.windowID);
    if (win) SDL_GetWindowSizeInPixels(win, &w, &h);
    if (e->type == SDL_EVENT_FINGER_DOWN) update_insets(win);
    float px = e->tfinger.x * w, py = e->tfinger.y * h;
    int slot = -1;
    for (int i = 0; i < 10; i++)
        if (fingers[i].id == e->tfinger.fingerID + 1) slot = i;
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        if (slot >= 0 || !shown()) return false;
        int c = hit(px, py, (float)w, (float)h, atomic_load(&g_hidden));
        if (c < 0) return false;  // (the touch screen's)
        for (int i = 0; i < 10 && slot < 0; i++)
            if (!fingers[i].id) slot = i;
        if (slot < 0) return true;
        if (CONTROLS[c].kind == C_MENU) { menu_toggle(); return true; }
        if (CONTROLS[c].kind == C_TOGGLE) { g_hidden = !atomic_load(&g_hidden); return true; }
        fingers[slot] = (Finger){e->tfinger.fingerID + 1, c};
    } else {
        if (slot < 0) return false;
        int ctl = fingers[slot].control;
        if (e->type == SDL_EVENT_FINGER_MOTION && !(ctl >= 0 && CONTROLS[ctl].kind == C_STICK)) {
            int c = hit(px, py, (float)w, (float)h, false);  // (slides between buttons; off them: released)
            fingers[slot].control = c >= 0 && CONTROLS[c].kind == C_BUTTON ? c : -1;
        }
        if (e->type != SDL_EVENT_FINGER_MOTION) fingers[slot] = (Finger){0, -1};
    }
    if (slot >= 0) pos[slot][0] = px, pos[slot][1] = py;
    recompute((float)w, (float)h, pos);
    return true;
}

// The game's input (runtime/hle/input.c): what the controller holds now
void touchpad_state(uint64_t *buttons, int32_t *lx, int32_t *ly) {
    if (!shown()) { *buttons = 0, *lx = *ly = 0; return; }
    *buttons = atomic_load(&g_buttons), *lx = atomic_load(&g_lx), *ly = atomic_load(&g_ly);
}

// Render thread, inside the overlay's ImGui frame: the controller over the game (w, h: the drawable in pixels)
void touchpad_draw(float w, float h) {
    if (!shown()) return;
    ImDrawList *dl = igGetBackgroundDrawList_Nil();
    ImFont *font = igGetFont();
    const ImU32 ring = IM_COL32(255, 255, 255, 120), fill = IM_COL32(20, 20, 20, 70), held = IM_COL32(255, 255, 255, 110),
                text = IM_COL32(255, 255, 255, 200);
    uint64_t b = atomic_load(&g_buttons);
    bool hidden = atomic_load(&g_hidden);
    for (int i = 0; i < NCONTROLS; i++) {
        const Control *c = &CONTROLS[i];
        if (hidden && c->kind != C_TOGGLE) continue;
        ImVec2 p = IM_VEC2(cx(c, w, h), c->y * h);
        float r = c->r * h, t = fmaxf(2.0f, h * 0.003f);
        if (c->kind == C_STICK) {
            int32_t lx = atomic_load(&g_lx), ly = atomic_load(&g_ly);
            ImDrawList_AddCircleFilled(dl, p, r, fill, 48);
            ImDrawList_AddCircle(dl, p, r, ring, 48, t);
            ImVec2 k = IM_VEC2(p.x + lx / 32767.0f * r * 0.6f, p.y - ly / 32767.0f * r * 0.6f);
            ImDrawList_AddCircleFilled(dl, k, r * 0.45f, lx || ly ? held : IM_COL32(255, 255, 255, 60), 32);
            continue;
        }
        bool on = c->kind == C_BUTTON && (b >> c->bit & 1);
        ImDrawList_AddCircleFilled(dl, p, r, on ? held : fill, 32);
        ImDrawList_AddCircle(dl, p, r, ring, 32, t);
        if (c->kind == C_TOGGLE) {  // (a little controller outline: show / hide)
            float s = r * 0.5f;
            ImDrawList_AddRect(dl, IM_VEC2(p.x - s, p.y - s * 0.55f), IM_VEC2(p.x + s, p.y + s * 0.55f), text, s * 0.4f, 0, t);
            if (hidden) ImDrawList_AddLine(dl, IM_VEC2(p.x - s, p.y + s), IM_VEC2(p.x + s, p.y - s), text, t);
            continue;
        }
        if (c->kind == C_MENU) {  // (three lines)
            for (int k = -1; k <= 1; k++)
                ImDrawList_AddLine(dl, IM_VEC2(p.x - r * 0.45f, p.y + k * r * 0.3f), IM_VEC2(p.x + r * 0.45f, p.y + k * r * 0.3f), text, t);
            continue;
        }
        float size = r * (c->label[1] ? 0.8f : 1.0f);
        ImVec2 ts;
        ImFont_CalcTextSizeA(&ts, font, size, FLT_MAX, 0, c->label, NULL, NULL);
        ImDrawList_AddText_FontPtr(dl, font, size, IM_VEC2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f), text, c->label, NULL, 0.0f, NULL);
    }
}
