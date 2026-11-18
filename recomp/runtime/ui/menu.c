// In-game overlay menu built with Dear ImGui (Vulkan; input fed from SDL events).
//
// Threads: SDL events arrive on the main thread and are queued; the menu is laid out and drawn on the
// render thread inside backend_present, after the game image, into the same drawable.
#include "ui/ig.h"
#include "../game.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "option_rows.h"
#include "mii_import.h"
#include "mii_render.h"
#include "theme_draw.h"
#include "osk.h"
#include "character.h"
#include "history.h"
#include "playtime.h"
#include "clock.h"
#include "cards.h"
#include "webview.h"
bool vk_imgui_backend_init(void);  // runtime/nvn/vulkan/imgui_glue.c

#include "../game_rows.h"
#include "../hle.h"
#include "../settings.h"
#include "../services/nexcountry.h"
#include "menu.h"
void input_reset(void);
void controls_bindings_draw(float scale);  // controls_ui.mm
#include "../input_bind.h"
#include "../nvn/shader_ahead.h"
const char *menu_clipboard(void);  // (below: the system clipboard's text)
#include "../paths.h"
void rt_request_clean_rebuild(void);  // runtime/main.c (Rebuild Game Code: General > Game)
bool gamebuild_available(void);       // runtime/gamebuild.c (the recompiler is part of this copy)
#include "../platform.h"
#include "../nvn/framegen.h"
#include "system_font.h"
#include "launcher_art.h"  // (the dot art of the splash)

// ------------------------------------------------------------------ state shared with the main thread
static _Atomic bool g_open;
static _Atomic bool g_text_busy;  // a text field / the on-screen keyboard has the keys (render thread: ui_text_busy)
static _Atomic bool g_want_fullscreen_apply;

typedef struct QEvent {
    enum { QE_KEY, QE_ANALOG, QE_MOUSE_POS, QE_MOUSE_BUTTON, QE_WHEEL, QE_TEXT } kind;
    ImGuiKey key;
    bool down;
    float value, x, y;
    int button;
    bool touch;  // (mouse events SDL made up from a finger: SDL_TOUCH_MOUSEID)
    char text[32];
} QEvent;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static QEvent *q_events;  // (a growable array: count, capacity)
static size_t q_count, q_cap;
static float g_pixel_density = 1;

static void push(const QEvent *e) {
    pthread_mutex_lock(&q_lock);
    if (q_count == q_cap) {
        q_cap = q_cap ? q_cap * 2 : 64;
        q_events = (QEvent *)realloc(q_events, q_cap * sizeof(QEvent));
    }
    q_events[q_count++] = *e;
    pthread_mutex_unlock(&q_lock);
}

bool menu_is_open(void) { return atomic_load(&g_open); }
static void set_open(bool open);
static void restart_notice_flush(void);  // (below the notice)
void menu_test_open(void) {
    set_open(true);
    if (getenv("LARECOMP_MENU_TAB")) { extern int g_tab_request_ext; g_tab_request_ext = atoi(getenv("LARECOMP_MENU_TAB")); }
}

void menu_tabs_focus(bool first_tab);
static void set_open(bool open) {
    if (open == atomic_load(&g_open)) return;
    atomic_store(&g_open, open);
    input_reset();  // nothing held while the menu had focus leaks into the game
    bind_capture_cancel();
    if (open) {
        static bool opened;  // the controller's cursor: the first tab (Controls) the first time this session, later the
        menu_tabs_focus(!opened);  // selected one
        if (opened) opt_remember_arm();  // (scroll and selected row of every page come back: opt_cursor_restored)
        if (!opened && getenv("LARECOMP_MENU_TAB")) { extern int g_tab_request_ext; g_tab_request_ext = atoi(getenv("LARECOMP_MENU_TAB")); }  // (test hook: the first opening, by F1 too)
        opened = true;
    }
    // (game_mod_rows_hide: a game's secret settings rows go back to "set" / "not set")
    else { settings_save(), osk_cancel(), game_mod_rows_hide(); g_text_busy = false; restart_notice_flush(); }
}

static ImGuiKey key_from_scancode(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    case SDL_SCANCODE_A: return ImGuiKey_A;
    case SDL_SCANCODE_C: return ImGuiKey_C;
    case SDL_SCANCODE_V: return ImGuiKey_V;
    case SDL_SCANCODE_X: return ImGuiKey_X;
    case SDL_SCANCODE_Z: return ImGuiKey_Z;
    case SDL_SCANCODE_Q: return ImGuiKey_Q;  // previous tab
    case SDL_SCANCODE_E: return ImGuiKey_E;  // next tab
    default: return ImGuiKey_None;
    }
}

// Nintendo layout: the right face button (A) confirms, the bottom one (B) goes back. Bindings don't reach the menus
// (input_bind.h: none can lock the player out), with one exception: A and B swapped onto the bottom and right buttons
// (bind_pad_a = south, bind_pad_b = east, the Xbox habit) swaps them here too, as the game does (issue #40).
static bool menu_ab_swapped(void) {
    return g_bind_pad[BIND_A] == SDL_GAMEPAD_BUTTON_SOUTH && g_bind_pad[BIND_B] == SDL_GAMEPAD_BUTTON_EAST;
}
static ImGuiKey key_from_pad(Uint8 b) {
    bool sw = menu_ab_swapped();
    switch (b) {
    case SDL_GAMEPAD_BUTTON_EAST: return sw ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown;   // activate
    case SDL_GAMEPAD_BUTTON_SOUTH: return sw ? ImGuiKey_GamepadFaceDown : ImGuiKey_GamepadFaceRight;  // cancel / back
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

// ---- software keyboard (runtime/hle/swkbd.c: the game's name entry etc.) --------------------------------------
// The game thread waits in menu_keyboard_run (re-presenting the last frame); this dialog takes the text: typed
// directly when the player last used a keyboard, an on-screen keyboard (d-pad + A, B deletes, Start / + OK) when
// they last used a controller.
static _Atomic int g_kbd;  // 1 open, 2 confirmed, 3 cancelled
static _Atomic bool g_last_pad;
static char g_kbd_text[400];  // (a comment: 75 characters of up to 4 bytes)
static char g_kbd_title[200];  // the game's header or guide text, if it gives one
static bool g_kbd_id;  // this prompt asks for a Course ID or a Maker ID (nine characters: runtime/ui/osk.h's rules)
static int g_kbd_max = 8;
static bool capturing(void) { return atomic_load(&g_open) || atomic_load(&g_kbd) == 1 || webview_active(); }
// The on-screen controller's menu button (runtime/ui/touchpad.cpp)
void menu_toggle(void) { set_open(!atomic_load(&g_open)); }

// A notice over the game for a few seconds (bottom centre), from any thread: menu_notice(text, seconds)
static pthread_mutex_t notice_lock = PTHREAD_MUTEX_INITIALIZER;
static char notice_text[512];
static double notice_until;  // SDL_GetTicks seconds
void menu_notice(const char *text, double seconds) {
    pthread_mutex_lock(&notice_lock);
    snprintf(notice_text, sizeof notice_text, "%s", text);
    notice_until = SDL_GetTicks() / 1000.0 + seconds;
    pthread_mutex_unlock(&notice_lock);
    LOG("[notice] queued for %.0f s at %.1f s: %s\n", seconds, SDL_GetTicks() / 1000.0, text);
}
static bool notice_active(void) { return SDL_GetTicks() / 1000.0 < notice_until; }

// Settings that only take effect when the game starts again are marked "(restart)" in their label. The notice isn't
// drawn while the menu is open, so a change to one is remembered and announced as the menu closes.
enum { RESTART_SKIP_INTRO, RESTART_SKIP_MODE_SELECT, RESTART_LANGUAGE, RESTART_SHADER_FAST, N_RESTART };
static const char *const restart_names[N_RESTART] = {"Skip intro", "Skip mode select", "Language", "Prepare at full speed"};
static bool restart_changed[N_RESTART];
static bool quit_prompt_req;  // the "Quit to Desktop?" window is asked for (Quit > Quit to Desktop)
static bool restart_prompt_req;  // the "Restart the game?" window is asked for (a restart row changed, or Quit > Restart)
static void restart_changed_mark(int which) { restart_changed[which] = true, restart_prompt_req = true; }
static void restart_notice_flush(void) {
    char t[256] = "Applies after restart: ";
    size_t n = strlen(t);
    int count = 0;
    for (int i = 0; i < N_RESTART; i++) {
        if (!restart_changed[i]) continue;
        restart_changed[i] = false;
        n += snprintf(t + n, sizeof t - n, "%s%s", count++ ? ", " : "", restart_names[i]);
    }
    if (count) menu_notice(t, 5);
}
bool touchpad_visible(void);
void touchpad_draw(float w, float h);
// The game gets no input while this is true (runtime/hle/input.c): menu or name entry up.
bool menu_input_captured(void) { return capturing(); }

bool menu_handle_event(const SDL_Event *e) {
    static bool back_down, start_down;
    if (webview_active()) {  // the Play Guide's web view is up (runtime/ui/webview.c): the controller drives it, the game and the menu get nothing
        if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            ImGuiKey k = key_from_pad(e->gbutton.button);
            int b = k == ImGuiKey_GamepadDpadUp ? WV_PAD_UP : k == ImGuiKey_GamepadDpadDown ? WV_PAD_DOWN :
                    k == ImGuiKey_GamepadDpadLeft ? WV_PAD_LEFT : k == ImGuiKey_GamepadDpadRight ? WV_PAD_RIGHT :
                    k == ImGuiKey_GamepadFaceDown ? WV_PAD_CONFIRM : k == ImGuiKey_GamepadFaceRight ? WV_PAD_BACK : -1;
            if (b >= 0) webview_gamepad(b, e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
            return true;
        }
        if (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP || e->type == SDL_EVENT_TEXT_INPUT) return true;
    }
    if (atomic_load(&g_open) && bind_capture_event(e)) return true;  // Controls tab waiting for a new binding: it takes the press
    if (e->type == SDL_EVENT_KEY_DOWN) g_last_pad = false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) g_last_pad = true;
    if (atomic_load(&g_kbd) == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // Enter confirms, Esc cancels
        if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) { g_kbd = 2; return true; }
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { g_kbd = 3; return true; }
    }
    if (atomic_load(&g_kbd) == 1 && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
        g_kbd = 2;
        return true;
    }
    // typing (a text field, the on-screen keyboard, the name entry): keys are text, not menu shortcuts
    bool typing = atomic_load(&g_kbd) == 1 || (atomic_load(&g_open) && atomic_load(&g_text_busy));
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F1 ||
                               ((e->key.scancode == SDL_SCANCODE_ESCAPE || e->key.scancode == SDL_SCANCODE_AC_BACK) &&
                                !atomic_load(&g_open)))) {
            if (!typing) set_open(!atomic_load(&g_open));  // (Esc with the menu open goes to ImGui below: it cancels a field)
            return true;
        }
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F11 ||
                               (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) ||
                               (e->key.scancode == SDL_SCANCODE_F && (e->key.mod & SDL_KMOD_GUI) && (e->key.mod & SDL_KMOD_CTRL)))) {
            g_settings.fullscreen = !g_settings.fullscreen;
            atomic_store(&g_want_fullscreen_apply, true);
            settings_save();
            return true;
        }
        /* fallthrough */
    case SDL_EVENT_KEY_UP: {
        if (!capturing()) return false;
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (down && (e->key.scancode == SDL_SCANCODE_ESCAPE || e->key.scancode == SDL_SCANCODE_AC_BACK)) {  // Esc: back out of popups, else close
            QEvent q = {0};
            q.kind = QE_KEY; q.key = ImGuiKey_Escape; q.down = true; push(&q);
            q.down = false; push(&q);
            return true;
        }
        {   // the modifiers as ImGui wants them (its shortcuts: Ctrl / Cmd+V paste, +C copy, +A select all, +Z undo)
            const SDL_Keymod m = e->key.mod;
            const struct { ImGuiKey k; bool on; } mods[] = {{ImGuiMod_Ctrl, (m & SDL_KMOD_CTRL) != 0},
                {ImGuiMod_Shift, (m & SDL_KMOD_SHIFT) != 0}, {ImGuiMod_Alt, (m & SDL_KMOD_ALT) != 0},
                {ImGuiMod_Super, (m & SDL_KMOD_GUI) != 0}};
            for (size_t mi = 0; mi < sizeof mods / sizeof mods[0]; mi++) {
                QEvent q = {0}; q.kind = QE_KEY; q.key = mods[mi].k; q.down = mods[mi].on; push(&q);
            }
        }
        ImGuiKey k = key_from_scancode(e->key.scancode);
        if (k != ImGuiKey_None) { QEvent q = {0}; q.kind = QE_KEY; q.key = k; q.down = down; push(&q); }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (!capturing()) return false;
        { QEvent q = {0}; q.kind = QE_TEXT; strlcpy(q.text, e->text.text, sizeof q.text); push(&q); }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) back_down = down;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_START) start_down = down;
        if (down && !typing && ((back_down && start_down) || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) {
            set_open(!atomic_load(&g_open));
            return true;
        }
        if (!capturing()) return false;
        ImGuiKey k = key_from_pad(e->gbutton.button);
        if (k != ImGuiKey_None) { QEvent q = {0}; q.kind = QE_KEY; q.key = k; q.down = down; push(&q); }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!capturing()) return false;
        float v = e->gaxis.value / 32767.0f;
        QEvent q = {0};
        q.kind = QE_ANALOG;
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            q.key = ImGuiKey_GamepadLStickLeft; q.value = v < -0.3f ? -v : 0; push(&q);
            q.key = ImGuiKey_GamepadLStickRight; q.value = v > 0.3f ? v : 0; push(&q);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            q.key = ImGuiKey_GamepadLStickUp; q.value = v < -0.3f ? -v : 0; push(&q);
            q.key = ImGuiKey_GamepadLStickDown; q.value = v > 0.3f ? v : 0; push(&q);
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: q.key = ImGuiKey_GamepadL2; q.value = v; push(&q); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: q.key = ImGuiKey_GamepadR2; q.value = v; push(&q); break;
        default: break;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (!capturing()) return false;
        {
            QEvent q = {0};
            q.kind = QE_MOUSE_POS; q.x = e->motion.x * g_pixel_density; q.y = e->motion.y * g_pixel_density;
            q.touch = e->motion.which == SDL_TOUCH_MOUSEID;
            push(&q);
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!capturing()) return false;
        {
            QEvent q = {0};
            q.kind = QE_MOUSE_BUTTON;
            q.button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            q.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            q.touch = e->button.which == SDL_TOUCH_MOUSEID;
            push(&q);
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!capturing()) return false;
        { QEvent q = {0}; q.kind = QE_WHEEL; q.x = e->wheel.x; q.y = e->wheel.y; q.touch = e->wheel.which == SDL_TOUCH_MOUSEID; push(&q); }
        return true;
    default:
        return false;
    }
}

void menu_apply_window_requests(void *win) {
    SDL_Window *w = (SDL_Window *)win;
    g_pixel_density = SDL_GetWindowPixelDensity(w);
    static bool text_input;
    bool want_text = capturing();
    if (want_text != text_input) {
        if (want_text) SDL_StartTextInput(w);
        else SDL_StopTextInput(w);
        text_input = want_text;
    }
    static bool first = true;
    if (first || atomic_exchange(&g_want_fullscreen_apply, false)) {
        first = false;
        SDL_SetWindowFullscreen(w, g_settings.fullscreen);
    }
}

// The mouse cursor over the game window: shown while the menu is open (mouse navigation works at once), hidden otherwise. SDL only
// applies this over its own window, so other applications keep their cursor.
void menu_cursor_update(void) {
    if (atomic_load(&g_open) || !g_settings.hide_cursor) SDL_ShowCursor();  // (SDL does nothing when it is already so, so no state is kept here)
    else SDL_HideCursor();
}

// ------------------------------------------------------------------ render thread
static bool g_inited;
static ImGuiContext *g_ctx;  // ours: the start screen (back to it after a reboot) has its own context
static ImFont *g_font, *g_font_big;
static ImFont *g_menu_font, *g_menu_font_big;  // the menu window (Baloo 2); everything else keeps g_font
static ImFont *g_nes_font, *g_nes_font_big;    // the NES theme's menu window: Press Start 2P (runtime/nes_font.c)
extern const unsigned char g_nes_font_ttf[];
extern const unsigned int g_nes_font_ttf_len;
static int g_tab, g_tab_request = -2;  // (-2: no request; -1 is the Prelaunch tab)
static int g_focus_tab = -99;  // put the nav cursor on this tab next frame (-100: the selected one; -99: none)
static int g_focus_frames;
// The fonts are rasterised at the output's size (up to twice the 1080 p sizes), so text on a 4K screen is
// as sharp as at 1080 p rather than a scaled-up 1080 p atlas
static float g_atlas_scale = 1;

// The start screen's Settings opening: the controller's cursor starts on the first tab the first time in a session,
// later on the tab that's selected
void menu_tabs_focus(bool first_tab) { g_focus_tab = first_tab ? -1 : -100, g_focus_frames = 0; }
int g_tab_request_ext = -1;

// ---- themes (General tab > Theme; g_settings.theme). One palette per THEME_* in settings.h; to make another theme, add a
// row here and a name in settings.c g_theme_names (and bump N_THEMES).
typedef struct {
    ImVec4 window, child, popup;      // backgrounds: the menu window, its panes, drop-downs
    ImVec4 text, text_dim;            // text, and descriptions / disabled text
    ImVec4 frame, button, tab;        // resting fills of fields, buttons, tabs
    ImVec4 hover, active, pressed;    // hovered / selected-in-list / pressed
    ImVec4 tab_selected, tab_alt;     // the open tab; the tab under the controller's cursor
    ImVec4 line, muted;               // separators; the small section captions
    ImVec4 accent, accent_dim;        // titles, check marks, the cursor border; slider grab
    // shape (px at 1080 p, scaled with the window)
    float window_round, child_round, frame_round, tab_round;  // corner radii
    float window_border, frame_border;                        // outline widths of the menu window / fields (0: none)
    float row_round, row_border;                              // the option rows' corners; their outline width
    float row_notch;                                          // > 0: the rows' corners are steps of this size (NES.css)
    bool border_text;                                         // the window / field outline in the text colour, not the accent
} ThemePalette;

