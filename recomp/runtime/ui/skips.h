// Endless Challenge skips (issue #32): how many courses the player has skipped, on this device only. One total for the install (kept in
// endless-skips.txt in the data folder, written at every skip and when the app closes) and the count since the app started.
#pragma once

void skips_note(void);       // an Endless skip happened (games/smm2/mods/endless_skips.c)
int skips_session(void);     // skips since the app started
long skips_total(void);      // skips, all sessions
