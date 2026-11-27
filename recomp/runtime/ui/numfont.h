// The game's own number font (Mario_NumberFont2, in Font/BmpFont_US.szs of the player's own game files): the big chunky outlined digits of Super Mario
// Maker 2, for the counters the recomp draws over the game (Show course deaths, Show Endless skips; issue #59). It holds 0-9 , . : ? x and a few symbols
// (no letters). The font file is a bitmap font (BFFNT) whose glyph sheet is a texture (BNTX, BC5, block-linear and upside down); it is unpacked,
// decoded and put in one texture the first time it is asked for.
#pragma once
#include <stdbool.h>

#include "ig.h"

// true when the font is there (loaded on the first call; false if the game's file is missing or is not what 3.0.3 has)
bool numfont_ready(void);
// the width of s drawn with glyphs `h` pixels tall (the font's own cell is 53 tall); characters it does not have are left out
float numfont_width(const char *s, float h);
// s with its top left at (x, y), the glyphs h tall, tinted `fill` (the black outline is in the font); the width drawn
float numfont_draw(ImDrawList *dl, float x, float y, float h, const char *s, ImU32 fill);