static const ThemePalette kThemes[N_THEMES] = {
    {   // Default: the start screen's look (launcher.c style_init) - the Build Helper console's black, blue (58,150,221)
        // buttons and borders, magenta rules, grey text, square and flat
        /*window*/ {0.047f, 0.047f, 0.047f, 1}, /*child*/ {0.047f, 0.047f, 0.047f, 1}, /*popup*/ {0.047f, 0.047f, 0.047f, 1},
        /*text*/ {0.800f, 0.800f, 0.800f, 1}, /*text_dim*/ {0.463f, 0.463f, 0.463f, 1},
        /*frame*/ {0.125f, 0.125f, 0.125f, 1}, /*button*/ {0.227f, 0.588f, 0.867f, 1}, /*tab*/ {0.125f, 0.125f, 0.125f, 1},
        /*hover*/ {0.420f, 0.691f, 0.900f, 1}, /*active*/ {0.340f, 0.640f, 0.885f, 1}, /*pressed*/ {0.170f, 0.441f, 0.650f, 1},
        /*tab_selected*/ {0.227f, 0.588f, 0.867f, 1}, /*tab_alt*/ {0.200f, 0.200f, 0.200f, 1},
        /*line*/ {0.533f, 0.090f, 0.596f, 1}, /*muted*/ {0.463f, 0.463f, 0.463f, 1},
        /*accent*/ {0.227f, 0.588f, 0.867f, 1}, /*accent_dim*/ {0.170f, 0.441f, 0.650f, 1},
        /*window_round*/ 0, /*child_round*/ 0, /*frame_round*/ 0, /*tab_round*/ 0,
        /*window_border*/ 2, /*frame_border*/ 0, /*row_round*/ 0, /*row_border*/ 1, /*row_notch*/ 0, /*border_text*/ false,
    },
    {   // NES: NES.css's dark container - #212529 panels with thick white 4 px borders, square edges, stepped corners on
        // the rows, and its palette: primary #209cee, success #4aa52e, warning #f7d51d / #e59400, disabled #adafbc
        /*window*/ {0.129f, 0.145f, 0.161f, 1}, /*child*/ {0.078f, 0.090f, 0.102f, 1}, /*popup*/ {0.129f, 0.145f, 0.161f, 1},
        /*text*/ {1, 1, 1, 1}, /*text_dim*/ {0.678f, 0.686f, 0.737f, 1},
        /*frame*/ {0.078f, 0.090f, 0.102f, 1}, /*button*/ {0.231f, 0.251f, 0.271f, 1}, /*tab*/ {0.231f, 0.251f, 0.271f, 1},
        /*hover*/ {0.125f, 0.612f, 0.933f, 1}, /*active*/ {0.063f, 0.553f, 0.878f, 1}, /*pressed*/ {0, 0.420f, 0.702f, 1},
        /*tab_selected*/ {0.290f, 0.647f, 0.180f, 1}, /*tab_alt*/ {0.125f, 0.612f, 0.933f, 1},
        /*line*/ {0.678f, 0.686f, 0.737f, 1}, /*muted*/ {0.678f, 0.686f, 0.737f, 1},
        /*accent*/ {1, 1, 1, 1}, /*accent_dim*/ {0.678f, 0.686f, 0.737f, 1},  // (white: no gold in this theme)
        /*window_round*/ 0, /*child_round*/ 0, /*frame_round*/ 0, /*tab_round*/ 0,
        /*window_border*/ 4, /*frame_border*/ 4, /*row_round*/ 0, /*row_border*/ 3, /*row_notch*/ 4, /*border_text*/ true,
    },
};

static const ThemePalette *theme_palette(void) {
    int t = g_settings.theme;
    return &kThemes[t >= 0 && t < N_THEMES ? t : THEME_DEFAULT];
}

// The current theme's accent as packed colour (a = 0..255), for the option rows and the other files' hand-drawn bits
uint32_t menu_accent(int a) { return igColorConvertFloat4ToU32(IM_VEC4(theme_palette()->accent.x, theme_palette()->accent.y, theme_palette()->accent.z, a / 255.0f)); }
uint32_t menu_muted(void) { return igColorConvertFloat4ToU32(theme_palette()->muted); }
float menu_row_round(void) { return theme_palette()->row_round; }
float menu_row_border(void) { return theme_palette()->row_border; }
float menu_row_notch(void) { return theme_palette()->row_notch; }
static bool nes_theme(void) { return theme_palette()->row_notch > 0; }   // (the pixel-art look: its own font, frame, icons)
static ImFont *menu_font(void) { return nes_theme() && g_nes_font ? g_nes_font : g_menu_font; }
static ImFont *menu_font_big(void) { return nes_theme() && g_nes_font_big ? g_nes_font_big : g_menu_font_big; }
bool menu_theme_nes(void) { return nes_theme(); }
static ImVec4 accent_vec(void) { return theme_palette()->accent; }

// The current theme (colours and shape, unscaled) into a style
static void theme_apply(ImGuiStyle *s) {
    const ThemePalette *pp = theme_palette();
    const ThemePalette p = *pp;
    s->WindowRounding = p.window_round; s->ChildRounding = p.child_round; s->FrameRounding = p.frame_round;
    s->GrabRounding = p.frame_round; s->TabRounding = p.tab_round; s->PopupRounding = p.frame_round;
    s->ScrollbarRounding = p.frame_round;
    s->WindowBorderSize = p.window_border; s->FrameBorderSize = p.frame_border; s->TabBarBorderSize = p.frame_border;
    ImVec4 *c = s->Colors;
    c[ImGuiCol_Border] = p.border_text ? p.text : p.accent;
    c[ImGuiCol_WindowBg] = p.window;
    c[ImGuiCol_ChildBg] = p.child;
    c[ImGuiCol_PopupBg] = p.popup;
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.text_dim;
    c[ImGuiCol_FrameBg] = p.frame;
    c[ImGuiCol_FrameBgHovered] = p.hover;
    c[ImGuiCol_FrameBgActive] = p.active;
    c[ImGuiCol_Button] = p.button;
    c[ImGuiCol_ButtonHovered] = p.hover;
    c[ImGuiCol_ButtonActive] = p.pressed;
    c[ImGuiCol_Header] = p.hover;
    c[ImGuiCol_HeaderHovered] = p.active;
    c[ImGuiCol_HeaderActive] = p.pressed;
    c[ImGuiCol_Tab] = p.tab;
    c[ImGuiCol_TabHovered] = p.hover;
    c[ImGuiCol_TabSelected] = p.tab_selected;
    c[ImGuiCol_TabSelectedOverline] = p.accent;
    c[ImGuiCol_CheckMark] = p.accent;
    c[ImGuiCol_SliderGrab] = p.accent_dim;
    c[ImGuiCol_SliderGrabActive] = p.accent;
    c[ImGuiCol_NavCursor] = p.accent;
    c[ImGuiCol_Separator] = p.line;
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
}

static void style_init(void) {
    ImGuiStyle *s = igGetStyle();
    igStyleColorsDark(NULL);
    s->WindowPadding = IM_VEC2(28, 24); s->FramePadding = IM_VEC2(14, 8); s->ItemSpacing = IM_VEC2(14, 12);
    s->ScrollbarSize = 18;
    theme_apply(s);
}


static const char *clipboard_get(ImGuiContext *ctx) { (void)ctx; return menu_clipboard(); }
static void clipboard_set(ImGuiContext *ctx, const char *t) { (void)ctx; SDL_SetClipboardText(t); }

static void init(void) {
    igDebugCheckVersionAndDataLayout(igGetVersion(), sizeof(ImGuiIO), sizeof(ImGuiStyle), sizeof(ImVec2), sizeof(ImVec4),
                                     sizeof(ImDrawVert), sizeof(ImDrawIdx));  // (IMGUI_CHECKVERSION)
    g_ctx = igCreateContext(NULL);
    igSetCurrentContext(g_ctx);
    ImGuiIO *io = igGetIO();
    io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io->BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io->IniFilename = NULL;
    // the system clipboard for the text fields (Ctrl / Cmd+V, and their Paste button: option_rows.mm)
    igGetPlatformIO_Nil()->Platform_GetClipboardTextFn = clipboard_get;
    igGetPlatformIO_Nil()->Platform_SetClipboardTextFn = clipboard_set;
    ImFontConfig *cfg = ImFontConfig_ImFontConfig();  // (the C++ constructor's defaults; freed below)
    cfg->OversampleH = 2;
    char sys[1024];
    int face = 0;
    if (ui_system_font(sys, sizeof sys, &face)) {  // (system_font.mm: an existing file; a missing one asserts in ImGui)
        cfg->FontNo = face;
        g_font = ImFontAtlas_AddFontFromFileTTF(io->Fonts, sys, 34 * g_atlas_scale, cfg, NULL);
        if (g_font) g_font_big = ImFontAtlas_AddFontFromFileTTF(io->Fonts, sys, 56 * g_atlas_scale, cfg, NULL);
        LOG("[menu] font %s\n", sys);
    }
    // the menu window itself: Baloo 2 SemiBold, as on the start screen (shipped, OFL), at its Settings window's sizes; the
    // FPS counter, overlays and on-screen keyboard keep the fonts above
    char baloo[1024];
    const char *menu_font_env = getenv("LARECOMP_MENU_FONT");
    if (menu_font_env) strlcpy(baloo, menu_font_env, sizeof baloo);  // (test) another weight / face
    if (getenv("LARECOMP_MENU_FONT") || rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", baloo, sizeof baloo)) {
        g_menu_font = ImFontAtlas_AddFontFromFileTTF(io->Fonts, baloo, 39 * g_atlas_scale, cfg, NULL);
        g_menu_font_big = ImFontAtlas_AddFontFromFileTTF(io->Fonts, baloo, 72 * g_atlas_scale, cfg, NULL);
    }
    {   // the NES theme's pixel font (compiled in; crisp: no oversampling, pixel-snapped)
        ImFontConfig *nc = ImFontConfig_ImFontConfig();
        nc->FontDataOwnedByAtlas = false;
        nc->OversampleH = nc->OversampleV = 1;
        nc->PixelSnapH = true;
        g_nes_font = ImFontAtlas_AddFontFromMemoryTTF(io->Fonts, (void *)g_nes_font_ttf, (int)g_nes_font_ttf_len, 20 * g_atlas_scale, nc, NULL);
        g_nes_font_big = ImFontAtlas_AddFontFromMemoryTTF(io->Fonts, (void *)g_nes_font_ttf, (int)g_nes_font_ttf_len, 32 * g_atlas_scale, nc, NULL);
        ImFontConfig_destroy(nc);
    }
    if (!g_font) {  // (no font file: ImGui's own, at the menu's size rather than its 13 px)
        LOG("[menu] no system font: the built-in one\n");
        ImFontConfig *d = ImFontConfig_ImFontConfig();
        d->SizePixels = 34 * g_atlas_scale;
        g_font = ImFontAtlas_AddFontDefault(io->Fonts, d);
        ImFontConfig_destroy(d);
    }
    if (!g_font_big) g_font_big = g_font;
    if (!g_menu_font || !g_menu_font_big) g_menu_font = g_font, g_menu_font_big = g_font_big;
    ImFontConfig_destroy(cfg);
    style_init();
    vk_imgui_backend_init();
    g_inited = true;
}

// Touch: the last pointer input was a finger (SDL's mice made up from touches). The
// menu then has bigger rows, tabs and scrollbars, and a drag scrolls the page.
static bool g_touch_ui = false;
static bool touch_layout(bool touch) {
    return touch;
}

// The clipboard's text (kept until the next call: SDL's copy is freed), "" when there is none
const char *menu_clipboard(void) {
    static char *last;
    SDL_free(last);
    last = SDL_HasClipboardText() ? SDL_GetClipboardText() : NULL;
    return last ? last : "";
}

