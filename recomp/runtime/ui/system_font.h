// The overlay menu's text font: a system font file (runtime/ui/system_font.cpp).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
// The path of a system font file for ImGui, and the face to use in it (a .ttc holds several). false: none found.
bool ui_system_font(char *path, size_t n, int *face);
// The rank-th system font to merge in for the CJK scripts, most fitting first (kana / Hangul seen in the text, else the user's UI
// language, decide the order). false: no more.
enum { UI_SCRIPT_JA, UI_SCRIPT_ZH_HANS, UI_SCRIPT_ZH_HANT, UI_SCRIPT_KO };
bool ui_cjk_font(int rank, bool kana, bool hangul, char *path, size_t n, int *face);
#ifdef __cplusplus
}
#endif
