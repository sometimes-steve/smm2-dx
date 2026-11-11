// Which character the player has picked (Mario, Luigi, Toad, Toadette) and the game style of the level being played, for the
// Stats tab (runtime/ui/character.c, runtime/ui/history.c, issue #25).
// Found by switching the character in the game and seeing which words followed: this one reads 0 / 1 / 2 / 3 for them, in the
// game's heap, which allocates the same way on every start (the scene manager's address in skip_intro.c is a heap one too).
// The style is found the same way, by loading a level in each style: 0 Super Mario Bros., 1 Super Mario Bros. 3, 2 Super Mario
// World, 3 New Super Mario Bros. U, 4 Super Mario 3D World; the word reads 0xffffffff while no level is loaded.
#include "mod.h"

#define CHARACTER_WORD 0x72455fb00cull
#define STYLE_WORD 0x72541bda00ull

int game_current_character(void) {
    static int last = -2;
    uint32_t v = g32(CHARACTER_WORD);
    int who = v <= 3 ? (int)v : -1;
    if (who != last) LOG("[mod] character: %d\n", who), last = who;
    return who;
}

int game_current_style(void) {
    static int last = -2;
    uint32_t v = g32(STYLE_WORD);
    int style = v <= 4 ? (int)v : -1;
    if (style != last) LOG("[mod] game style: %d\n", style), last = style;
    return style;
}