static void drain_events(ImGuiIO *io) {
    pthread_mutex_lock(&q_lock);
    QEvent *ev = q_events;  // (take the whole queue)
    size_t n = q_count;
    q_events = NULL;
    q_count = q_cap = 0;
    pthread_mutex_unlock(&q_lock);
    for (size_t i = 0; i < n; i++) {
        const QEvent *q = &ev[i];
        switch (q->kind) {
        case QE_KEY: ImGuiIO_AddKeyEvent(io, q->key, q->down); break;
        case QE_ANALOG: ImGuiIO_AddKeyAnalogEvent(io, q->key, q->value > 0.1f, q->value); break;
        // A finger is a touch screen to ImGui: its press comes a frame after the move (there is no hover before a
        // tap), so a single tap selects a tab or a row (as a mouse's press, it came in the same frame as the jump to
        // the new spot, and the first tap often only moved there: tabs needed a double tap)
        case QE_MOUSE_POS:
            ImGuiIO_AddMouseSourceEvent(io, q->touch ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
            ImGuiIO_AddMousePosEvent(io, q->x, q->y);
            g_touch_ui = touch_layout(q->touch);
            break;
        case QE_MOUSE_BUTTON:
            ImGuiIO_AddMouseSourceEvent(io, q->touch ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
            ImGuiIO_AddMouseButtonEvent(io, q->button, q->down);
            g_touch_ui = touch_layout(q->touch);
            break;
        case QE_WHEEL:
            ImGuiIO_AddMouseSourceEvent(io, q->touch ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
            ImGuiIO_AddMouseWheelEvent(io, q->x, q->y);
            break;
        case QE_TEXT: ImGuiIO_AddInputCharactersUTF8(io, q->text); break;
        }
    }
    free(ev);
}

// Section header in the big font.
static void header(const char *t) {
    igPushFont(g_font_big);
    igTextColored(accent_vec(), "%s", t);
    igPopFont();
    igSeparator();
    igSpacing();
}

static void hint(const char *t) {
    igPushTextWrapPos(0);
    igTextDisabled("%s", t);
    igPopTextWrapPos();
}

// A row with a label on the left and a choice of options on the right (dpad left/right cycles).
// reserve: room kept free right of the combo (a button on the same line)
static bool choice(const char *label, int *value, const char *const *names, int n, float reserve) {
    igPushID_Str(label);
    igAlignTextToFramePadding();
    igTextUnformatted(label, NULL);
    ImVec2 avail;
    igGetContentRegionAvail(&avail);
    igSameLine(avail.x * 0.45f, -1);
    bool changed = false;
    igSetNextItemWidth(reserve > 0 ? -reserve : -1);
    if (igBeginCombo("##c", names[*value], 0)) {
        for (int i = 0; i < n; i++) {
            if (igSelectable_Bool(names[i], i == *value, 0, IM_VEC2(0, 0))) { *value = i; changed = true; }
            if (i == *value) igSetItemDefaultFocus();
        }
        igEndCombo();
    }
    if (igIsItemFocused() && !igIsPopupOpen_Str("##c", 0)) {
        if (igIsKeyPressed_Bool(ImGuiKey_GamepadDpadRight, true) || igIsKeyPressed_Bool(ImGuiKey_RightArrow, true)) {
            *value = (*value + 1) % n; changed = true;
        }
        if (igIsKeyPressed_Bool(ImGuiKey_GamepadDpadLeft, true) || igIsKeyPressed_Bool(ImGuiKey_LeftArrow, true)) {
            *value = (*value + n - 1) % n; changed = true;
        }
    }
    igPopID();
    return changed;
}

void input_pads_reassign(void);  // runtime/hle/input.c

static bool toggle(const char *label, bool *v) {
    const char *names[] = {"Off", "On"};
    int i = *v;
    bool ch = choice(label, &i, names, 2, 0);
    *v = i;
    return ch;
}

void renderer_get_scale(float *sx, float *sy);
int input_pad_family(void);  // runtime/hle/input.c
double output_scale_dynamic_factor(void);

static void tab_display(void) {
    float scale = igGetFontSize() / 34;
    opt_flat_begin("##video", scale);
    if (opt_section_fold("Display")) {
    bool fs = g_settings.fullscreen;
    if (opt_toggle("Fullscreen", &fs, false, "Fills the screen. Also: F11 or Alt+Enter.")) {
        g_settings.fullscreen = fs;
        atomic_store(&g_want_fullscreen_apply, true);
    }
    opt_toggle("Hide Mouse Cursor", &g_settings.hide_cursor, true, "Hides the mouse cursor over the game while this menu is closed.");
    opt_toggle("Show FPS", &g_settings.show_fps, false, "Shows the frame rate in the top right corner.");
    opt_toggle("Show Clock", &g_settings.show_clock, true,
               "Shows the computer's current time at the bottom of the menu, in the time zone and 12 / 24-hour format Windows uses.");
    opt_toggle("Show memory use", &g_settings.show_memory, false,
               "Shows how much memory the app uses in the top right corner, once a second.");

    }
    if (opt_section_fold("Rendering")) {
    // display order -> RES_* (the ini / enum order predates the menu order)
    // (up to 4x: 7680 x 4320, 8K - more only ran out of GPU memory, and sat next to 75% in the list)
    // (Match display in the middle: left from it 75% / 50% / 25%, right Dynamic then 1x-4x; user 2026-10-11)
    static const char *res[] = {"25%", "50%", "75%", "Match display", "Dynamic", "1x", "2x", "3x", "4x"};
    static const int res_mode[] = {RES_Q25, RES_Q50, RES_Q75, RES_MATCH, RES_DYNAMIC, 1, 2, 3, 4};
    enum { RES_MATCH_AT = 3 };
    enum { NRES = sizeof res_mode / sizeof res_mode[0] };
    {
        int ri = RES_MATCH_AT;
        for (int i = 0; i < NRES; i++) if (res_mode[i] == g_settings.res_mode) ri = i;
        float sx, sy;
        renderer_get_scale(&sx, &sy);
        char d[400];
        int n = snprintf(d, sizeof d, "How sharp the 3D image is. Match display: your screen's own resolution. Dynamic: "
                                      "as sharp as your GPU keeps at full speed. 1x-4x: the Switch's 1080p times that. "
                                      "75%% / 50%% / 25%%: a share of the display, for slower machines.\n\nRendering at "
                                      "%.0f x %.0f.", 1920.0 * sx, 1080.0 * sy);
        if (g_settings.res_mode == RES_DYNAMIC)
            snprintf(d + n, sizeof d - n, " Dynamic: %.0f%% of the display right now.", output_scale_dynamic_factor() * 100);
        if (opt_choice("Internal resolution", &ri, res, NRES, RES_MATCH_AT, d, false)) g_settings.res_mode = res_mode[ri];
    }
    {   // the upscaler next to bilinear (UPSCALE_SHARP: AMD FSR 1)
        static const char *ups[] = {"Bilinear", "AMD FSR 1"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. AMD FSR 1: "
                         "edge-aware upscaling and sharpening, sharper. Bilinear: plain and soft.";
        int ui = g_settings.upscaler == UPSCALE_BILINEAR ? 0 : 1;
        char udm[512];
        if (g_settings.res_mode == RES_MATCH) {  // (renders at the window's size: nothing to scale up)
            snprintf(udm, sizeof udm, "%s\n\nNot used at Match display: the game already renders at your display's resolution.", ud);
            ud = udm;
            opt_disable_next();
        }
        if (opt_choice("Upscaler", &ui, ups, 2, 1, ud, true)) g_settings.upscaler = ui ? UPSCALE_SHARP : UPSCALE_BILINEAR;
    }
    static const char *aa[] = {"Off", "SMAA"};
    opt_choice("Anti-aliasing", &g_settings.antialias, aa, 2, AA_OFF,
               "Smooths jagged edges of the game image before it is scaled to the window. SMAA: finds the edges and "
               "blends only along them, so textures stay sharp. Most useful at a lower internal resolution.", true);
    static const char *fg[] = {"Off", "120 fps"};
    int nfg = 2;
    opt_choice("Frame generation", &g_settings.frame_gen, fg, nfg, FRAMEGEN_OFF,
               "Adds a generated frame between each pair of real frames for 120 fps on 120 Hz displays (high-refresh "
               "monitors). The game itself still runs at 60. Adds a little input lag; fast motion may show small "
               "artifacts. Does nothing on a 60 Hz display.", true);

    }
    if (opt_section_fold("Shaders")) {  // the shader compile-ahead (GitHub #37): how far it is, and the switch to let it run at full speed
        AheadStatus a = shader_ahead_status();
        char v[64];
        if (a.state == 2) snprintf(v, sizeof v, "Prepared");
        else if (a.state == 1) snprintf(v, sizeof v, "Preparing: %d so far", a.items);
        else snprintf(v, sizeof v, "Starts with the game");
        opt_action("Shader preparation", v,
                   "The game's shaders are prepared in the background while you play, so effects don't hitch the first "
                   "time they appear. It takes a few minutes the first time, and again after an update; until it's "
                   "done, the first time something appears it may hitch briefly. Once prepared, they're kept.", true);
        if (opt_toggle("Prepare at full speed (restart)", &g_settings.ahead_fast, false,
                   "Prepares the shaders with every CPU core and without pausing for the game. It finishes much sooner "
                   "but can make the game stutter meanwhile: turn it on, wait at the title screen until Shader "
                   "preparation says Prepared, then turn it off if you like. From the next start.")) restart_changed_mark(RESTART_SHADER_FAST);
    }
    opt_end();
}

static void tab_general(void) {
    opt_flat_begin("##general", igGetFontSize() / 34);
    opt_section("Appearance");
    opt_choice_live("Theme", &g_settings.theme, g_theme_names, N_THEMES, THEME_DEFAULT,
               "The look of this menu (A opens the list and shows each one as you move over it; B puts the old one back):\n"
               "- Default: black with blue accents, square and flat, like the setup screen.\n"
               "- NES: the 8-bit look of NES.css (https://nostalgic-css.github.io/NES.css/) - dark panels, thick white "
               "borders, square edges and stepped corners.");
    static const char *sizes[] = {"Compact", "Small", "Normal", "Large", "Extra large"};
    static const float size_scale[] = {0.6f, 0.8f, 1.0f, 1.25f, 1.5f};
    enum { NSIZES = sizeof size_scale / sizeof size_scale[0], SIZE_NORMAL = 2 };
    int si = SIZE_NORMAL;
    for (int i = 0; i < NSIZES; i++)  // (the nearest preset, for a hand-edited value in the ini)
        if (fabsf(size_scale[i] - g_settings.menu_scale) < fabsf(size_scale[si] - g_settings.menu_scale)) si = i;
    if (opt_choice_live("Menu size", &si, sizes, NSIZES, SIZE_NORMAL,
                   "How big this menu is. Normal adapts to the window (smaller on small windows, growing more slowly "
                   "on big or high-resolution ones); Compact and Small fit more on the screen, Large and Extra large "
                   "are easier to read from a distance. Applies at once; A opens the list and shows each size as you move over it, "
                   "B puts the old one back."))
        g_settings.menu_scale = size_scale[si];

    opt_section("Game");
    const bool is_smm2 = !strcmp(GAME_ID, "smm2");
    const bool intro_locked = is_smm2 && g_settings.skip_mode_select;  // (Skip mode select takes over from launch)
    if (intro_locked) {  // (greyed out, with no On / Off: the setting isn't used while Skip mode select is on)
        opt_disable_next();
        opt_action("Skip intro (restart)", "Unavailable",
                   "Not available while Skip mode select is on: it replaces Skip intro. Skip mode select is a combo "
                   "of no intro video and no mode select, so it already skips the logos itself. Turn Skip mode "
                   "select off to use this on its own.", true);
    } else if (opt_toggle("Skip intro (restart)", &g_settings.skip_intro, false,
                          "Skips the opening logos and goes straight to the title screen when the game starts. Applies the next time the game starts.")) {
        settings_save();
        restart_changed_mark(RESTART_SKIP_INTRO);
    }
    if (is_smm2 &&  // (Super Mario Maker 2's: mods/skip_intro.c)
        opt_toggle("Skip mode select (restart)", &g_settings.skip_mode_select, false,
                   "Replaces Skip intro: a combo of no intro video and no mode select. Goes from launch straight into "
                   "Course World, with no logos, no L+R and no choosing a mode, all hidden behind a loading screen. "
                   "Applies the next time the game starts (Quit > Restart the Game does it too). Because it includes "
                   "Skip intro, Skip intro is unavailable while this is on.")) {
        settings_save();
        restart_changed_mark(RESTART_SKIP_MODE_SELECT);
    }
    if (is_smm2) {  // (where Skip mode select goes: read when the game starts)
        static const char *const to[N_SKIP_TO] = {"Course World", "Make"};
        if (!g_settings.skip_mode_select) opt_disable_next();
        if (opt_choice("Skip mode select goes to (restart)", &g_settings.skip_mode_select_to, to, N_SKIP_TO, SKIP_TO_COURSE_WORLD,
                       "Where Skip mode select lands. Course World: the Play side's Course World. Make: the Course Maker, one "
                       "press from the title screen's L+R. Only used while Skip mode select is on. Applies the next time the game "
                       "starts (Quit > Restart the Game does it too).", true)) {
            settings_save();
            restart_changed_mark(RESTART_SKIP_MODE_SELECT);
        }
    }
    if (opt_toggle("Keep build cache", &g_settings.keep_obj_cache, false,
                   "Off: the compiled game code is thrown away once it is built, so it takes no extra space. On: about "
                   "800 MB is kept next to the game files, and a new version of " GAME_TITLE " that changes only some of the "
                   "game code recompiles just those parts the first time you start it, instead of all of it. Turning it "
                   "off deletes what is kept.")) {
        if (!g_settings.keep_obj_cache) rt_remove_game_file("extracted", ".objcache");
        settings_save();
    }
    {  // Rebuild Game Code: delete the compiled game code and its cache, restart, and the setup screen builds it again
        static int ask_rebuild;
        if (ask_rebuild && (igIsKeyPressed_Bool(ImGuiKey_GamepadFaceRight, false) || igIsKeyPressed_Bool(ImGuiKey_Escape, false)))
            ask_rebuild = 0;
        const char *what = "Deletes the compiled game code and the build cache, closes the game and starts the app again, "
                           "and the setup screen compiles it all from scratch. It takes several minutes and needs about 4 GB "
                           "of free memory and disk. Your saves and settings are not touched. Use it if the game code "
                           "seems out of date or broken after an update. Progress since your last save is lost.";
        const char *d = what;
        if (!gamebuild_available()) {  // (without the recompiler there would be no way back)
            opt_disable_next();
            opt_action("Rebuild Game Code", "Unavailable", "The recompiler is not part of this copy of " GAME_TITLE
                       ", so the game code could not be built again.", true);
        } else if (!ask_rebuild) {
            if (opt_action("Rebuild Game Code", "", d, true)) ask_rebuild = 1;
        } else {
            if (opt_action("Yes, rebuild from scratch", "", d, true)) {
                settings_save();
                rt_request_clean_rebuild();
                set_open(false);
                ask_rebuild = 0;
            }
            if (opt_action("Cancel", "", "Stays where you are.", true)) ask_rebuild = 0;
        }
    }
    if (opt_toggle("Show recomp version", &g_settings.show_recomp_version, true,
                   "Off: the title screen's Ver. box shows the game's own version (3.0.3). On: it shows this "
                   "recomp's version instead. With Skip intro or Skip mode select on, the same box also shows in the "
                   "bottom right corner while the game starts, then goes away. The title screen's box changes the "
                   "next time the title screen loads.")) {
        settings_save();
    }
    opt_section("Players");
    if (opt_toggle("Clean Usernames", &g_settings.clean_usernames, true,
                   "Some OpenCourse World players have symbols in their name that the game draws as boxes (the console's "
                   "button icons and other private-use characters), or line breaks that make the name several lines tall. "
                   "On: those are taken out of other players' names and what is left is shown. Off: shown as "
                   "OpenCourse World sends it. Applies to players the game loads from now on.")) settings_save();
    if (opt_toggle("Maker Flags", &g_settings.maker_flags, true,
                   "OpenCourse World does not know where makers are from, so the game has no flag to show. On: every maker "
                   "gets one anyway: a guess from the language of their name (Japanese kana is Japan, Korean is Korea, "
                   "and so on), otherwise one picked at random, the same for that maker every time, most often the US or "
                   "Japan. These flags are made up, not where anyone really is. Off: no flags. Applies to players the "
                   "game loads from now on.")) settings_save();
    if (g_settings.maker_flags) {
        // the flags by country name, in alphabetical order (the list in nexcountry.h is by code)
        static int order[110];
        static const char *sorted[110];
        static bool built;
        const int nf = (int)(sizeof NEXC_FLAGS / sizeof NEXC_FLAGS[0]);
        if (!built) {
            for (int i = 0; i < nf; i++) order[i] = i;
            for (int i = 1; i < nf; i++) {  // (insertion sort by name)
                int v = order[i], j = i - 1;
                while (j >= 0 && strcmp(NEXC_NAMES[order[j]], NEXC_NAMES[v]) > 0) order[j + 1] = order[j], j--;
                order[j + 1] = v;
            }
            for (int i = 0; i < nf; i++) sorted[i] = NEXC_NAMES[order[i]];
            built = true;
        }
        int cur = 0, def = 0;
        for (int i = 0; i < nf; i++) {
            if (!strcmp(NEXC_FLAGS[order[i]], g_settings.own_flag)) cur = i;
            if (!strcmp(NEXC_FLAGS[order[i]], "CA")) def = i;
        }
        if (opt_choice("Your Flag", &cur, sorted, nf, def,
                       "The flag shown for you, wherever your maker appears in the game. Only the flag: it is not sent to "
                       "OpenCourse World or seen by anyone else. Applies to players the game loads from now on.", true)) {
            memcpy(g_settings.own_flag, NEXC_FLAGS[order[cur]], 3);
            settings_save();
        }
    }
    if (opt_toggle("Strip Discord Tags",&g_settings.strip_discord_tags, true,
                   "Players who came to OpenCourse World from Discord can have a \"#\" or \"#0\" left at the end of their "
                   "name (Discord's old name tag, e.g. \"name#0\"). On: that is taken off other players' names. Off: shown "
                   "as OpenCourse World sends it. Tags with other numbers (\"name#1234\") are left alone. Applies to players "
                   "the game loads from now on.")) settings_save();
    if (opt_toggle("Fix Broken Miis",&g_settings.replace_bad_mii, true,
                   "Some OpenCourse World players have a blank or broken Mii, which shows up in the game wherever they "
                   "appear. On: it is replaced by a generated Mii that stays the same for that player until the app closes. Off: shown as "
                   "OpenCourse World sends it. Your own Mii is never changed. Applies to players the game loads from now "
                   "on.")) settings_save();
    opt_end();
}

static void tab_audio(void) {
    opt_flat_begin("##audio", igGetFontSize() / 34);
    opt_section("Sound");
    if (opt_toggle("Mute", &g_settings.mute, false,
                   "Silences all the game's sound. Volume is greyed out while this is on and keeps its value for when you "
                   "turn it back off. Applies at once.")) {
        settings_save();
    }
    float pct = roundf(g_settings.volume * 100);
    if (g_settings.mute) opt_disable_next();  // (greyed out: it isn't used while muted)
    if (opt_slider("Volume", &pct, 0, 200, 5, 100, "%g%%",
                   "The game's volume. 100% is the game's own level; above it, louder."))
        g_settings.volume = pct / 100.0f;
    if (opt_toggle("Mute in Background", &g_settings.mute_in_background, true,
                   "Fades the game's sound out when you switch to another window or minimize this one, and back in when you "
                   "return. Mute and Volume keep their values. The game keeps running. Opening this menu does not count as leaving.")) {
        settings_save();
    }
    opt_toggle("Reverb", &g_settings.reverb, true, "The game's reverb effect.");
    opt_toggle("Movie audio", &g_settings.movie_audio, true,
               "Sound in the game's movies. Applies the next time a movie starts.");
    opt_end();
}

bool launcher_has_settings(void);   // runtime/ui/launcher.cpp: the Mods tab's app section (desktop)
bool launcher_settings_rows(void);
bool online_rows_available(void);   // runtime/ui/online_rows.cpp: the Online section (OpenCourse World's rows come from the game)
void online_rows(void);
void mods_page(void);          // runtime/ui/mods_page.cpp: the tab's pages (built-in, installed, GameBanana, online)
bool mods_page_used_back(void);  // (B left a mod's page there: the menu stays open)
void menu_close(void) { set_open(false); }
static void tab_mods(void) {
    opt_begin("##mods", igGetFontSize() / 34);
    mods_page();
    opt_no_reset();  // (mods and their lists: nothing here to put back to defaults)
    opt_end();
}

void stats_rows(void);   // runtime/ui/stats_rows.c
static void tab_stats(void) {
    ImVec2 avail;  // (cards, not option rows: its own child, which the controller scrolls by moving between the cards)
    igGetContentRegionAvail(&avail);
    igBeginChild_Str("##stats", avail, ImGuiChildFlags_NavFlattened, 0);
    stats_rows();
    igEndChild();
}

void achievements_rows(void);   // runtime/ui/achievements_rows.c
static void tab_achievements(void) {
    ImVec2 avail;  // (cards, as the Stats tab: its own child, which the controller scrolls by moving between the cards)
    igGetContentRegionAvail(&avail);
    igBeginChild_Str("##achievements", avail, ImGuiChildFlags_NavFlattened, 0);
    achievements_rows();
    igEndChild();
}

// Search: a random course from OpenCourse World's list that nobody has cleared and the player never played (issue #24), with the
// filters of knflrpn.com/mm2 and smm-uncleared.com (less the year). The filters are rows on the left, the pick a box on the right (as
// the Stats tab's); the lists are read in the background (games/smm2/mods/search.c).
void menu_close(void);
static void search_time(char *out, size_t cap, unsigned ms) {
    if (ms < 60000) snprintf(out, cap, "%u.%02us", ms / 1000, ms % 1000 / 10);
    else snprintf(out, cap, "%um %02us", ms / 60000, ms / 1000 % 60);
}
static const char *const search_themes[] = {"Any", "Overworld", "Underground", "Castle", "Airship", "Underwater", "Ghost House", "Snow", "Desert", "Sky", "Forest"};
static const char *const search_tags[] = {"Any", "Standard", "Puzzle-Solving", "Speedrun", "Autoscroll", "Auto-Mario", "Short and Sweet", "Multiplayer Versus",
                                          "Themed", "Music", "Art", "Technical", "Shooter", "Boss Battle", "Single Player", "Link"};
static bool s_uncleared = true, s_unplayed = true, s_nozero = true;
static int s_style, s_theme, s_tag, s_cond, s_tmin, s_tmax, s_cmin, s_cmax;
static const unsigned s_tries_val[] = {0, 50, 100, 200, 500, 1000};
static const unsigned s_time_val[] = {0, 30000, 60000, 120000, 240000};

static SearchFilter search_filter(void) {
    return (SearchFilter){.uncleared = s_uncleared, .unplayed = s_unplayed, .style = s_style - 1, .theme = s_theme - 1, .tag = s_tag ? s_tag : -1,
                          .condition = s_cond, .min_tries = s_tries_val[s_tmin], .max_tries = s_tries_val[s_tmax],
                          .min_ms = s_time_val[s_cmin], .max_ms = s_time_val[s_cmax], .skip_zero_ms = s_nozero};
}

static void search_filters(void) {
    static const char *const styles[] = {"Any", "Super Mario Bros.", "Super Mario Bros. 3", "Super Mario World", "New Super Mario Bros. U", "Super Mario 3D World"};
    static const char *const conds[] = {"Any", "None", "Has one"};
    static const char *const tries_names[] = {"Any", "50", "100", "200", "500", "1000"};
    static const char *const time_names[] = {"Any", "30 seconds", "60 seconds", "2 minutes", "4 minutes"};
    opt_in_column(true);  // (its column is half the window already)
    opt_flat_begin("##search", igGetFontSize() / 34);
    opt_section("Courses");
    opt_toggle("Uncleared", &s_uncleared, true, "Only courses nobody has cleared yet.");
    opt_toggle("Unplayed", &s_unplayed, true,
               "Only courses you have never played, from the list of courses OpenCourse World has for you. Needs your maker code, which the game "
               "learns when it is online.");
    opt_choice("Game Style", &s_style, styles, 6, 0, "Any, or one of the five game styles.", true);
    opt_choice("Theme", &s_theme, search_themes, 11, 0, "Any, or one level theme.", true);
    opt_choice("Tag", &s_tag, search_tags, 16, 0, "Any, or a course with this tag (either of its two).", true);
    opt_choice("Clear Condition", &s_cond, conds, 3, 0, "Any, courses with no clear condition, or courses that have one.", true);
    opt_choice("Min. Attempts", &s_tmin, tries_names, 6, 0, "At least this many attempts by everyone together.", true);
    opt_choice("Max. Attempts", &s_tmax, tries_names, 6, 0, "At most this many attempts by everyone together.", true);
    opt_choice("Min. Clear Check", &s_cmin, time_names, 5, 0, "The clear check (the course's maker clearing it before upload) took at least this long.", true);
    opt_choice("Max. Clear Check", &s_cmax, time_names, 5, 0, "The clear check took at most this long.", true);
    opt_toggle("Exclude 0-Second Clear Checks", &s_nozero, true, "Hide courses whose clear check time is exactly 0 seconds.");
    opt_no_reset();
    opt_end();
    opt_in_column(false);
}

// A button in a box, drawn as the option pages' buttons are (no box until pointed at, a gold edge then, the label at the left, dim when it
// can't be used) and at the box's next row: true only when it is used
static bool search_button(Card *c, const char *label, bool enabled) {
    float fs = c->fs, h = fs * 1.7f;
    ImVec2 back;
    igGetCursorScreenPos(&back);
    ImVec2 p = IM_VEC2(c->x + fs * 0.3f, c->y);
    float w = c->w - fs * 0.6f;
    igSetCursorScreenPos(p);
    igPushID_Str(label);
    igPushStyleColor_Vec4(ImGuiCol_NavCursor, IM_VEC4(0, 0, 0, 0));
    if (!enabled) igBeginDisabled(true);
    bool clicked = igInvisibleButton("##b", IM_VEC2(w, h), ImGuiButtonFlags_EnableNav);
    bool hot = enabled && (igIsItemHovered(0) || (igIsItemFocused() && igGetCurrentContext()->NavCursorVisible));
    if (!enabled) igEndDisabled();
    igPopStyleColor(1);
    igPopID();
    ImDrawList *dl = c->dl;
    if (hot) {
        ImDrawList_AddRectFilled(dl, p, IM_VEC2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 22), 0, 0);
        ImDrawList_AddRect(dl, p, IM_VEC2(p.x + w, p.y + h), menu_accent(255), 0, 0, 2.0f);
    }
    ImDrawList_AddText_Vec2(dl, IM_VEC2(p.x + fs * 0.4f, p.y + (h - fs) * 0.5f), enabled ? IM_COL32(240, 236, 224, 255) : IM_COL32(110, 110, 102, 255), label, NULL);
    igSetCursorScreenPos(back);
    c->y += h + fs * 0.1f;
    return clicked && enabled;
}

uint64_t vk_imgui_texture_picture(const void *rgba, int w, int h);  // runtime/nvn/vulkan/imgui_glue.c

// The picture of the course in the Find box: 16:9 under the button; a spinning arc while it comes, "No picture" if it won't
static void search_picture(const Card *c, const char *id, float iw, float ih, int64_t created) {
    static char shown[12];  // (the course whose picture is in the texture)
    static uint64_t tid;
    float fs = c->fs;
    float x0 = c->x + (c->w - iw) * 0.5f;  // (centred in the box)
    ImVec2 p0 = IM_VEC2(x0, c->y), p1 = IM_VEC2(x0 + iw, c->y + ih);
    int w = 0, h = 0;
    const uint8_t *px = game_search_image(id, &w, &h);
    if (px && w > 0 && (strcmp(shown, id) || !tid)) {
        tid = vk_imgui_texture_picture(px, w, h);
        snprintf(shown, sizeof shown, "%s", id);
    }
    if (px && tid && !strcmp(shown, id)) {
        ImDrawList_AddImage(c->dl, (ImTextureID)tid, p0, p1, IM_VEC2(0, 0), IM_VEC2(1, 1), IM_COL32(255, 255, 255, 255));
    } else {
        ImDrawList_AddRectFilled(c->dl, p0, p1, IM_COL32(0, 0, 0, 70), fs * 0.2f, 0);
        if (w < 0) {
            const char *t = "No picture";
            ImDrawList_AddText_Vec2(c->dl, IM_VEC2((p0.x + p1.x) * 0.5f - text_w(t) * 0.5f, (p0.y + p1.y) * 0.5f - fs * 0.5f), menu_muted(), t, NULL);
        } else {
            ui_spin(c->dl, (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f, fs * 0.5f, menu_muted(), (float)(SDL_GetTicks() / 1000.0 * 5.0));
        }
    }
    if (created > 0) {  // the upload's date at the left and time at the right, over a dark strip along the bottom edge (as opencourse.world's)
        time_t tt = (time_t)created;
        struct tm *lt = localtime(&tt);
        char d[16] = "", t[8] = "";
        if (lt) strftime(d, sizeof d, "%m/%d/%Y", lt), strftime(t, sizeof t, "%H:%M", lt);
        float sh = fs * 1.4f;
        ImDrawList_AddRectFilled(c->dl, IM_VEC2(p0.x, p1.y - sh), p1, IM_COL32(0, 0, 0, 150), fs * 0.2f, ImDrawFlags_RoundCornersBottom);
        float ty = p1.y - sh + (sh - fs) * 0.5f;
        ImDrawList_AddText_Vec2(c->dl, IM_VEC2(p0.x + fs * 0.4f, ty), IM_COL32(255, 255, 255, 235), d, NULL);
        ImDrawList_AddText_Vec2(c->dl, IM_VEC2(p1.x - fs * 0.4f - text_w(t), ty), IM_COL32(255, 255, 255, 235), t, NULL);
    }
    ImDrawList_AddRect(c->dl, p0, p1, menu_accent(80), fs * 0.2f, 0, 1.5f);
}

static void search_find(void) {
    static const char *const sn[] = {"SMB1", "SMB3", "SMW", "NSMBU", "3DW"};
    float fs = igGetFontSize();
    SearchStatus st;
    game_search_status(&st);
    if (!st.online) {
        Card c = card_begin("Find", rows_h(fs, 1));
        card_row(&c, "Add your OpenCourse World config in the Online tab", "");
        card_end(&c);
        return;
    }
    SearchCourse r;
    char err[96];
    int rs = game_search_result(&r, err);
    ImVec2 av;
    igGetContentRegionAvail(&av);
    float wrap = av.x - fs * 1.4f, eh = 0;
    if (rs == 4) {  // (what went wrong, wrapped inside the box)
        ImVec2 sz;
        igCalcTextSize(&sz, err, NULL, false, wrap);
        eh = sz.y + fs * 0.2f;
    }
    int rows = 1 + (rs == 2 ? (r.title[0] != 0) + (r.owner[0] != 0) + 5 : rs == 1 || rs == 3 ? 1 : 0);  // (Course List, then the pick)
    float iw = (av.x - fs * 1.4f) * 0.7f, ih = iw * 9.0f / 16.0f;  // (the picture: 70% of the box)
    float h = rows_h(fs, rows) + fs * 1.8f * (rs == 2 ? 2 : 1) + eh + (rs == 2 ? ih + fs * 0.3f + fs * 1.5f : 0);
    Card c = card_begin("Find", h);
    bool busy = st.loading || st.played_loading;
    if (ui_card_refresh(&c, busy)) game_search_refresh();
    // the list: its size, or the leaderboards' spinning arc while it is read
    char v[96];
    if (st.loading) v[0] = 0;
    else if (st.error[0]) snprintf(v, sizeof v, "not read");
    else snprintf(v, sizeof v, "%u courses", st.loaded);
    float y = c.y;
    card_row(&c, "Course List", v);
    if (st.loading) {
        float rr = fs * 0.32f;
        ui_spin(c.dl, c.x + c.w - fs * 0.7f - rr, y + fs * 0.5f, rr, menu_muted(), (float)(SDL_GetTicks() / 1000.0 * 5.0));
    }
    // greyed until the lists are in (the course list, and the played list when Unplayed is on), and while a pick is made
    bool ready = !st.loading && st.loaded > 0 && (!s_unplayed || st.played_known) && rs != 1;
    SearchFilter f = search_filter();
    if (search_button(&c, rs == 2 ? "Find Another" : "Find a Course", ready)) game_search_roll(&f);
    if (rs == 1) card_row(&c, "Looking", "...");
    else if (rs == 3) card_row(&c, "Result", "nothing matches");
    else if (rs == 4) {
        ImVec2 back;
        igGetCursorScreenPos(&back);
        igSetCursorScreenPos(IM_VEC2(c.x + fs * 0.7f, c.y));
        igPushTextWrapPos(c.x + fs * 0.7f + wrap);
        igTextDisabled("%s", err);
        igPopTextWrapPos();
        igSetCursorScreenPos(back);
        c.y += eh;
    } else if (rs == 2) {
        search_picture(&c, r.id, iw, ih, r.created);
        c.y += ih + fs * 0.3f;
        card_row(&c, "Course ID", r.id);
        if (r.title[0]) card_row(&c, "Name", r.title);
        if (r.owner[0]) card_row(&c, "Maker", r.owner);
        snprintf(v, sizeof v, "%s, %s", r.style < 5 ? sn[r.style] : "?", r.theme < 10 ? search_themes[r.theme + 1] : "?");
        card_row(&c, "Style", v);
        if (r.tag1 && r.tag1 <= SEARCH_TAGS)
            snprintf(v, sizeof v, "%s%s%s", search_tags[r.tag1], r.tag2 && r.tag2 <= SEARCH_TAGS ? ", " : "", r.tag2 && r.tag2 <= SEARCH_TAGS ? search_tags[r.tag2] : "");
        else snprintf(v, sizeof v, "none");
        card_row(&c, "Tags", v);
        snprintf(v, sizeof v, "%u clears, %u attempts", r.clears, r.tries);
        card_row(&c, "Record", v);
        search_time(v, sizeof v, r.clear_check_ms);
        card_row(&c, "Clear Check", v);
        static char launch_err[96];
        static char launch_id[12];
        if (strcmp(launch_id, r.id)) launch_id[0] = 0, launch_err[0] = 0;  // (another course: the old answer is gone)
        if (search_button(&c, "Take to the Game", true)) {
            snprintf(launch_id, sizeof launch_id, "%s", r.id);
            if (game_search_launch(r.id, launch_err)) menu_close();  // (Course World's ID search)
        }
        if (launch_err[0]) {
            ImVec2 back;
            igGetCursorScreenPos(&back);
            igSetCursorScreenPos(IM_VEC2(c.x + fs * 0.7f, c.y));
            igPushTextWrapPos(c.x + c.w - fs * 0.7f);
            igTextDisabled("%s", launch_err);
            igPopTextWrapPos();
            igSetCursorScreenPos(back);
            c.y += fs * 1.5f;
        }
    }
    card_end(&c);
}

static void tab_search(void) {
    float fs = igGetFontSize();
    ImVec2 av;
    igGetContentRegionAvail(&av);
    int cols = av.x > fs * 38 ? 2 : 1;  // (as the Stats tab: two columns when the window is wide)
    if (!igBeginTable("##search_cols", cols, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX, IM_VEC2(0, 0), 0)) return;
    igTableNextColumn();
    if (cols == 1) {  // (one column: the pick first, the filters under it)
        search_find();
        search_filters();
    } else {
        search_filters();
        igTableNextColumn();
        igBeginChild_Str("##find", IM_VEC2(0, 0), ImGuiChildFlags_NavFlattened, 0);
        search_find();
        igEndChild();
    }
    igEndTable();
}

void online_page(void);  // runtime/ui/mods_page.cpp: the backend, its accounts, the game's rows for it, the app's
static void tab_online(void) {
    opt_flat_begin("##online", igGetFontSize() / 34);
    online_page();
    opt_no_reset();  // (the account: no page of settings to reset)
    opt_end();
}

// Quit: restart the game or out to the desktop (there is no way back to the setup screen: the game files are set up
// once). Each asks first (progress since the last save is lost); B / Esc or Cancel backs out. Under it, About: the
// version and the app's rows (updates).
void rt_request_reboot(int kind);  // runtime/main.c (RT_REBOOT_TITLE = 2: the start screen)
// "v0.5.3"
#ifndef RECOMP_VERSION
#define RECOMP_VERSION "dev"  // (the build passes the tag or git's description)
#endif
static const char *app_version(void) {
    static char v[64];
    if (!v[0]) {
        snprintf(v, sizeof v, "%s", RECOMP_VERSION);
    }
    return v;
}
static void tab_quit(float scale, bool in_game) {
    // (both ask in their own window: restart_popup)
    const char *lost = in_game ? "\n\nProgress since your last save is lost." : "";
    char d0[200], d2[200];
    snprintf(d0, sizeof d0, "Starts the game again right away, without closing the app. Settings that apply at the next start take effect.%s", lost);
    snprintf(d2, sizeof d2, "Closes " GAME_TITLE ".%s", lost);
    opt_flat_begin("##quit", scale);
    opt_section("Quit");
    if (!in_game) opt_disable_next();  // (nothing running to restart)
    if (opt_action("Restart the Game", "", in_game ? d0 : "No game is running.", true)) restart_prompt_req = true;
    if (opt_action("Quit to Desktop", "", d2, true)) quit_prompt_req = true;
    opt_section("About");
    opt_action("Version", app_version(), "This program's version. Please include it in bug reports.", false);
    launcher_settings_rows();  // (Check for updates, New version available)
    opt_no_reset();  // (Quit, Version, one update switch: nothing here to put back to defaults)
    opt_end();
}

// "Restart the game?" (asked for by a row that needs a restart, restart_changed_mark, and by Quit > Restart the Game;
// Not now leaves the settings noted: a notice reminds as the menu closes) and "Quit to Desktop?".
static void restart_popup(float scale, bool in_game) {
    if (in_game) {
        if (opt_confirm_window("Restart the game?##restart", &restart_prompt_req, "Restart the game now?", true, "Restart now", "Not now", scale) == 1) {
            memset(restart_changed, 0, sizeof restart_changed);
            settings_save();
            rt_request_reboot(1 /* RT_REBOOT_GAME */);
            set_open(false);
        }
    } else {
        restart_prompt_req = false, memset(restart_changed, 0, sizeof restart_changed);  // (the start screen: nothing to restart)
    }
    if (opt_confirm_window("Quit to Desktop?##quit", &quit_prompt_req, "Quit " GAME_TITLE " and go to the desktop?", in_game,
                       "Quit now", "Cancel", scale) == 1) {
        settings_save();
        SDL_Event q = {};
        q.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&q);
    }
}

void controls_bindings_rows(void);  // runtime/ui/controls_ui.c: the remapping editor as option rows
static void tab_about(void) {  // (the Controls tab)
    float scale = igGetFontSize() / 34;
    opt_flat_begin("##controls", scale);
    opt_section("Button Prompts");
    {
        static const char *const fam[] = {"Nintendo", "Xbox", "PlayStation"};
        char autol[64];
        snprintf(autol, sizeof autol, "Auto (%s)", fam[input_pad_family() % 3]);
        const char *names[] = {autol, "Nintendo", "Xbox", "PlayStation"};
        opt_choice("Button Icons", &g_settings.prompts, names, 4, 0,
                   "Whose button names and icons the menu and the game show. Auto follows the connected controller.", true);
    }
    opt_section("System Language");
    {
        // what the game is told the console's language is: its text follows this (runtime/settings.c)
        char autol[64];
        snprintf(autol, sizeof autol, "Auto (%s)", settings_language_name());
        const char *names[N_LANGUAGES];
        names[0] = autol;
        for (int i = 1; i < N_LANGUAGES; i++) names[i] = g_language_names[i];
        if (opt_choice("Language (restart)", &g_settings.language, names, N_LANGUAGES, 0,
                       "The game's text language, as its own system setting. Applies when the game restarts "
                       "(Quit > Restart the Game).", true)) {
            settings_save();
            restart_changed_mark(RESTART_LANGUAGE);
        }
    }
    opt_section("Controllers");
    if (opt_toggle("Controllers Start at Player 2", &g_settings.pads_from_p2, false,
                   "The keyboard and the on-screen controls are player 1, and every controller is the next player "
                   "(player 2, 3, 4), for local multiplayer with a keyboard and one controller. Off: the first "
                   "controller is player 1's too.")) {
        settings_save();
        input_pads_reassign();
    }
    controls_bindings_rows();
    opt_section("Menu Shortcuts");
    opt_action("Open or Close the Menu", "F1, Esc", "F1, Esc, the controller's Home / Xbox / PlayStation button, or Back + Start.", true);
    opt_action("Switch Tabs", "L / R", "L / R on a controller, Q / E on the keyboard.", true);
    opt_action("Back", "B", "B on a controller, Esc on the keyboard.", true);
    opt_action("Fullscreen", "F11", "F11 or Alt+Enter.", true);
    opt_end();
}


// Reboot (runtime/main.c): the game is gone - menu and name dialog close; the ImGui context stays.
RT_ON_RESET(menu_reset) {
    set_open(false);
    g_kbd = 0;
    osk_cancel();
    pthread_mutex_lock(&q_lock);
    q_count = 0;
    pthread_mutex_unlock(&q_lock);
}

// (the name field's character filter: no control characters)
static int kbd_char_filter(ImGuiInputTextCallbackData *d) { return d->EventChar < 0x20 ? 1 : 0; }

// The Course ID / Maker ID box (issue #17): capital letters, only what an ID holds, the dashes put in for you. A typed or
// pasted dash is dropped (the box has put it there already, so it lands in place). Edits rebuild the text from its ID
// characters, which also trims a paste to nine.
static int kbd_id_callback(ImGuiInputTextCallbackData *d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        int c = (int)d->EventChar;
        if (c >= 'a' && c <= 'z') d->EventChar = (ImWchar)(c - 32);
        return osk_id_char(c) ? 0 : 1;
    }
    if (d->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
        char dashed[16];
        osk_id_dashed(d->Buf, dashed);
        if (strcmp(dashed, d->Buf)) {
            ImGuiInputTextCallbackData_DeleteChars(d, 0, d->BufTextLen);
            ImGuiInputTextCallbackData_InsertChars(d, 0, dashed, NULL);
        }
        d->CursorPos = d->SelectionStart = d->SelectionEnd = d->BufTextLen;
    }
    return 0;
}

// The keyboard box's hints under the field: keycaps as the menu's footer (Enter / Esc, or the controller's names) and the count
static void kbd_hints(int chars) {
    const char *ok, *back, *start;
    ui_hint_names(false, &ok, &back, &start);
    const char *keys[] = {ok, back}, *what[] = {"Confirm", "Cancel"};
    char count[32];
    snprintf(count, sizeof count, "%d / %d", chars, g_kbd_max);
    ui_hint_row(keys, what, 2, count);
}

static void keyboard_draw(ImGuiIO *io, float scale) {
    ImDrawList_AddRectFilled(igGetBackgroundDrawList_Nil(), IM_VEC2(0, 0), io->DisplaySize, IM_COL32(0, 0, 0, 120), 0, 0);
    float w = fminf(io->DisplaySize.x * 0.9f, 1100 * scale);
    igSetNextWindowPos(IM_VEC2(io->DisplaySize.x * 0.5f, io->DisplaySize.y * 0.5f), ImGuiCond_Always, IM_VEC2(0.5f, 0.5f));
    igSetNextWindowSize(IM_VEC2(w, 0), ImGuiCond_Always);
    igBegin("##kbd", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize);
    igPushFont(g_font_big);
    igPushStyleColor_Vec4(ImGuiCol_Text, accent_vec());
    // (no text from the game: a name entry - Link's, a course's - or a longer text, a Course World comment)
    igTextWrapped("%s", g_kbd_title[0] ? g_kbd_title : g_kbd_max <= 32 ? "Enter your name" : "Enter text");
    igPopStyleColor(1);
    igPopFont();
    static bool was_open, was_pad;
    bool pad = atomic_load(&g_last_pad), appearing = !was_open || pad != was_pad;  // (switching device: focus the new one)
    was_open = true, was_pad = pad;
    if (!pad && g_kbd_id) {  // an ID: capitals, its characters only, dashes shown (the text itself is the plain nine)
        static char view[16];
        if (appearing) osk_id_dashed(g_kbd_text, view);
        if (appearing) igSetKeyboardFocusHere(0);
        igSetNextItemWidth(-1);
        igPushFont(g_font_big);
        igInputText("##name", view, sizeof view, ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CallbackEdit,
                    kbd_id_callback, NULL);
        igPopFont();
        osk_id_compact(view, g_kbd_text);
        kbd_hints((int)strlen(g_kbd_text));
    } else if (!pad) {  // keyboard: type straight into the field
        if (appearing) igSetKeyboardFocusHere(0);
        igSetNextItemWidth(-1);
        igPushFont(g_font_big);
        igInputText("##name", g_kbd_text, (size_t)(g_kbd_max * 4 + 1 < (int)sizeof g_kbd_text ? g_kbd_max * 4 + 1 : sizeof g_kbd_text),
                    ImGuiInputTextFlags_CallbackCharFilter, kbd_char_filter, NULL);
        igPopFont();
        // (the game's limit is in characters: cut at the character boundary after the last one, never inside one)
        int chars = 0;
        for (char *p = g_kbd_text; *p; p++) {
            if (((unsigned char)*p & 0xc0) == 0x80) continue;
            if (chars++ == g_kbd_max) { *p = 0; chars--; break; }
        }
        kbd_hints(chars);
    } else {  // controller: on-screen keyboard (osk.mm, shared with our menus' text fields)
        osk_set_id_mode(g_kbd_id);  // (an ID: the keys an ID can't hold are greyed out)
        int r = osk_keys(g_kbd_text, sizeof g_kbd_text, g_kbd_max, appearing, scale, g_font_big, true);
        osk_set_id_mode(false);
        if (r) g_kbd = r > 0 ? 2 : 3;  // (Start / + confirms in menu_handle_event too)
    }
    igEnd();
    if (atomic_load(&g_kbd) != 1) was_open = false;
}

// (tests: games/smm2/mods/edit_ctl.c "kbd <text>") answer the open keyboard dialog with text
void menu_kbd_test_answer(const char *text) {
    if (atomic_load(&g_kbd) != 1) return;
    snprintf(g_kbd_text, sizeof g_kbd_text, "%s", text);
    g_kbd = 2;
}

// Game thread (runtime/hle/swkbd.c): show the dialog, re-present the last frame until it's answered.
// Returns 1 confirmed (out = text, UTF-8), 0 cancelled.
// The dialog's title for the next menu_keyboard_run (UTF-8; NULL or "": a name entry's or a text's own)
void menu_keyboard_title(const char *title) { snprintf(g_kbd_title, sizeof g_kbd_title, "%s", title ? title : ""); }

int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    extern void nvn_represent(void);  // runtime/nvn/objects.c
    snprintf(g_kbd_text, sizeof g_kbd_text, "%s", initial ? initial : "");
    g_kbd_max = max_len > 0 && max_len < (int)sizeof g_kbd_text / 4 ? max_len : 8;
    g_kbd_id = g_kbd_max == 9;  // (the Course ID / Maker ID search: the one prompt of nine characters)
    if (g_kbd_id) { char c[10]; osk_id_compact(g_kbd_text, c); snprintf(g_kbd_text, sizeof g_kbd_text, "%s", c); }
    input_reset();
    g_kbd = 1;
    // (test) LARECOMP_KBD_AUTO=<text>: answer at once
    if (getenv("LARECOMP_KBD_AUTO")) { snprintf(g_kbd_text, sizeof g_kbd_text, "%s", getenv("LARECOMP_KBD_AUTO")); g_kbd = 2; }
    while (atomic_load(&g_kbd) == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot cancels)
        uint64_t t0 = SDL_GetTicks();
        nvn_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    bool ok = atomic_load(&g_kbd) == 2 && g_kbd_text[0];
    if (ok) snprintf(out, cap, "%s", g_kbd_text);
    g_kbd = 0;
    g_kbd_title[0] = 0;
    input_reset();
    return ok;
}

// The tab bar and its pages, into the current window: the in-game menu, and the start screen's Settings (in_game
// false: nothing of the game is loaded yet - the tabs that need it are greyed out; prelaunch: the start screen's own
// first tab, game files and fonts, drawn by launcher.mm).
// The footer: what the buttons do, with the Button Icons setting's names for them (issue #11). Keycaps in the theme's
// colours, drawn along the bottom of the page area (the page's child leaves the room).
static void footer_draw(ImVec2 at, float w, float h, bool popup) {
    int fam = g_settings.prompts ? (g_settings.prompts - 1) % 3 : input_pad_family() % 3;  // 0 Nintendo, 1 Xbox, 2 PlayStation
    bool pad = atomic_load(&g_last_pad);
    typedef struct { const char *key, *what; } Hint;
    Hint items[3];
    int n = 0;
    items[n++] = (Hint){pad ? (fam == 2 ? "Cross" : "A") : "Enter", popup ? "Choose" : "Select"};
    items[n++] = (Hint){pad ? (fam == 2 ? "Circle" : "B") : "Esc", popup ? "Cancel" : "Back"};
    if (!popup) items[n++] = (Hint){pad ? (fam == 0 ? "L / R" : fam == 1 ? "LB / RB" : "L1 / R1") : "Q / E", "Switch Tab"};
    ImDrawList *dl = igGetWindowDrawList();
    const bool nes = nes_theme();
    float fs = igGetFontSize(), ch = fs * 1.45f, cy = at.y + h * 0.5f + fs * 0.1f, x = at.x + fs * 0.4f;
    ImDrawList_AddLine(dl, IM_VEC2(at.x, at.y + fs * 0.1f), IM_VEC2(at.x + w, at.y + fs * 0.1f), menu_accent(70), 1.0f);
    // the time played, all sessions, at the right (issue #15): on every page; the hints give way to it on a narrow window
    char played[48], played_label[64];
    playtime_format(playtime_total(), played, sizeof played);
    snprintf(played_label, sizeof played_label, "Played  %s", played);
    ImVec2 pw;
    igCalcTextSize(&pw, played_label, NULL, false, -1.0f);
    ImDrawList_AddText_Vec2(dl, IM_VEC2(at.x + w - pw.x - fs * 0.1f, cy - pw.y * 0.5f), menu_muted(), played_label, NULL);
    // the time of day (issue #56), just left of the played time; the hints give way to it first
    float clock_w = 0;
    char now_label[24];
    if (g_settings.show_clock && clock_format(now_label, sizeof now_label)) {
        ImVec2 cw2;
        igCalcTextSize(&cw2, now_label, NULL, false, -1.0f);
        float cx = at.x + w - pw.x - fs * 0.1f - fs * 1.4f - cw2.x;
        if (cx > at.x + fs * 0.4f) {  // (a window too narrow for both: the clock is left out)
            ImDrawList_AddText_Vec2(dl, IM_VEC2(cx, cy - cw2.y * 0.5f), menu_muted(), now_label, NULL);
            clock_w = cw2.x + fs * 1.4f;
        }
    }
    const float room = w - pw.x - clock_w - fs * 1.5f;  // (what is left of the line for the hints)
    for (int i = 0; i < n; i++) {
        ImVec2 kw, ww;
        igCalcTextSize(&kw, items[i].key, NULL, false, -1.0f);
        igCalcTextSize(&ww, items[i].what, NULL, false, -1.0f);
        float cw = kw.x + fs * 0.9f;
        if (x + cw + fs * 0.4f + ww.x > at.x + room) break;  // (a narrow window: the rest is left out)
        ImVec2 a = IM_VEC2(x, cy - ch * 0.5f), b = IM_VEC2(x + cw, cy + ch * 0.5f);
        ImDrawList_AddRectFilled(dl, a, b, IM_COL32(255, 255, 255, 24), nes ? 0 : ch * 0.3f, 0);
        ImDrawList_AddRect(dl, a, b, menu_accent(200), nes ? 0 : ch * 0.3f, 0, nes ? 2.0f : 1.5f);
        ImDrawList_AddText_Vec2(dl, IM_VEC2(x + fs * 0.45f, cy - kw.y * 0.5f), menu_accent(255), items[i].key, NULL);
        x += cw + fs * 0.4f;
        ImDrawList_AddText_Vec2(dl, IM_VEC2(x, cy - ww.y * 0.5f), menu_muted(), items[i].what, NULL);
        x += ww.x + fs * 1.4f;
    }
}

static float g_tabs_w;  // the width the menu's body needs (measured last frame: the same whichever page is open)

// The Mii editor's page (the library and the editor come into it; the part icons are read from the player's firmware, tools/extract_mii_icons.py)
// The Miis are 88-byte CharInfo files in miis/ (miiNNN.charinfo); "Play as This Mii" copies one to mii.charinfo, which the game
// reads live (services/mii.c). Every change is saved to its file at once; when the Mii being edited is the one in use, the in-use
// file follows it. The values are the game's own numbers (the part icons come later, tools/extract_mii_icons.py).
#include <dirent.h>
#include <sys/stat.h>
#include "../services/mii_charinfo_file.h"
#include "../../third_party/stb/stb_image.h"
void mii_default_charinfo(uint8_t out[0x58], const char *nickname);  // services/mii.c

#define MII_DIR "miis"
#define MII_MAX 64
typedef struct { char file[32]; char name[48]; bool in_use; } MiiEntry;
static MiiEntry g_miis[MII_MAX];
static int g_mii_n, g_mii_open = -1;
static bool g_mii_scan = true, g_mii_ask_delete;
static uint8_t g_mii_cur[0x58];
static int g_mii_pick = -1;  // the field whose pictures are being chosen from (an index into k_mii_fields), else -1

uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);  // runtime/nvn/vulkan/imgui_glue.c
uint64_t vk_imgui_texture_mii(const void *rgba, int w, int h);   // (the Mii editor's preview: one streamed image)
// The pictures of a part (sysdata/miiicons/<name>.png, set up from the firmware), 78 x 66 each: all of a kind are put in one
// texture when first wanted (the UI has a small pool of textures, so one each would run out), 12 to a row. The texture, and
// the corner coordinates of picture idx (0 when there are none); fmt names the pictures (IconHair%03d), count how many.
#define MII_ICON_W 78
#define MII_ICON_H 66
#define MII_SHEET_COLS 12
static uint64_t mii_icon(const char *fmt, int count, int idx, ImVec2 *uv0, ImVec2 *uv1) {
    static struct { const char *fmt; uint64_t tid; int rows; } sheets[10];
    static int n;
    int k = 0;
    for (; k < n; k++)
        if (sheets[k].fmt == fmt) break;  // (the table's own strings: the same pointer)
    if (k == n) {
        if (n >= 10) return 0;
        sheets[n].fmt = fmt;
        int rows = (count + MII_SHEET_COLS - 1) / MII_SHEET_COLS, W = MII_SHEET_COLS * MII_ICON_W, H = rows * MII_ICON_H, loaded = 0;
        unsigned char *sheet = calloc((size_t)W * H, 4);
        static unsigned char buf[65536];  // (an icon is a few KB)
        for (int i = 0; sheet && i < count; i++) {
            char name[28], path[96];
            snprintf(name, sizeof name, fmt, i);
            snprintf(path, sizeof path, "sysdata/miiicons/%s.png", name);
            FILE *f = fopen(path, "rb");  // (stb is built without file reading)
            if (!f) continue;
            size_t len = fread(buf, 1, sizeof buf, f);
            fclose(f);
            int w, h, c;
            unsigned char *px = stbi_load_from_memory(buf, (int)len, &w, &h, &c, 4);
            if (!px) continue;
            if (w == MII_ICON_W && h == MII_ICON_H) {
                for (int y = 0; y < h; y++)
                    memcpy(sheet + ((size_t)((i / MII_SHEET_COLS) * MII_ICON_H + y) * W + (i % MII_SHEET_COLS) * MII_ICON_W) * 4,
                           px + (size_t)y * w * 4, (size_t)w * 4);
                loaded++;
            }
            stbi_image_free(px);
        }
        sheets[n].tid = loaded ? vk_imgui_texture_rgba(sheet, W, H) : 0;
        sheets[n].rows = rows;
        free(sheet);
        k = n++;
    }
    if (!sheets[k].tid || idx < 0 || idx >= count) return 0;
    float W = MII_SHEET_COLS * MII_ICON_W, H = (float)sheets[k].rows * MII_ICON_H;
    float x = (float)(idx % MII_SHEET_COLS) * MII_ICON_W, y = (float)(idx / MII_SHEET_COLS) * MII_ICON_H;
    *uv0 = IM_VEC2((x + 0.5f) / W, (y + 0.5f) / H);  // (half a texel in: the next picture does not bleed in)
    *uv1 = IM_VEC2((x + MII_ICON_W - 0.5f) / W, (y + MII_ICON_H - 0.5f) / H);
    return sheets[k].tid;
}

