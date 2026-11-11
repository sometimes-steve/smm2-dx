// What the Stats tab keeps about how the game is played, on this device only (issue #25): sessions (how long, how many, days
// in a row), play time per day, per weekday and hour and per game style. One small file, history.txt in the data folder,
// written every minute and when the app closes, left out of release zips. Every time here is the play time counter's
// (ui/playtime.h: nothing here runs a timer of its own).
#pragma once
#include <stdbool.h>

void history_tick(void);  // once per presented frame, after playtime_tick
double history_longest_session(void);   // seconds
double history_average_session(void);   // seconds; 0 until a session has been played
int history_sessions(void);             // sessions played (a start of the app that was played in)
int history_days_played(void);
int history_streak(void);               // days in a row up to today (or yesterday, while today has not been played yet)
int history_best_streak(void);

// Play time per calendar day (the last 30 are kept): days_ago 0 is today. history_weekday: 0 = Sunday ... 6, of that day.
double history_day_seconds(int days_ago);
int history_weekday(int days_ago);
// Play time by weekday (0 = Sunday) and hour of the day (0..23), everything recorded
double history_heat(int weekday, int hour);
// Play time per game style (issue #25), counted while game_current_style() says which one is in play
#define HISTORY_STYLES 5
const char *history_style_name(int id);  // "Super Mario Bros.", "Super Mario Bros. 3", "Super Mario World", ...
double history_style_seconds(int id);
