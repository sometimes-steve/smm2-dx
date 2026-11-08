// Optional local CharInfo override. The editor exports 88-byte Switch CharInfo, not StoreData.
// No server credentials or network writes belong in this loader.
#pragma once
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

// A Mii from somewhere else than the local file (the OCW mod: the player's Mii from the server), asked for when the file is
// missing or invalid: fills out and returns true, or false when it has none. May block for a moment (a request); it keeps
// its own answer for a while, so asking again is cheap. Set by games/smm2/mods/ocw.c, used by services/mii.c.
void mii_set_remote(bool (*fn)(uint8_t out[0x58]));

// Is this a CharInfo the game can use: a create ID with the UUID variant bits, an eyebrow position the game accepts, and
// every field within its limits (CharInfoRaw); the nickname, UUID and padding are not looked at otherwise.
static int mii_charinfo_valid(const uint8_t data[0x58]) {
    static const uint8_t max[] = {
        3,11,1,127,127,1,3,11,9,11,11,131,99,1,
        59,99,7,6,7,12,18,23,99,8,6,11,12,18,
        17,8,18,35,99,8,6,18,99,5,5,8,16,19,99,7,20,1,8,16,30,255
    };
    for (size_t i = 0; i < sizeof max; i++) if (data[0x26 + i] > max[i]) return 0;
    if ((data[8] & 0xc0) != 0x80 || data[65] < 3) return 0;
    return 1;
}

// 1 = read and valid (in out), 0 = no such file, -1 = there but not a valid CharInfo (out untouched).
static int mii_read_charinfo_file(const char *path, uint8_t out[0x58]) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t data[0x58];
    size_t n = fread(data, 1, sizeof data, f);
    int extra = fgetc(f);
    int failed = ferror(f);
    fclose(f);
    if (failed || n != sizeof data || extra != EOF) return -1;
    if (!mii_charinfo_valid(data)) return -1;
    memcpy(out, data, sizeof data);
    return 1;
}