typedef struct { const char *sec, *label; uint8_t off, lo, hi; const char *icon; } MiiField;  // (sec: the section this row starts, else NULL; icon: the picture names of its values, if it has them)
static const MiiField k_mii_fields[] = {
    {"Basics", "Favorite Color", 0x27, 0, 11}, {NULL, "Height", 0x29, 0, 127}, {NULL, "Build", 0x2A, 0, 127},
    {"Face", "Face Shape", 0x2D, 0, 11, "IconFaceline%02d"}, {NULL, "Skin Color", 0x2E, 0, 9}, {NULL, "Wrinkles", 0x2F, 0, 11, "IconFaceT_line%02d"}, {NULL, "Makeup", 0x30, 0, 11, "IconFaceT_make%02d"},
    {"Hair", "Hair Style", 0x31, 0, 131, "IconHair%03d"}, {NULL, "Hair Color", 0x32, 0, 99}, {NULL, "Hair Flip", 0x33, 0, 1},
    {"Eyes", "Eye Type", 0x34, 0, 59, "IconEye%02d"}, {NULL, "Eye Color", 0x35, 0, 99}, {NULL, "Eye Size", 0x36, 0, 7}, {NULL, "Eye Stretch", 0x37, 0, 6},
    {NULL, "Eye Rotation", 0x38, 0, 7}, {NULL, "Eye Spacing", 0x39, 0, 12}, {NULL, "Eye Height", 0x3A, 0, 18},
    {"Eyebrows", "Eyebrow Type", 0x3B, 0, 23, "IconEyebrow%02d"}, {NULL, "Eyebrow Color", 0x3C, 0, 99}, {NULL, "Eyebrow Size", 0x3D, 0, 8},
    {NULL, "Eyebrow Stretch", 0x3E, 0, 6}, {NULL, "Eyebrow Rotation", 0x3F, 0, 11}, {NULL, "Eyebrow Spacing", 0x40, 0, 12},
    {NULL, "Eyebrow Height", 0x41, 3, 18},
    {"Nose", "Nose Type", 0x42, 0, 17, "IconNose%02d"}, {NULL, "Nose Size", 0x43, 0, 8}, {NULL, "Nose Height", 0x44, 0, 18},
    {"Mouth", "Mouth Type", 0x45, 0, 35, "IconMouth%02d"}, {NULL, "Mouth Color", 0x46, 0, 99}, {NULL, "Mouth Size", 0x47, 0, 8},
    {NULL, "Mouth Stretch", 0x48, 0, 6}, {NULL, "Mouth Height", 0x49, 0, 18},
    {"Facial Hair", "Beard Color", 0x4A, 0, 99}, {NULL, "Beard", 0x4B, 0, 5}, {NULL, "Mustache", 0x4C, 0, 5},
    {NULL, "Mustache Size", 0x4D, 0, 8}, {NULL, "Mustache Height", 0x4E, 0, 16},
    {"Glasses", "Glasses Type", 0x4F, 0, 19}, {NULL, "Glasses Color", 0x50, 0, 99}, {NULL, "Glasses Size", 0x51, 0, 7},
    {NULL, "Glasses Height", 0x52, 0, 20},
    {"Mole", "Mole Type", 0x53, 0, 1}, {NULL, "Mole Size", 0x54, 0, 8}, {NULL, "Mole X", 0x55, 0, 16}, {NULL, "Mole Y", 0x56, 0, 30},
};

