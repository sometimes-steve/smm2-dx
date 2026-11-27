// The little pictures beside the counters drawn over the game (Show course deaths, Show Endless skips): SVGs (ui/icons/*.svg, built into the program)
// drawn with nanosvg (third_party/nanosvg).
#pragma once
#include <stdint.h>

enum { HUD_ICON_DEATHS, HUD_ICON_SKIP };

// An ImGui texture id of the icon drawn px x px (0 if it could not be made). Made once for each size (rounded up to a multiple of 16).
uint64_t hud_icon(int which, int px);
