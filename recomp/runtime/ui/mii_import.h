// Mii Editor > Import from a link (mii_import.c): a miicharacters.com Mii page in, a CharInfo out.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum { MII_IMP_IDLE, MII_IMP_BUSY, MII_IMP_DONE, MII_IMP_ERROR } MiiImportState;

// Starts reading the Mii at `link` (https://www.miicharacters.com/mii/23844, with or without www / https) on a background
// thread; returns at once. False (with `why`) when the text isn't such a link or one is already being read.
int mii_import_start(const char *link, char *why, int cap);

// BUSY while it works. DONE (once: the result is handed over) fills `out` (a CharInfo; the create id is the default Mii's:
// the caller makes a new one), `name` (UTF-8) and the Mii's picture as a JPEG (*img, malloc'd, the caller frees it; NULL when
// the page had none); ERROR (once) fills `why`. IDLE when nothing was started.
MiiImportState mii_import_poll(uint8_t out[0x58], char name[48], char why[160], uint8_t **img, size_t *img_len);