static const char *mii_in_use_path(void) {
    const char *p = getenv("RECOMP_MII_CHARINFO");
    return p && *p ? p : "mii.charinfo";
}

static void mii_name_get(const uint8_t d[0x58], char *out, size_t n) {  // the nickname: UTF-16 -> UTF-8
    size_t o = 0;
    for (int i = 0; i < 10; i++) {
        unsigned c = d[16 + 2 * i] | d[17 + 2 * i] << 8;
        if (!c) break;
        if (c < 0x80 && o + 1 < n) out[o++] = (char)c;
        else if (c < 0x800 && o + 2 < n) out[o++] = (char)(0xC0 | c >> 6), out[o++] = (char)(0x80 | (c & 0x3F));
        else if (c >= 0x800 && o + 3 < n) out[o++] = (char)(0xE0 | c >> 12), out[o++] = (char)(0x80 | (c >> 6 & 0x3F)), out[o++] = (char)(0x80 | (c & 0x3F));
    }
    out[o] = 0;
}

static void mii_name_set(uint8_t d[0x58], const char *s) {  // UTF-8 -> UTF-16, up to 10 characters
    memset(d + 16, 0, 22);
    const unsigned char *p = (const unsigned char *)s;
    for (int n = 0; *p && n < 10; n++) {
        unsigned cp = *p++;
        if (cp >= 0xC0) {
            int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : 1;
            cp &= 0x3F >> extra;
            while (extra-- && (*p & 0xC0) == 0x80) cp = cp << 6 | (*p++ & 0x3F);
            if (cp > 0xFFFF) cp = '?';
        }
        d[16 + 2 * n] = (uint8_t)cp, d[17 + 2 * n] = (uint8_t)(cp >> 8);
    }
}

static bool mii_write(const char *path, const uint8_t d[0x58]) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(d, 1, 0x58, f) == 0x58;
    return fclose(f) == 0 && ok;
}

static void mii_path(char *out, size_t n, int i) { snprintf(out, n, MII_DIR "/%s", g_miis[i].file); }

static void mii_rescan(void) {
    uint8_t use[0x58];
    bool have = mii_read_charinfo_file(mii_in_use_path(), use) == 1;
    g_mii_n = 0;
    DIR *d = opendir(MII_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && g_mii_n < MII_MAX) {
        size_t len = strlen(e->d_name);
        if (len < 10 || len >= sizeof g_miis[0].file || strcmp(e->d_name + len - 9, ".charinfo")) continue;
        char path[64];
        uint8_t data[0x58];
        snprintf(path, sizeof path, MII_DIR "/%s", e->d_name);
        if (mii_read_charinfo_file(path, data) != 1) continue;
        MiiEntry *m = &g_miis[g_mii_n++];
        snprintf(m->file, sizeof m->file, "%s", e->d_name);
        mii_name_get(data, m->name, sizeof m->name);
        if (!*m->name) snprintf(m->name, sizeof m->name, "(no name)");
        m->in_use = have && !memcmp(use, data, 0x58);
    }
    closedir(d);
    for (int i = 1; i < g_mii_n; i++)  // (by file name = the order they were made; few entries)
        for (int j = i; j > 0 && strcmp(g_miis[j - 1].file, g_miis[j].file) > 0; j--) {
            MiiEntry t = g_miis[j];
            g_miis[j] = g_miis[j - 1], g_miis[j - 1] = t;
        }
}

// Saves a new Mii to the library under the first free name; the index of its entry, or -1.
static int mii_add(const uint8_t data[0x58]) {
    mkdir(MII_DIR, 0755);
    for (int k = 1; k < 1000; k++) {
        char path[64];
        snprintf(path, sizeof path, MII_DIR "/mii%03d.charinfo", k);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); continue; }
        if (!mii_write(path, data)) return -1;
        mii_rescan();
        char want[32];
        snprintf(want, sizeof want, "mii%03d.charinfo", k);
        for (int i = 0; i < g_mii_n; i++) if (!strcmp(g_miis[i].file, want)) return i;
        return -1;
    }
    return -1;
}

