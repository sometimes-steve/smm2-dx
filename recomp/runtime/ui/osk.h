// On-screen keyboard (ImGui) for controllers, shared by the game's name entry (runtime/ui/menu.cpp keyboard_draw,
// runtime/hle/swkbd.c) and every text field in our menus (in-game overlay, start screen): ui_input_text.
#pragma once
#include <stddef.h>
#include <stdbool.h>

#include "ui/ig.h"

// A text field. Activated with a controller (A on it) it opens the on-screen keyboard and the confirmed text goes
// back into buf; with a mouse / keyboard it's typed into directly. Returns true when the text changed (typed, or OK
// on the on-screen keyboard; with ImGuiInputTextFlags_EnterReturnsTrue: Enter / OK). title heads the keyboard.
bool ui_input_text(const char *label, char *buf, size_t size, ImGuiInputTextFlags flags, const char *hint,
                   const char *title);
// true (once) after the on-screen keyboard of the field just drawn with ui_input_text was cancelled
bool ui_input_text_cancelled(void);

// Once per frame, after the rest of the UI, at the root (outside any window), in the context the field was drawn in.
void osk_draw(ImFont *big, float scale);
bool osk_is_open(void);
void osk_cancel(void);  // (menu closed / reboot: drop it, the field keeps its text)

// Something is being typed in the current context: a text field has keyboard focus or the on-screen keyboard is up.
// Menu shortcuts (back, tabs, close) must not fire then: the keys are text.
bool ui_text_busy(void);

// A row of keycap hints, as the menu's footer: n pairs of a key's name and what it does, and `right` (a count, or NULL) at the
// right edge. Drawn at the cursor in the current window, which it moves down past the row. ui_hint_names: the names of the
// confirm / back / start buttons for the player's Button Icons, with a keyboard (pad false) or a controller.
void ui_hint_row(const char *const *keys, const char *const *what, int n, const char *right);
void ui_hint_names(bool pad, const char **confirm, const char **back, const char **start);

// The Course ID / Maker ID prompt (issue #17). An ID is nine characters, each a digit or one of B C D F G H J K L M N P Q R S T V
// W X Y (the letters of 570 IDs from OpenCourse World, checked), shown as AAA-BBB-CCC. osk_id_char: can c be in one (a
// lowercase letter counts: it is made capital). osk_id_compact: text -> its ID characters, capital, at most nine. osk_id_dashed:
// those nine with the dashes between the groups (a dash only once a character follows it). osk_set_id_mode: osk_keys follows
// the rules: capital letters only, the characters that can't be in an ID greyed out, typed ones dropped, the text dashed.
bool osk_id_char(int c);
void osk_id_compact(const char *in, char out[10]);
void osk_id_dashed(const char *raw, char out[16]);
void osk_set_id_mode(bool on);

// The typed text (big font) and the key grid, drawn into the current window: d-pad / stick move, A types, B deletes,
// Start / + OK; typed keys work too (Enter OK, Esc cancel). text: ASCII, at most max_chars. Returns 1 OK, -1 cancel,
// 0 still typing.
int osk_keys(char *text, size_t cap, int max_chars, bool appearing, float scale, ImFont *big, bool upper_first);
