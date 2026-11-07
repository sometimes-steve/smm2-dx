// The overlay menu's text font (system_font.h): a system font file ImGui can load. The known paths are tried in order;
// without one the menu falls back to ImGui's built-in 13 px bitmap font.
#include "system_font.h"

#include <stdio.h>
#include <sys/stat.h>

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
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