static void mii_fresh_id(uint8_t d[0x58]) {  // a new create id (byte 8 keeps its "valid" bits 10xxxxxx)
    static bool seeded;
    if (!seeded) srand((unsigned)time(NULL)), seeded = true;
    for (int i = 0; i < 16; i++) d[i] = (uint8_t)(rand() >> 3);
    d[8] = (uint8_t)((d[8] & 0x3F) | 0x80);
}

// A saved Mii's picture: miis/<file>.jpg, kept when it was imported from MiiCharacters.com. 96 pixels at most, one texture each
// (the descriptor pool is small: only the first MII_PHOTOS Miis shown get one).
static void mii_photo_path(char *out, size_t n, const char *file) {
    char stem[32];
    snprintf(stem, sizeof stem, "%s", file);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    snprintf(out, n, MII_DIR "/%s.jpg", stem);
}
#define MII_PHOTOS 12
static struct { char file[32]; uint64_t tid; bool tried; } g_photo[MII_PHOTOS];
static void mii_photo_forget(const char *file) {  // (the file changed or went: looked at again next time)
    for (int k = 0; k < MII_PHOTOS; k++)
        if (!strcmp(g_photo[k].file, file)) g_photo[k].tried = false, g_photo[k].tid = 0;
}
static uint64_t mii_photo(const char *file) {
    int k = 0;
    while (k < MII_PHOTOS && g_photo[k].file[0] && strcmp(g_photo[k].file, file)) k++;
    if (k == MII_PHOTOS) return 0;
    if (!g_photo[k].file[0]) snprintf(g_photo[k].file, sizeof g_photo[k].file, "%s", file);
    if (g_photo[k].tried) return g_photo[k].tid;
    g_photo[k].tried = true;
    char path[64];
    mii_photo_path(path, sizeof path, file);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = len > 0 && len < 4000000 ? malloc((size_t)len) : NULL;
    if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) free(buf), buf = NULL;
    fclose(f);
    if (!buf) return 0;
    int w, h, c;
    unsigned char *px = stbi_load_from_memory(buf, (int)len, &w, &h, &c, 4);
    free(buf);
    if (!px) return 0;
    int fct = w / 96 > h / 96 ? w / 96 : h / 96;  // (a box average down to about 96)
    if (fct < 1) fct = 1;
    int w2 = w / fct, h2 = h / fct;
    unsigned char *small = malloc((size_t)w2 * h2 * 4);
    if (small) {
        for (int y = 0; y < h2; y++)
            for (int x = 0; x < w2; x++)
                for (int ch = 0; ch < 4; ch++) {
                    int sum = 0;
                    for (int dy = 0; dy < fct; dy++)
                        for (int dx = 0; dx < fct; dx++) sum += px[(((y * fct + dy) * w) + x * fct + dx) * 4 + ch];
                    small[((size_t)y * w2 + x) * 4 + ch] = (unsigned char)(sum / (fct * fct));
                }
        g_photo[k].tid = vk_imgui_texture_rgba(small, w2, h2);
        free(small);
    }
    stbi_image_free(px);
    return g_photo[k].tid;
}
static void mii_photo_draw(uint64_t tid, ImVec2 a, float size) {
    ImDrawList_AddImage(igGetWindowDrawList(), (ImTextureID)tid, a, IM_VEC2(a.x + size, a.y + size), IM_VEC2(0, 0), IM_VEC2(1, 1),
                        IM_COL32(255, 255, 255, 255));
}

static void tab_mii(void) {
    if (g_mii_scan) mii_rescan(), g_mii_scan = false;
    if (g_mii_open >= g_mii_n) g_mii_open = g_mii_pick = -1;
    if (g_mii_open < 0) g_mii_pick = -1;
    static bool tested;  // test hook: LARECOMP_MII_TEST=<n> opens a new Mii's editor, with the pictures of field n (-1: none) open
    if (!tested && g_mii_open < 0 && getenv("LARECOMP_MII_TEST")) {
        tested = true;
        uint8_t d[0x58];
        mii_default_charinfo(d, "Test");
        mii_fresh_id(d);
        int at = mii_add(d);
        if (at >= 0) g_mii_open = at, memcpy(g_mii_cur, d, 0x58), g_mii_pick = atoi(getenv("LARECOMP_MII_TEST"));
    }
    static char imp_link[160], imp_msg[160];  // Import from a Link: the pasted text, and what came of it
    static bool imp_busy;
    static bool imp_tested;  // test hook: LARECOMP_MII_IMPORT=<link> imports it as soon as this tab shows
    if (!imp_tested && getenv("LARECOMP_MII_IMPORT")) {
        imp_tested = true;
        char why[160] = "";
        if (mii_import_start(getenv("LARECOMP_MII_IMPORT"), why, sizeof why)) imp_busy = true;
        else snprintf(imp_msg, sizeof imp_msg, "%s", why);
    }
    if (imp_busy) {
        uint8_t d[0x58], *img = NULL;
        size_t img_len = 0;
        char nm[48] = "", why[160] = "";
        MiiImportState s = mii_import_poll(d, nm, why, &img, &img_len);
        if (s == MII_IMP_ERROR) imp_busy = false, snprintf(imp_msg, sizeof imp_msg, "%s", why);
        if (s == MII_IMP_DONE) {
            imp_busy = false;
            mii_name_set(d, nm[0] ? nm : "Mii");
            mii_fresh_id(d);
            int at = g_mii_n < MII_MAX ? mii_add(d) : -1;
            if (at >= 0) {
                g_mii_open = at, memcpy(g_mii_cur, d, 0x58), g_mii_ask_delete = false, g_mii_pick = -1, imp_link[0] = 0, imp_msg[0] = 0;
                if (img) {  // (its picture, kept beside it)
                    char pp[64];
                    mii_photo_path(pp, sizeof pp, g_miis[at].file);
                    FILE *pf = fopen(pp, "wb");
                    if (pf) fwrite(img, 1, img_len, pf), fclose(pf);
                    mii_photo_forget(g_miis[at].file);
                }
            } else snprintf(imp_msg, sizeof imp_msg, "The library is full (%d Miis).", MII_MAX);
        }
        free(img);
    }
    if (g_mii_open < 0) {
        opt_flat_begin("##mii", igGetFontSize() / 34);
        opt_no_reset();
        if (opt_section_link("Import from MiiCharacters.com", "Open MiiCharacters.com in your browser, to find a Mii and copy its link"))
            SDL_OpenURL("https://www.miicharacters.com/");
        if (opt_text("Mii Link", imp_link, sizeof imp_link, "miicharacters.com/mii/23844",
                     "Paste the link of a Mii from miicharacters.com: it is added to the library, read from the page's QR picture.")) {
            char why[160] = "";
            imp_msg[0] = 0;
            if (!imp_link[0]) snprintf(imp_msg, sizeof imp_msg, "Paste a link first.");
            else if (mii_import_start(imp_link, why, sizeof why)) imp_busy = true;
            else snprintf(imp_msg, sizeof imp_msg, "%s", why);
        }
        if (imp_busy) opt_action("Reading the page...", "", "Fetching the Mii from miicharacters.com.", false);
        else if (imp_msg[0]) opt_action(imp_msg, "", "", false);
        opt_section("Your Miis");
        for (int i = 0; i < g_mii_n; i++) {
            igPushID_Int(i);
            uint64_t ph = mii_photo(g_miis[i].file);
            float sz = 120 * igGetFontSize() / 34;
            if (ph) opt_next_row_height(sz + 8);  // (a row with a picture is tall enough for it: its box goes round it)
            if (opt_action(g_miis[i].name, g_miis[i].in_use ? "Playing as" : "", "Edit this Mii, or choose it to play as.", false)) {
                char path[64];
                mii_path(path, sizeof path, i);
                if (mii_read_charinfo_file(path, g_mii_cur) == 1) g_mii_open = i, g_mii_ask_delete = false;
            }
            ImVec2 ra, rb;
            igGetItemRectMin(&ra);
            igGetItemRectMax(&rb);
            if (ph) mii_photo_draw(ph, IM_VEC2(rb.x - sz - 8, (ra.y + rb.y) * 0.5f - sz * 0.5f), sz);  // (at the row's right edge)
            igPopID();
        }
        if (!g_mii_n)
            opt_action("No Miis saved yet", "", "Paste a miicharacters.com link above, or make a new one.", false);
        uint8_t use[0x58];
        bool have = mii_read_charinfo_file(mii_in_use_path(), use) == 1, listed = false;
        for (int i = 0; i < g_mii_n; i++) listed |= g_miis[i].in_use;
        if (g_mii_n < MII_MAX) {
            opt_section("Add");
            if (opt_action("New Mii", "", "A new Mii, to change as you like.", true)) {
                uint8_t d[0x58];
                mii_default_charinfo(d, "New Mii");
                mii_fresh_id(d);
                int at = mii_add(d);
                if (at >= 0) g_mii_open = at, memcpy(g_mii_cur, d, 0x58), g_mii_ask_delete = false;
            }
            if (have && !listed &&
                opt_action("Add the Mii in Use", "", "Saves the Mii you are playing as now (mii.charinfo) to the library.", true))
                mii_add(use);
        }
        opt_end();
        return;
    }
    MiiEntry *m = &g_miis[g_mii_open];
    char path[64], name[48];
    mii_path(path, sizeof path, g_mii_open);
    uint8_t old[0x58];
    memcpy(old, g_mii_cur, 0x58);
    bool changed = false;
    if (g_mii_pick >= 0) {  // choosing a part from its pictures
        const MiiField *f = &k_mii_fields[g_mii_pick];
        igText("%s", f->label);
        igSameLine(0, igGetFontSize());
        if (igButton("Done", IM_VEC2(0, 0))) g_mii_pick = -1;
        igSeparator();
        igBeginChild_Str("##miipick", IM_VEC2(0, 0), 0, 0);
        float w = igGetFontSize() * 4.4f, h = w * 66 / 78, gap = igGetStyle()->ItemSpacing.x;
        ImVec2 avail;
        igGetContentRegionAvail(&avail);
        int cols = (int)((avail.x + gap) / (w + gap));
        if (cols < 1) cols = 1;
        static int scrolled = -2;  // (the pick the scroll was last set for: the chosen picture comes into view when it opens)
        ImVec2 cur_at = {0, 0};
        for (int i = f->lo; i <= f->hi; i++) {
            if ((i - f->lo) % cols) igSameLine(0, -1);
            igPushID_Int(i);
            ImVec2 uv0, uv1;
            uint64_t tex = mii_icon(f->icon, f->hi + 1, i, &uv0, &uv1);
            bool cur = g_mii_cur[f->off] == i;
            ImVec4 bg = {0, 0, 0, 0};
            igPushStyleColor_Vec4(ImGuiCol_Button, cur ? (ImVec4){0.95f, 0.74f, 0.16f, 1.0f} : (ImVec4){0.80f, 0.82f, 0.86f, 1.0f});
            if (cur) igGetCursorPos(&cur_at);
            bool hit = tex ? igImageButton("##i", (ImTextureID)tex, IM_VEC2(w, h), uv0, uv1, bg, (ImVec4){1, 1, 1, 1})
                           : igButton("...", IM_VEC2(w + igGetStyle()->FramePadding.x * 2, h + igGetStyle()->FramePadding.y * 2));
            igPopStyleColor(1);
            if (hit) g_mii_cur[f->off] = (uint8_t)i, changed = true;
            if (igIsItemHovered(0)) igSetTooltip("%d", i);
            igPopID();
        }
        static int settle;  // (the first frames lay the grid out in a window not yet its width: the scroll is set once it has settled)
        if (scrolled != g_mii_pick && ++settle >= 3) igSetScrollY_Float(cur_at.y - avail.y * 0.35f), scrolled = g_mii_pick, settle = 0;
        igEndChild();
        if (g_mii_pick < 0) scrolled = -2;
    } else {
    opt_flat_begin("##mii", igGetFontSize() / 34);
    opt_no_reset();
    if (opt_action("Back to the Library", "", "Your changes are already saved.", true)) {
        g_mii_open = -1, g_mii_scan = true;
        opt_end();
        return;
    }
    {  // the preview: the Mii as a public renderer draws it, again a moment after each change (its saved picture meanwhile)
        mii_render_want(g_mii_cur);
        int rw = 0, rh = 0, rstate = 0;
        unsigned rseq = 0;
        char rwhy[96];
        const uint8_t *rp = mii_render_get(&rw, &rh, &rseq, &rstate, rwhy);
        static unsigned shown_seq;
        static uint64_t render_tid;
        if (rp && (rseq != shown_seq || !render_tid)) render_tid = vk_imgui_texture_mii(rp, rw, rh), shown_seq = rseq;
        uint64_t tid = rp ? render_tid : mii_photo(g_miis[g_mii_open].file);
        // at the page's right side and not scrolling with the fields (drawn at the window's own position, not the cursor's)
        ImVec2 wp, ws;
        igGetWindowPos(&wp);
        igGetWindowSize(&ws);
        float fs = igGetFontSize(), sz = fminf(320 * fs / 34, ws.x * 0.5f - 40 * fs / 34);
        ImVec2 a = IM_VEC2(wp.x + ws.x - sz - 28 * fs / 34, wp.y + 16 * fs / 34);
        ImDrawList *dl = igGetWindowDrawList();
        if (tid) mii_photo_draw(tid, a, sz);
        else ImDrawList_AddRectFilled(dl, a, IM_VEC2(a.x + sz, a.y + sz), IM_COL32(255, 255, 255, 14), 6, 0);
        const char *note = rstate == 1 ? "Drawing the preview..." : rstate == 2 ? rwhy : NULL;
        char nb[128];
        if (rstate == 2) snprintf(nb, sizeof nb, "No preview: %s", rwhy), note = nb;
        if (note) ImDrawList_AddText_Vec2(dl, IM_VEC2(a.x, a.y + sz + 6 * fs / 34), menu_muted(), note, NULL);
    }
    opt_section("Mii");
    if (!opt_text_editing("Name")) mii_name_get(g_mii_cur, name, sizeof name);
    else snprintf(name, sizeof name, "%s", m->name);
    if (opt_text("Name", name, 11 * 4, "Name", "Up to 10 characters.")) {
        mii_name_set(g_mii_cur, name);
        changed = true;
    }
    if (opt_action("Play as This Mii", m->in_use ? "Playing as" : "", "Makes this the Mii the game shows for you.", true)) {
        if (mii_write(mii_in_use_path(), g_mii_cur)) g_mii_scan = true;
    }
    int gender = g_mii_cur[0x28];
    static const char *const genders[] = {"Male", "Female"};
    if (opt_choice("Gender", &gender, genders, 2, 0, "The Mii's gender.", true)) g_mii_cur[0x28] = (uint8_t)gender, changed = true;
    for (size_t i = 0; i < sizeof k_mii_fields / sizeof *k_mii_fields; i++) {
        const MiiField *f = &k_mii_fields[i];
        if (f->sec) opt_section(f->sec);
        int v = g_mii_cur[f->off];
        ImVec2 uv0, uv1;
        uint64_t tex = f->icon ? mii_icon(f->icon, f->hi + 1, v, &uv0, &uv1) : 0;
        if (f->icon && tex) {  // a part with pictures: A / click opens them all; its own is shown beside the value
            char val[16];
            snprintf(val, sizeof val, "%d", v);
            bool open = opt_action(f->label, val, "Choose from the pictures.", true);
            ImVec2 lo, hi, tw;
            igGetItemRectMin(&lo);
            igGetItemRectMax(&hi);
            igCalcTextSize(&tw, val, NULL, false, -1.0f);
            float ih = (hi.y - lo.y) - 4, iw = ih * MII_ICON_W / MII_ICON_H, x = hi.x - tw.x - igGetFontSize() * 2.6f - iw;
            ImDrawList_AddImage(igGetWindowDrawList(), (ImTextureID)tex, IM_VEC2(x, lo.y + 2), IM_VEC2(x + iw, lo.y + 2 + ih), uv0, uv1,
                                IM_COL32(255, 255, 255, 255));
            if (open) g_mii_pick = (int)i;
        } else if (opt_int(f->label, &v, f->lo, f->hi, 1, f->lo, "%d", "The game's number for this part.", NULL)) {
            g_mii_cur[f->off] = (uint8_t)v, changed = true;
        }
    }
    opt_section("Manage");
    if (opt_action("Duplicate", "", "A copy of this Mii in the library.", true)) {
        uint8_t d[0x58];
        memcpy(d, g_mii_cur, 0x58);
        mii_fresh_id(d);
        char from[64];
        mii_photo_path(from, sizeof from, m->file);
        int at = mii_add(d);
        if (at >= 0) {
            g_mii_open = at, memcpy(g_mii_cur, d, 0x58);
            FILE *src = fopen(from, "rb");  // (the picture goes with the copy)
            if (src) {
                char to[64];
                mii_photo_path(to, sizeof to, g_miis[at].file);
                FILE *dst = fopen(to, "wb");
                char chunk[4096];
                size_t n;
                while (dst && (n = fread(chunk, 1, sizeof chunk, src)) > 0) fwrite(chunk, 1, n, dst);
                if (dst) fclose(dst);
                fclose(src);
                mii_photo_forget(g_miis[at].file);
            }
        }
        opt_end();
        return;
    }
    if (!g_mii_ask_delete) {
        if (opt_action("Delete", "", "Removes this Mii from the library.", true)) g_mii_ask_delete = true;
    } else {
        if (opt_action("Yes, Delete This Mii", "", "It cannot be brought back.", true)) {
            remove(path);
            char pp[64];
            mii_photo_path(pp, sizeof pp, m->file);
            remove(pp), mii_photo_forget(m->file);
            g_mii_open = -1, g_mii_scan = true, g_mii_ask_delete = false;
            opt_end();
            return;
        }
        if (opt_action("Cancel", "", "Keeps the Mii.", true)) g_mii_ask_delete = false;
    }
    opt_end();
    }
    if (changed && mii_charinfo_valid(g_mii_cur)) {
        bool was_in_use = false;
        uint8_t use[0x58];
        if (mii_read_charinfo_file(mii_in_use_path(), use) == 1) was_in_use = !memcmp(use, old, 0x58);
        mii_write(path, g_mii_cur);
        if (was_in_use) mii_write(mii_in_use_path(), g_mii_cur);
        mii_name_get(g_mii_cur, m->name, sizeof m->name);
        if (!*m->name) snprintf(m->name, sizeof m->name, "(no name)");
        g_mii_scan = true;  // (the list's "in use" marks)
    } else if (changed) {
        memcpy(g_mii_cur, old, 0x58);  // (not a Mii the game accepts: back to what it was)
    }
}

