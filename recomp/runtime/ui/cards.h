// The cards the Stats and Achievements tabs are made of (runtime/ui/stats_rows.c): a focusable box with a title, rows drawn
// into it from card.y down, and the controller's cursor takes each one, which is how a page of them scrolls. Plain C.
#pragma once
#include "ui/ig.h"

uint32_t menu_accent(int a);  // runtime/ui/menu.c
uint32_t menu_muted(void);

typedef struct { float fs, x, y, w; ImDrawList *dl; } Card;

Card card_begin(const char *title, float h);  // h: the card's height; the width is what is left
float rows_h(float fs, int n);                // the height of a card of n rows
void card_end(Card *c);
float text_w(const char *s);
void card_row(Card *c, const char *label, const char *value);  // label at the left, value at the right
