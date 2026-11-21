// The overlay menu's text font (system_font.h): a system font file ImGui can load. The known paths are tried in order;
// without one the menu falls back to ImGui's built-in 13 px bitmap font.
#include "system_font.h"

#include <stdio.h>
#include <sys/stat.h>
#include <windows.h>

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

// The Windows fonts that carry the CJK scripts (the player's own: nothing is shipped), face 0 of each file
static const struct { const char *path; int script; } cjk_fonts[] = {
    {"C:/Windows/Fonts/YuGothM.ttc", UI_SCRIPT_JA}, {"C:/Windows/Fonts/msgothic.ttc", UI_SCRIPT_JA},
    {"C:/Windows/Fonts/msyh.ttc", UI_SCRIPT_ZH_HANS}, {"C:/Windows/Fonts/simsun.ttc", UI_SCRIPT_ZH_HANS},
    {"C:/Windows/Fonts/msjh.ttc", UI_SCRIPT_ZH_HANT}, {"C:/Windows/Fonts/mingliub.ttc", UI_SCRIPT_ZH_HANT},
    {"C:/Windows/Fonts/malgun.ttf", UI_SCRIPT_KO},
};

// The user's UI language picks which script comes first for Han characters all four share (a Japanese or Korean name written in Han
// is the same code points as a Chinese one): kana or Hangul in the text outrank it. The rest follow in a fixed order.
bool ui_cjk_font(int rank, bool kana, bool hangul, char *path, size_t n, int *face) {
    int first = UI_SCRIPT_JA;
    unsigned id = GetUserDefaultUILanguage(), lang = id & 0x3ff, sub = id >> 10;
    if (lang == 0x04) first = sub == 2 || sub == 4 ? UI_SCRIPT_ZH_HANS : UI_SCRIPT_ZH_HANT;  // (Chinese: 0x0804 PRC and 0x1004 Singapore are simplified)
    else if (lang == 0x12) first = UI_SCRIPT_KO;
    if (hangul) first = UI_SCRIPT_KO;
    if (kana) first = UI_SCRIPT_JA;
    int order[4] = {first};
    for (int s = 0, k = 1; s < 4; s++)
        if (s != first) order[k++] = s;
    int seen = 0;
    for (int o = 0; o < 4; o++)
        for (size_t i = 0; i < sizeof cjk_fonts / sizeof *cjk_fonts; i++) {
            if (cjk_fonts[i].script != order[o] || !exists(cjk_fonts[i].path)) continue;
            if (seen++ == rank) {  // (the first file of a script that exists; its other files are not tried)
                *face = 0;
                return (size_t)snprintf(path, n, "%s", cjk_fonts[i].path) < n;
            }
            break;
        }
    return false;
}

bool ui_system_font(char *path, size_t n, int *face) {
    static const char *const known[] = {"C:/Windows/Fonts/segoeui.ttf"};
    for (size_t i = 0; i < sizeof known / sizeof *known; i++) {
        const char *p = known[i];
        if (exists(p)) {
            *face = 0;
            return (size_t)snprintf(path, n, "%s", p) < n;
        }
    }
    return false;
}
