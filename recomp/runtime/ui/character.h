// Time played per character (issue #25): which character the player uses the most, on this device only. The time is the
// play time counter's (ui/playtime.h: nothing here runs a timer of its own), added to the character the game says is in
// play (game_current_character, runtime/game_rows.h). One total per character in character.txt in the data folder,
// written every minute and when the app closes, left out of release zips.
#pragma once
#include <stdbool.h>

#define CHARACTER_COUNT 4
const char *character_name(int id);      // "Mario", "Luigi", "Toad", "Toadette"; "" for an id out of range
void character_tick(void);               // once per presented frame, after playtime_tick
double character_seconds(int id);        // all sessions
// The character with the most time: its name in `name` and its time ("3 h 12 min") in `time`; false when none has any.
bool character_most_played(char *name, unsigned long name_cap, char *time, unsigned long time_cap);
