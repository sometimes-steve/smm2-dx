// Play time (issue #15): how long the game has been played, on this device only. Counted while the game is running and its
// window has the focus, with the menu closed and no loading screen up. One total for the install (kept in playtime.txt in the
// data folder, written every minute and when the app closes) and the time since the app started.
#pragma once
#include <stdbool.h>

void playtime_tick(bool playing);  // once per presented frame: `playing` says whether this one counts
double playtime_session(void);     // seconds since the app started
double playtime_total(void);       // seconds, all sessions
// "Under 1 min", "45 min", "3 h 12 min"
void playtime_format(double seconds, char *out, unsigned long cap);
