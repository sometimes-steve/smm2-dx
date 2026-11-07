// Control remapping editor (runtime/input_bind.h), shared by the in-game menu's Controls tab (menu.mm) and the start
// screen's Controls page (launcher.mm): a table of Switch inputs with the controller button and the key bound to
// each. Select a binding, then press the new button / key (Esc or 5 s without a press cancels; a button already in
// use swaps over). Drawn in whichever ImGui context is current.
#include "ui/ig.h"

#include <SDL3/SDL.h>
#include <float.h>
#include <stdio.h>

#include "../input_bind.h"
#include "../settings.h"

static void cell(int dev, int slot, int fam, int cap_dev, int cap_slot, int ms_left) {
    char lbl[96];
    bool waiting = cap_dev == dev && cap_slot == slot;
    if (waiting) snprintf(lbl, sizeof lbl, "Press %s...  %d##%d_%d", dev == BIND_DEV_PAD ? "a button" : "a key",
                          (ms_left + 999) / 1000, dev, slot);
    else snprintf(lbl, sizeof lbl, "%s##%d_%d", dev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[slot], fam) :
                                                 bind_key_label(g_bind_key[slot]), dev, slot);
    // changed from the default: gold text; the same button / key on two inputs (a hand-edited larecomp.ini): red
    const int16_t *t = dev == BIND_DEV_PAD ? g_bind_pad : g_bind_key;
    int n = dev == BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT, pushed = 0;
    bool dup = false;
    for (int i = 0; i < n; i++) dup |= i != slot && t[i] == t[slot] && t[slot] > (dev == BIND_DEV_PAD ? -1 : 0);
    if (waiting) { igPushStyleColor_Vec4(ImGuiCol_Button, IM_VEC4(0.55f, 0.42f, 0.12f, 1)); pushed++; }
    else if (dup) { igPushStyleColor_Vec4(ImGuiCol_Button, IM_VEC4(0.55f, 0.16f, 0.12f, 1)); pushed++; }
    if (!bind_is_default_slot(dev, slot)) { igPushStyleColor_Vec4(ImGuiCol_Text, IM_VEC4(0.98f, 0.82f, 0.38f, 1)); pushed++; }
    if (igButton(lbl, IM_VEC2(-FLT_MIN, 0))) {
        if (waiting) bind_capture_cancel();
        else bind_capture_begin(dev, slot);
    }
    igPopStyleColor(pushed);
    if (waiting && !igIsItemVisible()) igSetScrollHereY(0.5f);  // (a capture started by a test hook)
}

void controls_bindings_draw(float scale) {
    (void)scale;
    int fam = bind_label_family();
    static const char *const fam_name[] = {"Nintendo", "Xbox", "PlayStation"};
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    const char *pad_name = ids && n > 0 ? SDL_GetGamepadNameForID(ids[0]) : NULL;
    char line[200];
    if (n > 0) snprintf(line, sizeof line, "Controller: %s  (%s button names)", pad_name ? pad_name : "connected", fam_name[fam % 3]);
    else snprintf(line, sizeof line, "No controller connected  (%s button names shown)", fam_name[fam % 3]);
    SDL_free(ids);
    igTextUnformatted(line, NULL);
    igPushTextWrapPos(0);
    igTextDisabled("Select a binding, then press the new button or key. Esc (or 5 seconds) cancels. A button "
                   "that's already in use swaps with the old one.");
    igPopTextWrapPos();

    int cd, cs, ms;
    bind_capture_active(&cd, &cs, &ms);
    // a finished capture: say what happened, keep larecomp.ini current
    static int seen_serial = -1;
    static char msg[200];
    int rdev, rslot, rother, serial;
    int kind = bind_last_result(&rdev, &rslot, &rother, &serial);
    if (seen_serial < 0) seen_serial = serial;
    if (serial != seen_serial) {
        seen_serial = serial;
        const char *now = rslot < 0 ? "" : rdev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[rslot], fam) : bind_key_label(g_bind_key[rslot]);
        if (kind == 5) snprintf(msg, sizeof msg, "%s bindings reset to the defaults.", rdev == BIND_DEV_PAD ? "Controller" : "Keyboard");
        else if (kind == 1) snprintf(msg, sizeof msg, "%s is now %s.", bind_input_name(rslot), now);
        else if (kind == 2)
            snprintf(msg, sizeof msg, "%s is now %s; %s moved to %s (swapped).", bind_input_name(rslot), now,
                     bind_input_name(rother), rdev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[rother], fam) : bind_key_label(g_bind_key[rother]));
        else if (kind == 3) snprintf(msg, sizeof msg, "Cancelled: %s unchanged.", bind_input_name(rslot));
        else if (kind == 4) snprintf(msg, sizeof msg, "Nothing pressed: %s unchanged.", bind_input_name(rslot));
        if (kind == 1 || kind == 2 || kind == 5) settings_save();
    }
    if (msg[0]) igTextColored(IM_VEC4(0.93f, 0.76f, 0.33f, 1), "%s", msg);

    if (igBeginTable("##binds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX,
                     IM_VEC2(0, 0), 0.0f)) {
        igTableSetupColumn("Switch", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        igTableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 1.3f, 0);
        igTableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthStretch, 1.3f, 0);
        igTableHeadersRow();
        // in the order a player thinks of them: face buttons, shoulders, Plus / Minus, d-pad, sticks
        static const int order[] = {BIND_A, BIND_B, BIND_X, BIND_Y, BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_PLUS,
                                    BIND_MINUS, BIND_UP, BIND_DOWN, BIND_LEFT, BIND_RIGHT, BIND_LSTICK, BIND_RSTICK,
                                    BIND_LS_UP, BIND_LS_DOWN, BIND_LS_LEFT, BIND_LS_RIGHT, BIND_RS_UP, BIND_RS_DOWN,
                                    BIND_RS_LEFT, BIND_RS_RIGHT};
        for (size_t oi = 0; oi < sizeof order / sizeof order[0]; oi++) {
            int slot = order[oi];
            igTableNextRow(0, 0.0f);
            igTableNextColumn();
            igAlignTextToFramePadding();
            igTextUnformatted(bind_input_name(slot), NULL);
            igTableNextColumn();
            if (slot < BIND_PAD_COUNT) cell(BIND_DEV_PAD, slot, fam, cd, cs, ms);
            else igTextDisabled(slot < BIND_RS_UP ? "Left stick" : "Right stick");
            igTableNextColumn();
            cell(BIND_DEV_KEY, slot, fam, cd, cs, ms);
        }
        igEndTable();
    }
    igSpacing();
    igBeginDisabled(bind_is_default(BIND_DEV_PAD));
    if (igButton("Reset controller to defaults", IM_VEC2(0, 0))) { bind_capture_cancel(); bind_reset(BIND_DEV_PAD); }
    igEndDisabled();
    igSameLine(0, -1);
    igBeginDisabled(bind_is_default(BIND_DEV_KEY));
    if (igButton("Reset keyboard to defaults", IM_VEC2(0, 0))) { bind_capture_cancel(); bind_reset(BIND_DEV_KEY); }
    igEndDisabled();
    igPushTextWrapPos(0);
    igTextDisabled("Bindings apply to the game only. Menus keep the fixed layout (right face button / Enter "
                   "select, bottom face button / Esc back), and the in-game menu always opens with F1 or Esc, or "
                   "the controller's Back + Start held together, or its Home / Guide button - so this page can't be "
                   "locked out. The game's button icons follow the controller bindings.");
    igPopTextWrapPos();
}
