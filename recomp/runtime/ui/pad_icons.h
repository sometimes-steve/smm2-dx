// Controller button pictures for the menu (Controls page, the footer's and keyboard's button prompts): the Nintendo, Xbox
// and PlayStation sets (SVGs in ui/icons/pad/, built into the program, from tools/gen_pad_icons.py), drawn with nanosvg
// into one texture for each family, made when first wanted. fam: 0 Nintendo, 1 Xbox, 2 PlayStation (input_pad_family).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ig.h"

// The picture of the controller button `host` (an input_bind.h PADB value) in family fam: its texture and corners (false: none)
bool pad_icon_host(int fam, int host, uint64_t *tex, ImVec2 *uv0, ImVec2 *uv1);
// The picture of the Switch input `slot` (input_bind.h BIND_A.., not the keyboard's sticks), always the Nintendo one
bool pad_icon_slot(int slot, uint64_t *tex, ImVec2 *uv0, ImVec2 *uv1);
// A prompt's button name as the menu writes it ("A", "Cross", "LB / RB", "L1 / R1", "+") as pictures h tall, starting at `at`;
// draw false only measures. Returns the width (0 when any part has no picture: draw the name as text).
float pad_label_draw(ImDrawList *dl, int fam, const char *label, ImVec2 at, float h, bool draw);
