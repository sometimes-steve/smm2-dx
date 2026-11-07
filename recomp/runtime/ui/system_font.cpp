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
    static const char *const known[] = {"/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc",
                                        "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                        "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                                        "/system/fonts/Roboto-Regular.ttf"};
    for (const char *p : known)
        if (exists(p)) {
            *face = 0;
            return (size_t)snprintf(path, n, "%s", p) < n;
        }
    return false;
}
