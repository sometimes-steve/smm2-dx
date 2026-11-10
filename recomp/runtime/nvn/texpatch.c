// Texture edits on upload (texpatch.h). The shared runtime knows no textures except the one this game needs changed:
// the title screen's corner line "(c) 2019 Nintendo" is not text but a picture, the texture Title_Claim_00 (384 x 60, BC7)
// of the layout Play_Title_00 (issue #14). It is recognised when the game uploads it - by its size and a hash of its bytes
// (the hash is all that is kept of it) - and its blocks are replaced by the project's own line, recomp/tools/gen_title_claim.py
// -> title_claim.inc. Nothing of the game's files is changed or shipped, and a texture that doesn't match is left alone.
#include <stdint.h>
#include <string.h>

#include "texpatch.h"

void rt_log(const char *fmt, ...);

#include "title_claim.inc"

enum { CLAIM_ID = 0, CLAIM_W = 384, CLAIM_H = 60, CLAIM_BYTES = 24576 };  // (its block-linear storage: 96 x 15 blocks, padded)
#define CLAIM_HASH 0x8346e903c886e6f1ull

int texpatch_identify(const NvnTexture *t) {
    if (t->width != CLAIM_W || t->height != CLAIM_H || !t->addr || nvn_texture_storage_size(t) < CLAIM_BYTES) return -1;
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *p = (const uint8_t *)(uintptr_t)t->addr;
    for (size_t i = 0; i < CLAIM_BYTES; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    static bool logged_match, logged_other;  // (each said once: the log shows whether the corner line was found)
    bool match = h == CLAIM_HASH;
    if (match ? !logged_match : !logged_other) {
        rt_log("[texpatch] a 384 x 60 texture: hash %016llx (%s)\n", (unsigned long long)h,
               match ? "the title screen's corner line: replaced" : "not the title screen's corner line");
        if (match) logged_match = true;
        else logged_other = true;
    }
    return h == CLAIM_HASH ? CLAIM_ID : -1;
}
int texpatch_bottle_kind(int id) { (void)id; return 0; }
bool texpatch_is_item_cursor(int id) { (void)id; return false; }
uint32_t texpatch_generation(void) { return 0; }
void texpatch_apply(int id, uint32_t lv, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t row) {
    if (id != CLAIM_ID || lv != 0 || w != CLAIM_W || h != CLAIM_H) return;
    const uint32_t wb = CLAIM_W / 4, hb = CLAIM_H / 4;
    for (uint32_t y = 0; y < hb; y++) memcpy(blocks + (uint64_t)y * row, k_title_claim + y * wb * 16, wb * 16);
}
int texpatch_heart_kind(int id) { (void)id; return 0; }
void texpatch_heart_tint(int kind, float tint[8]) { (void)kind, (void)tint; }
