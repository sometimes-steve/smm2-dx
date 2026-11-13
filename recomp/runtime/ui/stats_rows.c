// The Stats tab (runtime/ui/menu.c tab_stats, issue #25): cards in two columns (one on a narrow window) - play time, sessions,
// characters, game styles, the totals OpenCourse World keeps for the player and what is worked out from them - then the
// last two weeks and the hours played, across the full width. Play time, sessions, characters, styles, days and hours are
// kept on this device only. Every card can take the controller's cursor, which is how the page scrolls with one. Plain C.
#include "ui/ig.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../game.h"  // GAME_TITLE
#include "../game_rows.h"
#include "cards.h"
#include "character.h"
#include "history.h"
#include "playtime.h"



// A card: a focusable box of height h with its title, the width of the space left. Rows go in with card_row at c.y.
Card card_begin(const char *title, float h) {
    Card c;
    c.fs = igGetFontSize();
    ImVec2 p, av;
    igGetCursorScreenPos(&p);
    igGetContentRegionAvail(&av);
    c.x = p.x, c.top = p.y, c.w = av.x, c.dl = igGetWindowDrawList();
    igPushID_Str(title);
    igPushStyleColor_Vec4(ImGuiCol_NavCursor, IM_VEC4(0, 0, 0, 0));
    igSetNextItemAllowOverlap();  // (buttons inside the box, the refresh arrow and Find a Course, are clicked over it)
    igInvisibleButton("##card", IM_VEC2(c.w, h), ImGuiButtonFlags_EnableNav);
    igPopStyleColor(1);
    igPopID();
    bool hot = igIsItemHovered(0) || igIsItemFocused();
    ImDrawList_AddRectFilled(c.dl, IM_VEC2(p.x, p.y), IM_VEC2(p.x + c.w, p.y + h), menu_accent(hot ? 44 : 22), c.fs * 0.3f, 0);
    ImDrawList_AddRect(c.dl, IM_VEC2(p.x, p.y), IM_VEC2(p.x + c.w, p.y + h), menu_accent(hot ? 190 : 80), c.fs * 0.3f, 0, 1.5f);
    ImDrawList_AddText_Vec2(c.dl, IM_VEC2(p.x + c.fs * 0.7f, p.y + c.fs * 0.5f), menu_muted(), title, NULL);
    ImDrawList_AddLine(c.dl, IM_VEC2(p.x + c.fs * 0.7f, p.y + c.fs * 1.75f), IM_VEC2(p.x + c.w - c.fs * 0.7f, p.y + c.fs * 1.75f), menu_accent(70), 1.0f);  // (a hairline under the title, as the option pages' headings)
    c.y = p.y + c.fs * 2.1f;
    return c;
}
float rows_h(float fs, int n) { return fs * 2.1f + n * fs * 1.5f + fs * 0.4f; }
void card_end(Card *c) { igDummy(IM_VEC2(1, c->fs * 0.2f)); }

float text_w(const char *s) {
    ImVec2 v;
    igCalcTextSize(&v, s, NULL, false, -1.0f);
    return v.x;
}

void card_row(Card *c, const char *label, const char *value) {
    ImU32 text = igGetColorU32_Col(ImGuiCol_Text, 1.0f);
    ImDrawList_AddText_Vec2(c->dl, IM_VEC2(c->x + c->fs * 0.7f, c->y), menu_muted(), label, NULL);
    ImDrawList_AddText_Vec2(c->dl, IM_VEC2(c->x + c->w - c->fs * 0.7f - text_w(value), c->y), text, value, NULL);
    c->y += c->fs * 1.5f;
}