// The pages are in groups: the groups are big buttons down the left, and a group with more than one page has a row of tabs for them at
// the top of the frame on the right. A page's id is what g_tab, g_tab_request and the test hooks hold (-1 = Prelaunch); a group's button
// has the id GROUP_ID + its number here, for the controller's cursor.
typedef struct { const char *name; int n; int pages[4]; } TabGroup;
enum { GROUP_ID = 100 };
static const char *const k_page_names[] = {"Video", "Audio", "Mods", "Online", "Stats", "General", "Achievements", "Search", "Controls", "Quit", "Mii Editor"};
static const TabGroup k_groups[] = {
    {"Prelaunch", 1, {-1}},
    {"Settings", 4, {5, 8, 0, 1}},  // General, Controls, Video, Audio
    {"Mods", 1, {2}},
    {"OpenCourse World", 2, {3, 7}},  // Online, Search
    {"Player", 3, {10, 4, 6}},        // Mii Editor, Stats, Achievements
    {"Quit", 1, {9}},
};
enum { N_GROUPS = sizeof k_groups / sizeof k_groups[0] };

static const char *page_name(int id) { return id == -1 ? "Prelaunch" : k_page_names[id]; }
static int group_of_page(int id) {
    for (int g = 0; g < N_GROUPS; g++)
        for (int i = 0; i < k_groups[g].n; i++)
            if (k_groups[g].pages[i] == id) return g;
    return 0;
}
static int s_focused_tab = -99;  // the tab or button the controller's cursor is on (a page's id, or GROUP_ID + a group's number; from the frame before; -99: none)

static float tab_width(const char *name) {
    char lbl[64];
    ImVec2 ts;
    snprintf(lbl, sizeof lbl, "  %s  ", name);
    igCalcTextSize(&ts, lbl, NULL, false, -1.0f);
    return ts.x + igGetStyle()->FramePadding.x * 2 + igGetStyle()->ItemInnerSpacing.x;
}

// The controller's cursor landing on an item (g_focus_tab: its id, or -100 for "the selected one", which `selected` says): the item
// just drawn takes the focus; a few frames, as the page's child window appearing makes ImGui pick its first widget once it's laid out
static void focus_item(int id, bool selected) {
    if (!(g_focus_tab == id || (g_focus_tab == -100 && selected))) return;
    igSetFocusID(igGetItemID(), igGetCurrentWindow());  // (SetKeyboardFocusHere skips tabs: no tab stop)
    igSetNavCursorVisible(true);
    if (++g_focus_frames >= 4) g_focus_tab = -99, g_focus_frames = 0;
    else g_focus_tab = id;
}

// One tab of a group's row: the gold focus bar instead of ImGui's outline (it sits 4 px outside the tab and the tab bar's clipping could
// cut its top off). Returns whether its page is the one showing.
static bool nav_tab(const char *name, int id, bool request, float scale) {
    char lbl[64];
    snprintf(lbl, sizeof lbl, "  %s  ", name);
    // (s_focused_tab: from the frame before - colours go in before the tab is drawn)
    bool lit = s_focused_tab == id && igGetCurrentContext()->NavCursorVisible;
    igPushStyleColor_Vec4(ImGuiCol_NavCursor, IM_VEC4(0, 0, 0, 0));
    if (lit) {
        igPushStyleColor_Vec4(ImGuiCol_Tab, theme_palette()->tab_alt);
        igPushStyleColor_Vec4(ImGuiCol_TabHovered, theme_palette()->tab_alt);
    }
    bool open = igBeginTabItem(lbl, NULL, request ? ImGuiTabItemFlags_SetSelected : 0);
    igPopStyleColor(lit ? 3 : 1);
    focus_item(id, false);
    if (lit) {
        ImVec2 a, b;
        igGetItemRectMin(&a), igGetItemRectMax(&b);
        float h = 4 * scale > 2 ? 4 * scale : 2;
        ImDrawList_AddRectFilled(igGetWindowDrawList(), IM_VEC2(a.x + 2, b.y - h), IM_VEC2(b.x - 2, b.y), menu_accent(255), 0, 0);
    }
    if (igIsItemFocused()) s_focused_tab = id;
    else if (s_focused_tab == id) s_focused_tab = -99;
    return open;
}

// A group's button down the left: the selected one filled with the theme's accent, the cursor's with a gold bar along its left edge.
// Returns true when it is used.
static bool group_button(const char *name, int id, bool selected, float w, float h, float scale) {
    char lbl[80];
    snprintf(lbl, sizeof lbl, "  %s##group%d", name, id);
    bool lit = s_focused_tab == id && igGetCurrentContext()->NavCursorVisible;
    igPushStyleColor_Vec4(ImGuiCol_NavCursor, IM_VEC4(0, 0, 0, 0));
    igPushStyleColor_U32(ImGuiCol_Header, menu_accent(selected ? 120 : 0));
    igPushStyleColor_U32(ImGuiCol_HeaderHovered, menu_accent(selected ? 150 : 50));
    igPushStyleColor_U32(ImGuiCol_HeaderActive, menu_accent(170));
    igPushStyleVar_Vec2(ImGuiStyleVar_SelectableTextAlign, IM_VEC2(0.0f, 0.5f));
    bool clicked = igSelectable_Bool(lbl, selected || lit, 0, IM_VEC2(w, h));
    igPopStyleVar(1);
    igPopStyleColor(4);
    focus_item(id, selected);
    if (lit) {
        ImVec2 a, b;
        igGetItemRectMin(&a), igGetItemRectMax(&b);
        ImDrawList_AddRectFilled(igGetWindowDrawList(), a, IM_VEC2(a.x + (4 * scale > 2 ? 4 * scale : 2), b.y), menu_accent(255), 0, 0);
    }
    if (igIsItemFocused()) s_focused_tab = id;
    else if (s_focused_tab == id) s_focused_tab = -99;
    return clicked;
}

typedef struct {
    float scale;
    bool in_game, down_from_tab;
    void (*prelaunch)(float);
} PageCtx;

// The open page: its child window (what is left of the frame) and the page in it
static void page_draw(PageCtx *c, int id) {
    igSpacing();
    igBeginChild_Str("##page", IM_VEC2(0, 0), ImGuiChildFlags_NavFlattened, 0);
    // Down from the tab row, pages that aren't option rows (Prelaunch, Controls, Stats, Achievements): their first control
    // (option-row pages: opt_focus_first)
    if (c->down_from_tab && (id == -1 || id == 8 || id == 4 || id == 6)) {
        opt_focus_first(false);
        igSetKeyboardFocusHere(0);
        igSetNavCursorVisible(true);
    }
    switch (id) {
    case -1: c->prelaunch(c->scale); break;
    case 0: tab_display(); break;
    case 1: tab_audio(); break;
    case 2: tab_mods(); break;
    case 3: tab_online(); break;
    case 4: tab_stats(); break;
    case 6: tab_achievements(); break;
    case 7: tab_search(); break;
    case 5: tab_general(); break;
    case 8: tab_about(); break;
    case 9: tab_quit(c->scale, c->in_game); break;
    case 10: tab_mii(); break;
    }
    if (opt_cursor_restored()) g_focus_tab = -99, g_focus_frames = 0;  // (a row took the cursor back: not the tab)
    static const char *scroll_test;  // test hook: page scroll, 0..1
    static bool scroll_test_read;
    if (!scroll_test_read) scroll_test = getenv("LARECOMP_MENU_SCROLL"), scroll_test_read = true;
    if (scroll_test) igSetScrollY_Float(igGetScrollMaxY() * (float)atof(scroll_test));
    igEndChild();
}

static bool page_shown(int id, bool prelaunch, bool have_mods) {  // (Quit: in game only; Mods: when the game has some; Online: when it has rows)
    if (id == 9) return !prelaunch;
    if (id == 2) return have_mods;
    if (id == 3) return online_rows_available() || launcher_has_settings();
    if (id == -1) return prelaunch;
    return true;
}

