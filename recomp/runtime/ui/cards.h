// The cards the Stats and Achievements tabs are made of (runtime/ui/stats_rows.c): a focusable box with a title, rows drawn
// into it from card.y down, and the controller's cursor takes each one, which is how a page of them scrolls. Plain C.
#pragma once
#include "ui/ig.h"

uint32_t menu_accent(int a);  // runtime/ui/menu.c
uint32_t menu_muted(void);

typedef struct { float fs, x, y, w, top; ImDrawList *dl; } Card;  // top: the card's top edge

Card card_begin(const char *title, float h);  // h: the card's height; the width is what is left
float rows_h(float fs, int n);                // the height of a card of n rows
void card_end(Card *c);
float text_w(const char *s);
void card_row(Card *c, const char *label, const char *value);  // label at the left, value at the right
// The small circular refresh arrow in a card's top right corner, a spinning arc while busy (runtime/ui/achievements_rows.c). True when clicked.
bool ui_card_refresh(const Card *c, bool busy);
// A small arc going round (t: the time in radians), in place of a figure that is not known yet (runtime/ui/achievements_rows.c)
void ui_spin(ImDrawList *dl, float cx, float cy, float r, ImU32 col, float t);