// bars: one per item, as long as its share of the longest, with its time (or its count) at the right
static void card_bars(Card *c, int n, const char *const *names, const double *secs, bool counts) {
    double max = 0;
    for (int i = 0; i < n; i++) if (secs[i] > max) max = secs[i];
    float lab = 0;
    for (int i = 0; i < n; i++) lab = fmaxf(lab, text_w(names[i]));
    float x0 = c->x + c->fs * 0.7f + lab + c->fs * 0.6f, x1 = c->x + c->w - c->fs * 0.7f - c->fs * 6.0f;
    if (x1 < x0 + c->fs * 2) x1 = x0 + c->fs * 2;
    ImU32 text = igGetColorU32_Col(ImGuiCol_Text, 1.0f);
    for (int i = 0; i < n; i++) {
        char t[32];
        if (counts) snprintf(t, sizeof t, "%.0f", secs[i]);
        else playtime_format(secs[i], t, sizeof t);
        ImDrawList_AddText_Vec2(c->dl, IM_VEC2(c->x + c->fs * 0.7f, c->y), menu_muted(), names[i], NULL);
        float top = c->y + c->fs * 0.25f, bot = c->y + c->fs * 1.0f;
        ImDrawList_AddRectFilled(c->dl, IM_VEC2(x0, top), IM_VEC2(x1, bot), menu_accent(28), 2, 0);
        if (max > 0 && secs[i] > 0)
            ImDrawList_AddRectFilled(c->dl, IM_VEC2(x0, top), IM_VEC2(x0 + (x1 - x0) * (float)(secs[i] / max), bot),
                                     menu_accent(secs[i] >= max ? 255 : 150), 2, 0);
        ImDrawList_AddText_Vec2(c->dl, IM_VEC2(c->x + c->w - c->fs * 0.7f - text_w(t), c->y), text, t, NULL);
        c->y += c->fs * 1.5f;
    }
}

static void ratio(char *out, unsigned long cap, double num, unsigned den, const char *unit, bool percent) {
    if (!den) snprintf(out, cap, "-");  // (nothing to divide by yet)
    else if (percent) snprintf(out, cap, "%.1f%%", 100.0 * num / den);
    else snprintf(out, cap, "%.1f %s", num / den, unit);
}

static void card_play_time(void) {
    float fs = igGetFontSize();
    Card c = card_begin("Play Time", rows_h(fs, 4));
    char v[48];
    playtime_format(playtime_session(), v, sizeof v);
    card_row(&c, "This Session", v);
    playtime_format(playtime_total(), v, sizeof v);
    card_row(&c, "All Time", v);
    snprintf(v, sizeof v, "%d", history_days_played());
    card_row(&c, "Days Played", v);
    snprintf(v, sizeof v, "%d day%s  (best %d)", history_streak(), history_streak() == 1 ? "" : "s", history_best_streak());
    card_row(&c, "Days in a Row", v);
    card_end(&c);
}

static void card_sessions(void) {
    float fs = igGetFontSize();
    Card c = card_begin("Sessions", rows_h(fs, 3));
    char v[48];
    playtime_format(history_longest_session(), v, sizeof v);
    card_row(&c, "Longest Session", v);
    playtime_format(history_average_session(), v, sizeof v);
    card_row(&c, "Average Session", v);
    snprintf(v, sizeof v, "%d", history_sessions());
    card_row(&c, "Sessions Played", v);
    card_end(&c);
}

static void card_characters(void) {
    float fs = igGetFontSize();
    Card c = card_begin("Characters", rows_h(fs, CHARACTER_COUNT));
    const char *names[CHARACTER_COUNT];
    double secs[CHARACTER_COUNT];
    for (int i = 0; i < CHARACTER_COUNT; i++) names[i] = character_name(i), secs[i] = character_seconds(i);
    card_bars(&c, CHARACTER_COUNT, names, secs, false);
    card_end(&c);
}

static void card_styles(const OnlineProfile *pr) {
    float fs = igGetFontSize();
    Card c = card_begin("Game Styles (Clears)", rows_h(fs, HISTORY_STYLES));
    const char *names[HISTORY_STYLES];
    double n[HISTORY_STYLES];
    for (int i = 0; i < HISTORY_STYLES; i++) names[i] = history_style_name(i), n[i] = pr->have_styles ? pr->style_clears[i] : 0;
    card_bars(&c, HISTORY_STYLES, names, n, true);
    card_end(&c);
}

