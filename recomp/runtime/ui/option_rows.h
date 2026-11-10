// Settings pages in two panes (runtime/ui/option_rows.cpp): option rows on the left - an uppercase label, the value on
// the right (brighter when it isn't the default) - and the selected (controller) or hovered (mouse) row's description on
// the right, instead of tooltips. A / click changes a row (toggle / next value), left / right step it.
//
//   opt_begin("##gameplay", scale);
//   opt_section("Difficulty");
//   if (opt_toggle("One-hit KO", &g_settings.diff_ohko, false, "Any hit defeats Link.")) settings_save();
//   opt_end();
#pragma once
#include <stdbool.h>

void opt_begin(const char *id, float scale);
void opt_section(const char *title);
// the page being drawn has no "Reset Page to Defaults" row at its end (the Quit tab: nothing there is a setting to reset)
void opt_no_reset(void);
// a section that folds (A / click on its heading): true when open, so its rows are drawn: if (opt_section_fold("X")) { rows }
bool opt_section_fold(const char *title);
// a choice that previews each entry while it is highlighted in its list; B / Esc puts the old value back. True when *v changed.
bool opt_choice_live(const char *label, int *v, const char *const *names, int n, int def, const char *desc);
// the menu opened again: the pages' scroll and selected row come back (opt_cursor_restored: a row took the cursor)
void opt_remember_arm(void);
bool opt_cursor_restored(void);
bool opt_toggle(const char *label, bool *v, bool def, const char *desc);
// wrap: past the last value back to the first (and the other way); false: stops at the ends
bool opt_choice(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap);
// steps of `step` between lo and hi; fmt formats the value (e.g. "%gx")
bool opt_slider(const char *label, float *v, float lo, float hi, float step, float def, const char *fmt, const char *desc);
// whole numbers lo..hi in steps of `step` (holding left / right speeds up); fmt formats it (e.g. "%d rupees"), lo_name
// (if given) is shown for lo instead (e.g. "Any")
bool opt_int(const char *label, int *v, int lo, int hi, int step, int def, const char *fmt, const char *desc,
             const char *lo_name);
// an action: true when activated
bool opt_button(const char *label, const char *desc);
// one of a set of choices (a ring on the right, filled when selected); true when chosen
bool opt_radio(const char *label, bool selected, const char *desc);
// an action / item row with a value on the right (upper: the label in capitals, as option rows; false: as written,
// e.g. a seed name); true when activated
bool opt_action(const char *label, const char *value, const char *desc, bool upper);
// One-pane pages (the Ship of Harkinian look): full-width rows, each row's description as a tooltip while it is hovered or
// selected. opt_flat_begin takes the place of opt_begin (opt_end closes both). The two-pane page stays for the Mods.
void opt_flat_begin(const char *id, float scale);
// A picture (an ImTextureID, w x h) for the description pane of the row just drawn: shown above its text while that row is
// the hovered / selected one. w < 0: it won't come (a place says so); tex 0: still loading (a place says so).
#include <stdint.h>
void opt_desc_image(uint64_t tex, int w, int h);
// text: the value shown on the right (hint when empty); A / click turns the row into a text field (the on-screen
// keyboard with a controller). True when Enter / the keyboard confirmed a change.
#include <stddef.h>
bool opt_text(const char *label, char *buf, size_t cap, const char *hint, const char *desc);
// that text row is being edited (its buffer is the user's: don't overwrite it)
bool opt_text_editing(const char *label);
// the next row is shown greyed out and can't be changed (e.g. forced by the seed)
void opt_disable_next(void);
// rows after this sit one step in (+1) or back out (-1): options that belong to the row above
void opt_indent(int step);
void opt_end(void);
// the next page's first row takes the controller's cursor (Down on the tab bar: ImGui's own pick across the nested
// panes was unreliable - some tabs went to a row further down, some nowhere). Cleared by the next opt_begin / call.
void opt_focus_first(bool on);
// touch (a finger last): taller rows, the ◀ ▶ arrows easier to hit
void opt_touch(bool on);
// A question in its own window over the page (a modal popup): 1 = yes, -1 = no (B / Esc too), 0 = still open or not
// asked. *asked: set to ask it (cleared once the window is up). The safe button has the focus. in_game adds "Progress
// since your last save is lost." Call it where the question was asked (the same ID stack).
int opt_confirm_window(const char *id, bool *asked, const char *msg, bool in_game, const char *yes_label,
                       const char *no_label, float scale);
