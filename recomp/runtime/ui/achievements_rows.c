// The Achievements tab (runtime/ui/menu.c tab_achievements): for now the leaderboard entries, the player's place on each of
// OpenCourse World's in-game leaderboards (more kinds of achievement come later). The places come over NEX, as the game's own
// boards do (game_online_boards, games/smm2/ocw_leaderboards.py), looking through to rank 999. Plain C.
#include "ui/ig.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>

#include "../game_rows.h"
#include "cards.h"

// A small arc going round, in place of a place that is not known yet
static void spin(ImDrawList *dl, float cx, float cy, float r, ImU32 col, float t) {
    ImVec2 pts[14];
    for (int i = 0; i < 14; i++) {
        float a = t + i * (4.2f / 13);
        pts[i] = IM_VEC2(cx + r * cosf(a), cy + r * sinf(a));
    }
    ImDrawList_AddPolyline(dl, pts, 14, col, 0, 2.0f);
}
static void spinner(const Card *c, float y) {
    float r = c->fs * 0.32f;
    spin(c->dl, c->x + c->w - c->fs * 0.7f - r, y + c->fs * 0.5f, r, menu_muted(), (float)(SDL_GetTicks() / 1000.0 * 5.0));
}

// The refresh button, small and quiet at the top right of the tab: a circular arrow (a spinning arc while a run is out).
// Does not take any space: the page's first box starts at the top as before (a narrow page, one column, is moved down).
static bool refresh_button(float fs, bool busy, bool narrow) {
    ImVec2 p, av;
    igGetCursorScreenPos(&p);
    igGetContentRegionAvail(&av);
    float s = fs * 1.5f;
    ImVec2 back;
    igGetCursorPos(&back);
    igSetCursorScreenPos(IM_VEC2(p.x + av.x - s, p.y));
    igPushID_Str("##boards_refresh");
    igPushStyleColor_Vec4(ImGuiCol_NavCursor, IM_VEC4(0, 0, 0, 0));
    bool clicked = igInvisibleButton("##r", IM_VEC2(s, s), ImGuiButtonFlags_EnableNav);
    igPopStyleColor(1);
    igPopID();
    bool hot = igIsItemHovered(0) || igIsItemFocused();
    ImDrawList *dl = igGetWindowDrawList();
    ImU32 col = hot && !busy ? igGetColorU32_Col(ImGuiCol_Text, 1.0f) : menu_muted();
    float cx = p.x + av.x - s * 0.5f, cy = p.y + s * 0.5f, r = fs * 0.4f;
    if (busy) spin(dl, cx, cy, r, col, (float)(SDL_GetTicks() / 1000.0 * 5.0));
    else {  // an open circle with an arrowhead at its end
        spin(dl, cx, cy, r, col, 0.6f);
        float a = 0.6f + 4.2f, tx = cx + r * cosf(a), ty = cy + r * sinf(a), d = fs * 0.28f;
        ImDrawList_AddTriangleFilled(dl, IM_VEC2(tx + d * cosf(a + 1.2f), ty + d * sinf(a + 1.2f)),
                                     IM_VEC2(tx + d * cosf(a + 2.6f), ty + d * sinf(a + 2.6f)),
                                     IM_VEC2(tx - d * 0.2f * cosf(a + 1.9f) + d * 0.9f * cosf(a + 1.57f),
                                             ty - d * 0.2f * sinf(a + 1.9f) + d * 0.9f * sinf(a + 1.57f)), col);
    }
    igSetCursorPos(back);
    if (narrow) igDummy(IM_VEC2(1, s));
    return clicked && !busy;
}

void achievements_rows(void) {
    float fs = igGetFontSize();
    igDummy(IM_VEC2(1, fs * 0.2f));
    if (game_online_state() == 0) {
        Card c = card_begin("Leaderboards", rows_h(fs, 1));
        card_row(&c, "Add your OpenCourse World config in the Online tab", "");
        card_end(&c);
        return;
    }
    OnlineBoards b;
    game_online_boards(&b);
    static const char *const names[ONLINE_BOARDS] = {"Maker Points (Weekly)", "Maker Points (All Time)", "Clears", "First Clear",
                                                     "World Records", "Endless (Easy)", "Endless (Normal)", "Endless (Expert)",
                                                     "Endless (Super Expert)"};
    ImVec2 av;
    igGetContentRegionAvail(&av);
    int cols = av.x > fs * 38 ? 2 : 1;  // (as the Stats tab: a box is no wider than a column)
    if (refresh_button(fs, b.busy, cols == 1)) game_online_boards_refresh();
    if (!igBeginTable("##boards", cols, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX, IM_VEC2(0, 0), 0)) return;
    igTableNextColumn();
    Card c = card_begin("Leaderboards", rows_h(fs, ONLINE_BOARDS));
    for (int i = 0; i < ONLINE_BOARDS; i++) {
        char v[16];
        bool looking = !b.have || b.rank[i] == -2;  // (not known yet, or still looking)
        if (looking) v[0] = 0;
        else if (b.rank[i] > 0) snprintf(v, sizeof v, "#%d", b.rank[i]);
        else snprintf(v, sizeof v, "-");  // (not on it, or could not be read)
        float y = c.y;
        card_row(&c, names[i], v);
        if (looking) spinner(&c, y);
    }
    card_end(&c);
    igEndTable();
    igDummy(IM_VEC2(1, fs * 0.5f));
}