static void card_totals(const OnlineProfile *pr) {
    float fs = igGetFontSize();
    if (!pr->known) {
        Card c = card_begin("OpenCourse World", rows_h(fs, 1));
        card_row(&c, "Maker Code", "Not known yet");
        card_end(&c);
        return;
    }
    Card c = card_begin("OpenCourse World", rows_h(fs, 6));
    const char *none = pr->busy ? "Loading..." : "Unavailable";
    struct { const char *label; unsigned n; } st[] = {{"Courses Played", pr->played}, {"Courses Cleared", pr->cleared},
                                                      {"Attempts", pr->attempts}, {"Lives Lost", pr->lives_lost},
                                                      {"World Records", pr->world_records}, {"First Clears", pr->first_clears}};
    for (int i = 0; i < 6; i++) {
        char v[32];
        if (pr->have_stats) snprintf(v, sizeof v, "%u", st[i].n);
        else snprintf(v, sizeof v, "%s", none);
        card_row(&c, st[i].label, v);
    }
    card_end(&c);
}

static void card_clear_stats(const OnlineProfile *pr) {
    float fs = igGetFontSize();
    Card c = card_begin("Clear Stats", rows_h(fs, 3));
    char v[48];
    bool have = pr->known && pr->have_stats;
    ratio(v, sizeof v, have ? pr->cleared : 0, have ? pr->played : 0, "", true);
    card_row(&c, "Clear Rate", v);
    ratio(v, sizeof v, have ? pr->attempts : 0, have ? pr->cleared : 0, "attempts", false);
    card_row(&c, "Attempts per Clear", v);
    ratio(v, sizeof v, have ? pr->lives_lost : 0, have ? pr->cleared : 0, "lives", false);
    card_row(&c, "Lives Lost per Clear", v);
    card_end(&c);
}

// the last 14 days, a bar each (today at the right), the weekday's first letter under it
#define TREND_H(fs) ((fs) * 10.1f)  // (the box's natural height)
static void card_trend(float box_h) {  // box_h: the box's height (the one beside it may be taller)
    enum { DAYS = 14 };
    float fs = igGetFontSize(), plot = box_h - fs * 4.1f;
    Card c = card_begin("Last 14 Days", box_h);
    double v[DAYS], max = 0, total = 0;
    for (int i = 0; i < DAYS; i++) {
        v[i] = history_day_seconds(DAYS - 1 - i);
        total += v[i];
        if (v[i] > max) max = v[i];
    }
    char t[48], line[96];
    float left = c.x + fs * 0.7f, right = c.x + c.w - fs * 0.7f, slot = (right - left) / DAYS, bw = slot * 0.62f;
    float base = c.y + plot;
    static const char letters[] = "SMTWTFS";
    ImU32 text = igGetColorU32_Col(ImGuiCol_Text, 1.0f);
    ImDrawList_AddLine(c.dl, IM_VEC2(left, base), IM_VEC2(right, base), menu_accent(90), 1.0f);
    for (int i = 0; i < DAYS; i++) {
        float x = left + slot * i + (slot - bw) * 0.5f;
        if (max > 0 && v[i] > 0) {
            float h = fmaxf(2.0f, plot * (float)(v[i] / max));
            ImDrawList_AddRectFilled(c.dl, IM_VEC2(x, base - h), IM_VEC2(x + bw, base), menu_accent(v[i] >= max ? 255 : 160), 2, 0);
        }
        char d[2] = {letters[history_weekday(DAYS - 1 - i)], 0};
        ImDrawList_AddText_Vec2(c.dl, IM_VEC2(x + (bw - text_w(d)) * 0.5f, base + fs * 0.3f), i == DAYS - 1 ? text : menu_muted(), d, NULL);
    }
    if (max > 0) {  // (at the right of the title's line: the left is the title's)
        char best[48];
        playtime_format(max, best, sizeof best);
        playtime_format(total, t, sizeof t);
        snprintf(line, sizeof line, "Best %s  Total %s", best, t);
    } else {
        snprintf(line, sizeof line, "Nothing recorded yet");
    }
    ImDrawList_AddText_Vec2(c.dl, IM_VEC2(c.x + c.w - fs * 0.7f - text_w(line), c.y - fs * 1.6f), menu_muted(), line, NULL);
    card_end(&c);
}

