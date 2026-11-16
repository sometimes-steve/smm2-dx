// The Mii editor's preview (mii_render.c): the Mii drawn by a public Mii renderer, off the menu's thread.
#pragma once
#include <stdint.h>

// The Mii to show: called every frame the editor is open. A change is sent for rendering once it has stood still for a moment
// (the Mii's 88 bytes go to mii-unsecure.ariankordi.net, which draws them; nothing else is sent).
void mii_render_want(const uint8_t d[0x58]);

// The latest picture (RGBA, *w x *h; *seq changes with each new one), NULL before the first. *state: 0 shown picture is the
// wanted Mii's, 1 it is being drawn now (the picture, if any, is the one before), 2 it couldn't be drawn (why in `why`, 96 bytes).
const uint8_t *mii_render_get(int *w, int *h, unsigned *seq, int *state, char *why);
