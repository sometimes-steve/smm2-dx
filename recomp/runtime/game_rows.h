// A game's mod settings that are not switches, in the overlay menu's Mods tab after the switches
// (runtime/game_hooks.c for the neutral defaults, runtime/ui/menu.cpp for the drawing, a game's own code to fill
// them in - games/smm2/mods/ocw.c).
//
// One row is a section title (value and text both NULL), a switch (value), or a text field (text; A or a click
// turns it into a field, with the on-screen keyboard for a controller).
//
// A field marked `secret` is not shown: the row says "set" or "not set", and activating it once shows the field so a
// new value can be typed. It is hidden again when the menu closes, so a token is not sitting on screen whenever the
// tab is opened (it never reaches a log either: runtime/net/netcfg.h net_log_secret). Revealing on demand rather
// than masking in place is deliberate - a mask would mean swapping the buffer under a live text field.
//
// The rows are shown whether or not any mod is on, so a setting that has to be entered *before* its mod can work is
// reachable (GameModToggle's rows are hidden when nothing is on).
//
// This struct is shared rather than repeated per file: GameModToggle is copied into each of its users, which is
// survivable for four fields and not for these nine.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *label, *desc;
    bool *value;        // a switch (text NULL)
    bool def;           // its default
    char *text;         // a text field, the game's own buffer (value NULL)
    unsigned long cap;  // its size
    const char *hint;   // shown when the field is empty ("not set")
    bool secret;        // not shown until the row is activated (and hidden again when the menu closes)
    int max_len;        // longest value accepted (0: the buffer's size decides)
} GameModRow;

#ifdef __cplusplus
extern "C" {
#endif
int game_mod_rows(const GameModRow **out);  // how many, and where (runtime/game_hooks.c: none)
void game_mod_rows_changed(void);           // one of them was changed in the menu
// The Online tab (runtime/ui/online_rows.c): is the game's online service set up and in use. 0: no usable config yet;
// 1: online (the config the game started with is the config now); 2: the config changed since the game started (a
// restart applies it); 3: set up, but the service's name does not resolve (it is down, or this machine has no
// internet). runtime/game_hooks.c: 0.
int game_online_state(void);
// A config file the player picked (the one opencourse.world gives out) replaces the current one. False (and nothing
// changed) when it isn't a usable config.
bool game_online_import(const char *path);
// The player's own profile on the online service, for the Online tab: known is false until the maker code has been learned
// (the first profile the game fetches); the numbers come from the service's public API in the background (busy while a
// request is out, have_stats once one answered) and are refreshed about once a minute while the tab is looked at.
typedef struct {
    bool known, busy, have_stats;
    char maker[12];  // "AAA-BBB-CCC"
    char name[64];   // "" until the service says
    unsigned played, cleared, attempts, lives_lost, world_records, first_clears;
    bool have_styles;         // style_clears is filled in (read from the played list, a few seconds after the totals)
    unsigned style_clears[5];  // courses cleared per game style, in ui/history.h order (SMB1, SMB3, SMW, NSMBU, 3DW)
} OnlineProfile;
void game_online_profile(OnlineProfile *out);
// Where the player is on the online service's in-game leaderboards, for the Achievements tab (games/smm2/ocw_leaderboards.py asks
// over NEX, as the game does, in the background once the game is online, and again when asked). The boards, in this
// order: Maker Points Weekly, Maker Points All Time, Clears, First Clear, World Record, then the Endless Challenge's Easy,
// Normal, Expert and Super Expert. rank: the place (1 is the top), 0 not on it (looked through to 999), -1 could not be read, -2 not found yet (the walk is still going). value is the
// player's own figure for the board where it has one (points, clears, the Endless high score), else 0.
#define ONLINE_BOARDS 9
typedef struct {
    bool have, busy;  // have: an answer is there (maybe an old one); busy: a request is out
    int rank[ONLINE_BOARDS];
    unsigned value[ONLINE_BOARDS];
} OnlineBoards;
void game_online_boards(OnlineBoards *out);  // runtime/game_hooks.c: nothing
void game_online_boards_refresh(void);       // ask again (no-op while a run is out): they are asked for once the game is online, then only on this
// The character the player has picked (ui/character.h, issue #25): an index into character_name(), or -1 when
// not known. runtime/game_hooks.c: -1.
int game_current_character(void);
// The game style of the level being played (ui/history.h, issue #25): 0 Super Mario Bros., 1 Super Mario Bros. 3, 2 Super
// Mario World, 3 New Super Mario Bros. U, 4 Super Mario 3D World; -1 when not in a level or not known. runtime/game_hooks.c: -1.
int game_current_style(void);
// The server's answer holds a maker's Mii that is blank or broken (runtime/services/ssl.c finds it as the plaintext reaches the
// game): fills `out` with the Mii to hand the game instead and returns true, or false to leave it as it is (the setting is
// off, or it is the player's own). maker: the 9 characters of the maker code without dashes ("" if the answer has none);
// name: the maker's name there. runtime/game_hooks.c: false.
bool game_replace_bad_mii(const char *maker, const char *name, uint8_t out[0x58]);  // runtime/game_hooks.c: nothing known
// The player's own maker code, the 9 characters without dashes, for the flag they chose (runtime/services/ssl.c gives every maker in
// the server's answers a flag): fills `out` and returns true, or false when it is not known yet. runtime/game_hooks.c: false.
bool game_own_maker(char out[10]);
// The secret rows go back to "set" / "not set" (runtime/ui/menu.cpp calls this when the menu closes; drawn and
// defined by runtime/ui/mods_page.cpp, the Mods tab's Online page)
void game_mod_rows_hide(void);
#ifdef __cplusplus
}
#endif