// when it is played: a cell for each hour of each weekday (Monday first); darker is more
static float heat_cell(float fs, float width) {  // a cell's size in a box `width` wide
    float lab = text_w("Mon") + fs * 0.5f, cell = fminf((width - fs * 1.4f - lab) / 24, fs * 1.5f);
    return cell < 4 ? 4 : cell;
}
static void card_heatmap(float box_h) {  // box_h: the box's height, at least its own
    float fs = igGetFontSize();
    ImVec2 av;
    igGetContentRegionAvail(&av);
    float lab = text_w("Mon") + fs * 0.5f, cell = heat_cell(fs, av.x);
    Card c = card_begin("When You Play", fmaxf(box_h, fs * 2.1f + cell * 7 + fs * 1.9f));
    static const char *const days[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    double max = 0;
    for (int w = 0; w < 7; w++)
        for (int h = 0; h < 24; h++) max = fmax(max, history_heat(w, h));
    float x0 = c.x + fs * 0.7f + lab;
    for (int r = 0; r < 7; r++) {
        int wd = (r + 1) % 7;  // (Monday first; history's weekdays count from Sunday)
        float y = c.y + cell * r;
        ImDrawList_AddText_Vec2(c.dl, IM_VEC2(c.x + fs * 0.7f, y + (cell - fs) * 0.5f), menu_muted(), days[r], NULL);
        for (int h = 0; h < 24; h++) {
            double s = history_heat(wd, h);
            int a = max > 0 && s > 0 ? 50 + (int)(205 * sqrt(s / max)) : 18;
            ImDrawList_AddRectFilled(c.dl, IM_VEC2(x0 + cell * h + 1, y + 1), IM_VEC2(x0 + cell * (h + 1) - 1, y + cell - 1),
                                     menu_accent(a), 2, 0);
        }
    }
    for (int h = 0; h < 24; h += 3) {
        char t[8];
        snprintf(t, sizeof t, "%d", h);
        ImDrawList_AddText_Vec2(c.dl, IM_VEC2(x0 + cell * h, c.y + cell * 7 + fs * 0.3f), menu_muted(), t, NULL);
    }
    const char *note = max > 0 ? "Darker is more time" : "Nothing recorded yet";
    ImDrawList_AddText_Vec2(c.dl, IM_VEC2(c.x + c.w - fs * 0.7f - text_w(note), c.y - fs * 1.6f), menu_muted(), note, NULL);
    card_end(&c);
}

void stats_rows(void) {
    float fs = igGetFontSize();
    ImVec2 av;
    igGetContentRegionAvail(&av);
    OnlineProfile pr;
    game_online_profile(&pr);
    igDummy(IM_VEC2(1, fs * 0.2f));
    int cols = av.x > fs * 38 ? 2 : 1;  // (a narrow window: one column)
    if (igBeginTable("##stats", cols, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX, IM_VEC2(0, 0), 0)) {
        void (*simple[])(void) = {card_play_time, card_sessions, card_characters};
        for (int i = 0; i < 3; i++) igTableNextColumn(), simple[i]();
        igTableNextColumn();
        card_styles(&pr);
        igTableNextColumn();
        card_totals(&pr);
        igTableNextColumn();
        card_clear_stats(&pr);
        igEndTable();
    }
    // the last 14 days and when you play, side by side in a wide window (the one box as tall as the other)
    if (igBeginTable("##stats2", cols, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX, IM_VEC2(0, 0), 0)) {
        igTableNextColumn();
        ImVec2 col;
        igGetContentRegionAvail(&col);
        float h = TREND_H(fs), hh = fs * 2.1f + heat_cell(fs, col.x) * 7 + fs * 1.9f;
        if (hh > h) h = hh;
        card_trend(h);
        igTableNextColumn();
        card_heatmap(h);
        igEndTable();
    }
    igDummy(IM_VEC2(1, fs * 0.5f));
}