static void tabs_body(float scale, bool in_game, void (*prelaunch)(float)) {
    const GameModRow *mrows;
    bool have_mods = game_mod_rows(&mrows) > 0 || launcher_has_settings() || online_rows_available();
    bool pre = prelaunch != NULL;
    // what is shown: the groups with a page to show, and in them the pages (flat, in order: L / R go through it)
    int vg[N_GROUPS], nvg = 0;  // (the groups' numbers in k_groups)
    int flat[16], nflat = 0;
    int shown_pages[N_GROUPS][4], shown_n[N_GROUPS] = {0};
    for (int g = 0; g < N_GROUPS; g++) {
        for (int i = 0; i < k_groups[g].n; i++) {
            int pid = k_groups[g].pages[i];
            if (page_shown(pid, pre, have_mods)) flat[nflat++] = pid, shown_pages[g][shown_n[g]++] = pid;
        }
        if (shown_n[g]) vg[nvg++] = g;
    }
    static int last_pre = -1;  // (the first page: Prelaunch at the start screen, else the first of Settings)
    if (last_pre != pre) last_pre = pre, g_tab = flat[0];
    int cur = -1;  // g_tab / g_tab_request hold ids
    for (int i = 0; i < nflat; i++) if (flat[i] == g_tab) cur = i;
    if (cur < 0) cur = 0, g_tab = flat[0];
    // L/R (or Q/E) switch pages, going on into the next group (not while typing: Q / E are letters then, and the on-screen keyboard has
    // the controller)
    if (g_tab_request_ext >= 0) { g_tab_request = g_tab_request_ext, g_tab_request_ext = -1, g_focus_tab = -99; }  // (test hook: no cursor on another tab)
    static int request_frames;  // (a request waits for its tab to be drawn, which can take a frame or two)
    if (g_tab_request != -2 && ++request_frames > 6) g_tab_request = -2;
    if (g_tab_request == -2) request_frames = 0;
    bool any_popup = igIsPopupOpen_Str("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();
    if (!any_popup && !typing) {
        int step = igIsKeyPressed_Bool(ImGuiKey_GamepadR1, false) || igIsKeyPressed_Bool(ImGuiKey_E, false) ? 1
                 : igIsKeyPressed_Bool(ImGuiKey_GamepadL1, false) || igIsKeyPressed_Bool(ImGuiKey_Q, false) ? -1 : 0;
        if (step) {
            int to = flat[(cur + step + nflat) % nflat];
            g_tab_request = to, request_frames = 0;
            // the cursor goes along (left behind, A on it switched straight back; user 2026-10-11)
            if (s_focused_tab != -99) g_focus_tab = s_focused_tab >= GROUP_ID ? GROUP_ID + group_of_page(to) : to, g_focus_frames = 0;
        }
    }
    if (g_tab_request != -2) {  // (a request for a page that is not shown is dropped; one that is, is taken at once, its tab selecting it)
        bool ok = false;
        for (int i = 0; i < nflat; i++) ok |= flat[i] == g_tab_request;
        if (ok) g_tab = g_tab_request;
        else g_tab_request = -2;
    }
    // Down with the cursor on a page's tab: the page's first row (option rows; other pages keep ImGui's own pick)
    bool down_from_tab = s_focused_tab != -99 && s_focused_tab < GROUP_ID && igGetCurrentContext()->NavCursorVisible && !any_popup &&
                         (igIsKeyPressed_Bool(ImGuiKey_GamepadDpadDown, true) || igIsKeyPressed_Bool(ImGuiKey_GamepadLStickDown, true) ||
                          igIsKeyPressed_Bool(ImGuiKey_DownArrow, true));
    opt_focus_first(down_from_tab);
    // touch: taller, wider tabs, and the pages' buttons / combos too (a fingertip's size)
    bool touch = g_touch_ui;
    if (touch) igPushStyleVar_Vec2(ImGuiStyleVar_FramePadding, IM_VEC2(igGetStyle()->FramePadding.x * 1.6f,
                                                                       igGetStyle()->FramePadding.y * 2.2f));
    PageCtx pc = {.scale = scale, .in_game = in_game, .down_from_tab = down_from_tab, .prelaunch = prelaunch};
    const float fs = igGetFontSize(), footer_h = touch ? 0 : fs * 2.0f;  // (a finger has no buttons to name)
    ImVec2 at, avail;
    igGetCursorScreenPos(&at);
    igGetContentRegionAvail(&avail);
    const float body_h = avail.y - footer_h;
    // the left column as wide as its longest name needs; the window is wide enough for it and the widest row of tabs
    float lw = 0, sub_w = 0;
    for (int k = 0; k < nvg; k++) {
        float w = tab_width(k_groups[vg[k]].name) + fs * 0.8f;
        if (w > lw) lw = w;
        float row = 0;
        for (int i = 0; i < shown_n[vg[k]]; i++) row += tab_width(page_name(shown_pages[vg[k]][i]));
        if (shown_n[vg[k]] > 1 && row > sub_w) sub_w = row;
    }
    g_tabs_w = lw + sub_w + fs * 1.5f;
    static int last_page[N_GROUPS];  // (each group's page when it was last open: its button goes back to it)
    if (g_focus_tab == -1) g_focus_tab = GROUP_ID + group_of_page(g_tab);  // (the cursor starts on the open group's button)
    igBeginChild_Str("##groups", IM_VEC2(lw, body_h), ImGuiChildFlags_NavFlattened, 0);
    const float row_h = fs * (touch ? 2.6f : 1.9f);
    for (int k = 0; k < nvg; k++) {
        int gi = vg[k];
        if (group_button(k_groups[gi].name, GROUP_ID + gi, gi == group_of_page(g_tab), lw, row_h, scale)) {
            int to = shown_pages[gi][0];
            for (int i = 0; i < shown_n[gi]; i++) if (shown_pages[gi][i] == last_page[gi]) to = last_page[gi];
            g_tab = g_tab_request = to, request_frames = 0;
        }
    }
    igEndChild();
    ImDrawList_AddLine(igGetWindowDrawList(), IM_VEC2(at.x + lw + fs * 0.3f, at.y), IM_VEC2(at.x + lw + fs * 0.3f, at.y + body_h), menu_accent(70), 1.0f);
    igSameLine(0, fs * 0.6f);
    igBeginChild_Str("##frame", IM_VEC2(0, body_h), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        const int ag = group_of_page(g_tab), np = shown_n[ag];
        if (np == 1) {  // (one page: no row of tabs)
            last_page[ag] = g_tab;
            page_draw(&pc, g_tab);
        } else {
            igPushID_Int(ag);  // (each group's row of tabs keeps its own selection)
            if (igBeginTabBar("##pages", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
                for (int i = 0; i < np; i++) {
                    int pid = shown_pages[ag][i];
                    if (nav_tab(page_name(pid), pid, g_tab_request == pid, scale)) {
                        g_tab = pid, last_page[ag] = pid;
                        if (g_tab_request == pid) g_tab_request = -2;
                        page_draw(&pc, pid);
                        igEndTabItem();
                    }
                }
                igEndTabBar();
            }
            igPopID();
        }
    }
    igEndChild();
    opt_focus_first(false);  // (a page without option rows: nothing took it)
    if (footer_h > 0) footer_draw(IM_VEC2(at.x, at.y + body_h), avail.x, footer_h, any_popup);
    restart_popup(scale, in_game);
    if (touch) igPopStyleVar(1);
}

void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool in_game, void (*prelaunch)(float)) {
    ImFont *f0 = g_font, *f1 = g_font_big;
    g_font = font, g_font_big = font_big;
    ImGuiStyle saved = *igGetStyle();  // the menu's look (the start screen has its own style)
    style_init();
    ImGuiStyle_ScaleAllSizes(igGetStyle(), scale);
    tabs_body(scale, in_game, prelaunch);
    *igGetStyle() = saved;
    g_font = f0, g_font_big = f1;
}

// ---- a game's host-drawn screen (game_overlay_image, runtime/game_hooks.c): one RGBA8 image, drawn first (under the
// menu) into the 16:9 rectangle of the window; when it covers the game the rest of the window goes black.
const uint8_t *game_overlay_image(int *w, int *h, int *full, uint64_t *seq);
uint64_t vk_imgui_texture_stream(const void *rgba, int w, int h);  // runtime/nvn/vulkan/imgui_glue.cpp
static void game_overlay_draw(const uint8_t *img, int w, int h, bool full, uint64_t seq, ImVec2 disp) {
    static uint64_t shown = ~0ull;
    ImTextureID tid = 0;
    static uint64_t vk_id;
    if (seq != shown || !vk_id) vk_id = vk_imgui_texture_stream(img, w, h);
    tid = (ImTextureID)vk_id;
    shown = seq;
    float s = fminf(disp.x / w, disp.y / h), dw = w * s, dh = h * s, x = (disp.x - dw) / 2, y = (disp.y - dh) / 2;
    ImDrawList *dl = igGetBackgroundDrawList_Nil();
    if (full) ImDrawList_AddRectFilled(dl, IM_VEC2(0, 0), disp, IM_COL32(0, 0, 0, 255), 0, 0);
    if (tid) ImDrawList_AddImage(dl, tid, IM_VEC2(x, y), IM_VEC2(x + dw, y + dh), IM_VEC2(0, 0), IM_VEC2(1, 1), IM_COL32(255, 255, 255, 255));
}

// ---- a game's loading screen (game_splash, runtime/game_hooks.c): the dot art, the banner, a loading bar and the coffee
// line, in the setup screen's colours (runtime/ui/launcher.cpp). The bar is the setup screen's: [####----]  35%  step.
bool game_splash(float *progress, const char **what);
bool game_launch_skipping(void);  // runtime/game_hooks.c
// A line of text centred across the screen at *y, which moves down past it
static void splash_centred(ImDrawList *dl, ImFont *font, ImVec2 disp, float *y, const char *text, float size, ImU32 col) {
    ImVec2 sz;
    ImFont_CalcTextSizeA(&sz, font, size, 1e9f, 0.0f, text, NULL, NULL);
    ImDrawList_AddText_FontPtr(dl, font, size, IM_VEC2((disp.x - sz.x) / 2, *y), col, text, NULL, 0.0f, NULL);
    *y += sz.y + size * 0.25f;
}
static float splash_text_w(ImFont *font, float size, const char *text) {
    ImVec2 sz;
    ImFont_CalcTextSizeA(&sz, font, size, 1e9f, 0.0f, text, NULL, NULL);
    return sz.x;
}
static void splash_draw(ImVec2 disp, float progress, const char *what) {
    static const ImU32 GOLD = IM_COL32(193, 156, 0, 255), BLUE = IM_COL32(58, 150, 221, 255),
                       PURPLE = IM_COL32(136, 23, 152, 255), BLACK = IM_COL32(12, 12, 12, 255);
    static const int DX[8] = {0, 0, 0, 1, 1, 1, 0, 1}, DY[8] = {0, 1, 2, 0, 1, 2, 3, 3};
    ImDrawList *dl = igGetBackgroundDrawList_Nil();
    ImFont *font = igGetFont();
    float base = igGetFontSize();
    ImDrawList_AddRectFilled(dl, IM_VEC2(0, 0), disp, BLACK, 0, 0);
    // the art, centred in the upper part
    float step = fminf(disp.x * 0.6f / (ART_COLS * 2), disp.y * 0.5f / (ART_ROWS * 4));
    float aw = ART_COLS * 2 * step, ah = ART_ROWS * 4 * step, x0 = (disp.x - aw) / 2, y0 = disp.y * 0.06f, r = step * 0.42f;
    for (int y = 0; y < ART_ROWS; y++)
        for (int x = 0; x < ART_COLS; x++)
            for (int b = 0; b < 8; b++)
                if (ART[y][x] & (1 << b))
                    ImDrawList_AddCircleFilled(dl, IM_VEC2(x0 + ((x * 2 + DX[b]) + 0.5f) * step, y0 + ((y * 4 + DY[b]) + 0.5f) * step), r, PURPLE, 6);
    // the banner under it
    float y = y0 + ah + disp.y * 0.035f;
    splash_centred(dl, font, disp, &y, "smm2-dx", base * 2.6f, GOLD);
    splash_centred(dl, font, disp, &y, "by sometimes_steve", base * 1.3f, GOLD);
    // the loading bar: anchored on the width of the full bar, so it doesn't shift as it fills
    y += disp.y * 0.03f;
    int pct = (int)(progress * 100 + 0.5f);
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    int fill = pct / 5;
    char bar[96];
    snprintf(bar, sizeof bar, "[%.*s%.*s] %3d%%  ", fill, "####################", 20 - fill, "--------------------", pct);
    float bw = splash_text_w(font, base * 1.2f, "[####################] 100%  ");
    float bx = (disp.x - bw - splash_text_w(font, base * 1.2f, "Starting the game")) / 2;
    ImDrawList_AddText_FontPtr(dl, font, base * 1.2f, IM_VEC2(bx, y), BLUE, bar, NULL, 0.0f, NULL);
    ImDrawList_AddText_FontPtr(dl, font, base * 1.2f, IM_VEC2(bx + bw, y), BLUE, what && *what ? what : "Starting the game", NULL, 0.0f, NULL);
    // the coffee line at the very bottom
    const char *coffee = "If you enjoy my work, consider buying me a coffee! https://buymeacoffee.com/sometimes_steve";
    ImVec2 cs;
    ImFont_CalcTextSizeA(&cs, font, base, 1e9f, 0.0f, coffee, NULL, NULL);
    ImDrawList_AddText_FontPtr(dl, font, base, IM_VEC2((disp.x - cs.x) / 2, disp.y - cs.y - disp.y * 0.03f), GOLD, coffee, NULL, 0.0f, NULL);
}

// Touch: a finger dragged up / down scrolls the page under it (ImGui scrolls with a wheel or the scrollbar only). Once
// it has moved 14 px (at 1080 p) it's a drag, not a tap: the row it started on lets go without being pressed.
static void touch_drag_scroll(ImGuiIO *io, float scale) {
    static bool down, drag;
    static ImVec2 start, last;
    static ImGuiID win_id;
    ImGuiContext *g = igGetCurrentContext();
    if (io->MouseSource != ImGuiMouseSource_TouchScreen || !io->MouseDown[0]) { down = drag = false; return; }
    if (!down) {
        down = true, drag = false, start = last = io->MousePos, win_id = 0;
        for (ImGuiWindow *w = g->HoveredWindow; w; w = w->ParentWindow)  // (the nearest window that scrolls)
            if (w->ScrollMax.y > 0) { win_id = w->ID; break; }
        return;
    }
    ImGuiWindow *w = win_id ? igFindWindowByID(win_id) : NULL;
    if (!w) return;
    if (!drag && fabsf(io->MousePos.y - start.y) > 14 * scale) {
        drag = true;
        igClearActiveID();  // (the press doesn't become a tap on release)
    }
    if (drag) igSetScrollY_WindowPtr(w, w->Scroll.y - (io->MousePos.y - last.y));
    last = io->MousePos;
}

static bool g_drew;  // the last menu_render drew (menu_redraw draws it again)

void menu_redraw(void *cmd, uint32_t target_w, uint32_t target_h) {
    (void)target_w, (void)target_h;
    if (!g_drew) return;
    igSetCurrentContext(g_ctx);
    ImGui_ImplVulkan_RenderDrawData(igGetDrawData(), (VkCommandBuffer)cmd, VK_NULL_HANDLE);  // inside the window image's rendering
}

void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) {
    const double tw = target_w, th = target_h;
    bool open = atomic_load(&g_open);
    bool kbd = atomic_load(&g_kbd) == 1;
    int ov_w = 0, ov_h = 0, ov_full = 0;
    uint64_t ov_seq = 0;
    const uint8_t *ov_img = game_overlay_image(&ov_w, &ov_h, &ov_full, &ov_seq);
    float splash_p = 0;
    const char *splash_what = "";
    bool splash = game_splash(&splash_p, &splash_what);
    {   // play time (issue #15): this frame counts when the game is being looked at and played
        extern SDL_Window *ui_window(void);  // runtime/nvn/vulkan/device.c
        SDL_Window *w = ui_window();
        extern volatile int g_app_inactive;  // services/audio.c
        g_app_inactive = !w || !(SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) || (SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED);
        playtime_tick(!open && !kbd && !splash && w && (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) &&
                      !(SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED));
        history_tick();
        character_tick();  // (adds what playtime_tick just counted to the character being played)
    }
    g_drew = false;
    if (!open && !kbd && !g_settings.show_fps && !g_settings.show_memory && !(g_settings.show_recomp_version && game_launch_skipping()) && !ov_img && !splash && !touchpad_visible() && !notice_active()) {
        if (g_inited) { pthread_mutex_lock(&q_lock); q_count = 0; pthread_mutex_unlock(&q_lock); }
        return;
    }
    if (!g_inited) g_atlas_scale = th / 1080.0 < 1 ? 1.0f : th / 1080.0 > 2 ? 2.0f : (float)(th / 1080.0);
    if (!g_inited) init();
    igSetCurrentContext(g_ctx);
    ImGuiIO *io = igGetIO();
    io->DisplaySize = IM_VEC2((float)tw, (float)th);
    io->DisplayFramebufferScale = IM_VEC2(1, 1);
    static double last;
    double now = SDL_GetTicks() / 1000.0;
    io->DeltaTime = last > 0 && now > last ? (float)(now - last) : 1.0f / 60;
    last = now;
    drain_events(io);
    // fonts are built at 34/56 px for a ~1080 px tall output; scale to the window
    // reactive: grows slower than the window (0.8 at 1080 p, ~1.35 at 4K rather than 2), and never wider than the
    // window can hold (narrow windows shrink the menu instead of cutting it off); Menu size scales the result
    float scale = 0.8f * powf((float)(th / 1080.0), 0.75f);
    if (scale > (float)(tw / 1100.0)) scale = (float)(tw / 1100.0);
    scale *= g_settings.menu_scale;
    if (scale < 0.3f) scale = 0.3f;
    io->FontGlobalScale = scale / g_atlas_scale;

    // spacing/rounding follow the output size too (style tuned for 1080 p)
    static ImGuiStyle base;
    static float style_scale = -1;
    static int theme_seen = -1;
    if (style_scale < 0) base = *igGetStyle();
    bool recolour = theme_seen != g_settings.theme;  // (General tab > Theme: the colours of the unscaled style, then rescale)
    if (recolour) theme_apply(&base), theme_seen = g_settings.theme;
    if (recolour || fabsf(style_scale - scale) > 0.01f) {
        *igGetStyle() = base;
        ImGuiStyle_ScaleAllSizes(igGetStyle(), scale);
        style_scale = scale;
    }
    ImGui_ImplVulkan_NewFrame();
    igNewFrame();
    if (splash) splash_draw(io->DisplaySize, splash_p, splash_what);
    if (ov_img) game_overlay_draw(ov_img, ov_w, ov_h, ov_full, ov_seq, io->DisplaySize);

    if (g_settings.show_fps || g_settings.show_memory) {
        igSetNextWindowPos(IM_VEC2(io->DisplaySize.x - 16 * scale, 16 * scale), 0, IM_VEC2(1, 0));
        igSetNextWindowBgAlpha(0.5f);
        igBegin("##fps", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        static uint64_t real0, gen0;  // frame generation: the share of generated frames, over about half a second
        static double t0, out_ratio;
        if (now - t0 > 0.5) {
            uint64_t dr = g_fg_real - real0, dg = g_fg_generated - gen0;
            out_ratio = dr ? 1.0 + (double)dg / dr : 1.0;
            real0 = g_fg_real, gen0 = g_fg_generated, t0 = now;
        }
        if (g_settings.show_fps) {
            if (out_ratio > 1.05) igText("%.0f fps (%.0f with frame generation)", io->Framerate, io->Framerate * out_ratio);
            else igText("%.0f fps", io->Framerate);
        }
        if (g_settings.show_memory) {  // (sampled once a second: the call isn't free on every frame)
            static double mem_t;
            static uint64_t used, left;
            if (now - mem_t >= 1.0 || mem_t == 0) used = plat_memory_used(&left), mem_t = now;
            if (left) igText("%llu MB (%llu MB left)", (unsigned long long)(used >> 20), (unsigned long long)(left >> 20));
            else igText("%llu MB", (unsigned long long)(used >> 20));
        }
        igEnd();
    }

    if (g_settings.show_recomp_version && game_launch_skipping()) {  // (Show recomp version: while the game starts with Skip intro / Skip mode select, which never show the title screen's Ver. box)
        // the game's own Ver. box, measured from the title screen (1080 p units, scaled with the window's height): a
        // translucent black rounded box, 368 x 55, 44 from the right and 14 from the bottom, "Ver. <version>" in white
        // centred in it, cut off at its edges if too long
        const char *v = RECOMP_VERSION;
        if (*v == 'v') v++;
        char t[64];
        snprintf(t, sizeof t, "Ver. %s", v);
        float k = (float)(th / 1080.0);
        ImVec2 p1 = IM_VEC2(io->DisplaySize.x - 44 * k, io->DisplaySize.y - 14 * k), p0 = IM_VEC2(p1.x - 368 * k, p1.y - 55 * k);
        ImDrawList *dl = igGetForegroundDrawList_Nil();
        ImDrawList_AddRectFilled(dl, p0, p1, IM_COL32(0, 0, 0, 178), 14 * k, 0);
        ImFont *font = igGetFont();
        float size = 29 * k;
        ImVec2 ts;  // (centred in the box both ways; a text wider than the box is cut off at its edges)
        ImFont_CalcTextSizeA(&ts, font, size, 1e9f, 0.0f, t, NULL, NULL);
        ImDrawList_PushClipRect(dl, p0, p1, false);
        ImDrawList_AddText_FontPtr(dl, font, size, IM_VEC2(p0.x + (368 * k - ts.x) / 2, p0.y + (55 * k - ts.y) / 2), IM_COL32(255, 255, 255, 255), t, NULL, 0.0f, NULL);
        ImDrawList_PopClipRect(dl);
    }

    if (!open && !kbd) touchpad_draw((float)tw, (float)th);
    {   // (the notice's life in the log: when it first appears, when the open menu holds it back, when it ends)
        static int state;  // 0 none, 1 showing, 2 held back by the menu
        bool act = notice_active();
        int now_state = !act ? 0 : open ? 2 : 1;
        if (now_state != state) {
            LOG("[notice] %s (%.1f s)\n", now_state == 1 ? "shown" : now_state == 2 ? "held back: the menu is open" : state == 1 ? "ended" : "ended while the menu was open", SDL_GetTicks() / 1000.0);
            state = now_state;
        }
    }
    if (!open && notice_active()) {
        char t[512];
        pthread_mutex_lock(&notice_lock);
        snprintf(t, sizeof t, "%s", notice_text);
        pthread_mutex_unlock(&notice_lock);
        // the toast is always as at Normal: the Menu Size preset (g_settings.menu_scale) doesn't apply to it, so everything
        // here is the menu's scale divided by it (the window's own scale, the text and the padding)
        float ms = g_settings.menu_scale > 0.1f ? g_settings.menu_scale : 1.0f, ns = scale / ms;
        // the box fits its text (wrapping only past the widest it may be)
        float maxw = fminf(io->DisplaySize.x * 0.8f, 1100 * ns);
        ImVec2 pad = IM_VEC2(igGetStyle()->WindowPadding.x / ms, igGetStyle()->WindowPadding.y / ms), ts;
        ImFont_CalcTextSizeA(&ts, igGetFont(), igGetFontSize() / ms, 1e9f, maxw - 2 * pad.x, t, NULL, NULL);
        igSetNextWindowPos(IM_VEC2(io->DisplaySize.x * 0.5f, io->DisplaySize.y - 120 * ns), 0, IM_VEC2(0.5f, 1));  // (up from the bottom edge: the game's own bottom-centre prompts stay uncovered)
        igSetNextWindowSize(IM_VEC2(ts.x + 2 * pad.x + 1, ts.y + 2 * pad.y), 0);
        igSetNextWindowBgAlpha(0.85f);
        igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, pad);
        igBegin("##notice", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
        igSetWindowFontScale(1.0f / ms);
        igPushTextWrapPos(0);
        igTextUnformatted(t, NULL);
        igPopTextWrapPos();
        igEnd();
        igPopStyleVar(1);
    }

    if (open) touch_drag_scroll(io, scale);
    opt_touch(g_touch_ui);
    if (open) {
        // dim the game behind the menu
        ImDrawList_AddRectFilled(igGetBackgroundDrawList_Nil(), IM_VEC2(0, 0), io->DisplaySize, IM_COL32(0, 0, 0, 110), 0, 0);
        // the same size on every tab (it follows the window and the menu's scale, not the page: nothing jumps when you
        // switch tabs). Tall as the window allows (a slim margin) but no taller than the menu's own scale makes sensible on a big
        // screen; wide enough for the whole top row (the title and every tab) at least, otherwise 1.6 x its height.
        float h = fminf(io->DisplaySize.y - 2 * 18 * scale, 1400 * scale);
        float top_row = g_tabs_w + igGetStyle()->WindowPadding.x * 2 + 24 * scale;
        float w = fminf(io->DisplaySize.x * 0.96f, fmaxf(top_row, h * 1.6f));
        igSetNextWindowPos(IM_VEC2(io->DisplaySize.x * 0.5f, io->DisplaySize.y * 0.5f), ImGuiCond_Always, IM_VEC2(0.5f, 0.5f));
        igSetNextWindowSize(IM_VEC2(w, h), ImGuiCond_Always);
        if (g_touch_ui) igPushStyleVar_Float(ImGuiStyleVar_ScrollbarSize, 34 * scale);  // (a thumb's width)
        bool nes = nes_theme();
        if (nes) {  // (the frame is drawn below, with stepped corners: ImGui's own background and border can't have them)
            igPushStyleColor_Vec4(ImGuiCol_WindowBg, IM_VEC4(0, 0, 0, 0));
            igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 0);
        }
        igBegin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        if (nes) {
            igPopStyleVar(1);
            igPopStyleColor(1);
            ImVec2 wp;
            igGetWindowPos(&wp);
            nes_box(igGetWindowDrawList(), wp, IM_VEC2(wp.x + igGetWindowWidth(), wp.y + igGetWindowHeight()),
                    igGetColorU32_Vec4(theme_palette()->window), NES_WHITE, 4 * scale, 0);
        }
        ImFont *keep_big = g_font_big;  // (the window in the menu's font; header() uses g_font_big)
        g_font_big = menu_font_big();
        igPushFont(menu_font());
        // a close X in the top right corner (B / Esc too), over the body rather than in a row of its own: no title ("Title
        // screen" - rt_request_reboot(RT_REBOOT_TITLE) - is to find a new home; user 2026-10-11.)
        {
            float bx = (g_touch_ui ? 64 : 44) * scale;
            ImVec2 body_at;
            igGetCursorPos(&body_at);
            igSetCursorPos(IM_VEC2(igGetWindowWidth() - igGetStyle()->WindowPadding.x - bx, igGetStyle()->WindowPadding.y));
            ImVec2 p;
            igGetCursorScreenPos(&p);
            igPushStyleColor_Vec4(ImGuiCol_NavCursor, IM_VEC4(0, 0, 0, 0));
            bool close = igInvisibleButton("##close", IM_VEC2(bx, bx), ImGuiButtonFlags_EnableNav);
            igPopStyleColor(1);
            bool hot = igIsItemHovered(0) || igIsItemFocused();
            ImDrawList *dl = igGetWindowDrawList();
            if (nes) {  // a pixel X on a notched button (red when pointed at)
                nes_box(dl, p, IM_VEC2(p.x + bx, p.y + bx), hot ? NES_RED : NES_BLACK, hot ? NES_WHITE : NES_GREY, 3 * scale,
                        hot ? NES_RED_D : NES_RGB(21, 24, 27));
                float px = floorf(bx / 12);
                const ImU32 x[] = {NES_WHITE};
                nes_icon(dl, IM_VEC2(p.x + (bx - 7 * px) * 0.5f, p.y + (bx - 7 * px) * 0.5f), kNesCross, NES_ROWS(kNesCross), px, x, false);
            } else {
                ImU32 col = hot ? menu_accent(255) : IM_COL32(220, 216, 205, 255);
                float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * scale * 1.5f;
                if (hot) ImDrawList_AddRectFilled(dl, p, IM_VEC2(p.x + bx, p.y + bx), IM_COL32(255, 255, 255, 20), 8 * scale, 0);
                ImDrawList_AddLine(dl, IM_VEC2(p.x + m, p.y + m), IM_VEC2(p.x + bx - m, p.y + bx - m), col, t);
                ImDrawList_AddLine(dl, IM_VEC2(p.x + bx - m, p.y + m), IM_VEC2(p.x + m, p.y + bx - m), col, t);
            }
            if (close) set_open(false);
            igSetCursorPos(body_at);
        }

        bool any_popup = igIsPopupOpen_Str("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();  // (before the
        tabs_body(scale, true, NULL);                                                                          //  pages run)
        // B with nothing focused-deeper closes the menu (not the Esc / B that ends typing: typing is from the frame
        // before, so it's still true the frame a field lets go)
        if (!any_popup && !typing && !ui_text_busy() && igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            (igIsKeyPressed_Bool(ImGuiKey_GamepadFaceRight, false) || igIsKeyPressed_Bool(ImGuiKey_Escape, false)) &&
            !mods_page_used_back())
            set_open(false);
        (void)mods_page_used_back();  // (a B the Mods tab used, and the menu didn't look at: not kept for later)
        igPopFont();
        g_font_big = keep_big;
        igEnd();
        if (g_touch_ui) igPopStyleVar(1);
        osk_draw(g_font_big, scale);  // (a text field activated with the controller)
    }

    if (kbd) keyboard_draw(io, scale);
    igRender();
    g_drew = true;
    g_text_busy = open && ui_text_busy();
    ImGui_ImplVulkan_RenderDrawData(igGetDrawData(), (VkCommandBuffer)cmd, VK_NULL_HANDLE);  // inside the window image's rendering
}
