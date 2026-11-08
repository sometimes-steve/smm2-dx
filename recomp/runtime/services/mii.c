// mii:e / mii:u (the console's Mii database): a database holding the one default Mii, as a console always holds at
// least one. BuildDefault / BuildRandom give the same Mii. Calls are logged (the commands follow Ryujinx's
// IDatabaseService, MIT) so a game that needs Miis shows which ones.
//
// SMM2's Course World asks the console for a Mii for the player's maker profile and refuses to go on without one,
// which an empty database could not answer (the player was asked to make a Mii, with no way to). The entry is
// served through Get (CharInfoElement) and Get1 (CharInfo), with the ocw backend only (net/netcfg.h): with any other
// the database stays empty, as before (the recomp's own Course World, games/smm2/mods, needs no Mii). Get2 / Get3 want
// StoreData, the database's own packed record with its checksum, which is not built here: they still answer "none",
// and say so in the log.
//
// The player's own Mii: an 88-byte CharInfo in mii.charinfo in the data root (or where RECOMP_MII_CHARINFO says), which
// the Mii editor next to this project writes (it replaces the file whole, so it is read again at each request: an edit
// shows without a restart). It is served in place of the default Mii when it is a valid CharInfo (mii_charinfo_file.h:
// the exact size, every field in range); a file that is missing or not valid falls back to the default, so the game
// always has a Mii; between the two, the player's Mii from OCW when the OCW mod has one (mii_set_remote: it reads it
// from the server by the maker code in ocw-config.json). IsUpdated says so, once to each session that asked before, when what the database holds changed (the
// file written, replaced or removed), so a game that checks it can refresh what it shows.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../platform.h"
#include "../hle.h"
#include "../ipc.h"
#include "../net/netcfg.h"
#include "mii_charinfo_file.h"

// nn::mii::CharInfo (0x58 bytes, layout as in Ryujinx's CharInfo) of the console's first default Mii (Ryujinx's
// DefaultMii table, entry 2, colours through its Ver3 tables): a valid Mii for games that need one (BuildDefault,
// BuildRandom; mods that give the player a profile). The nickname is UTF-8, up to 10 characters.
void mii_default_charinfo(uint8_t out[0x58], const char *nickname) {
    static const uint8_t face[0x58 - 0x26] = {
        0,  0,  0,  64, 64, 0,  // font region, favourite colour, gender, height, build, type
        0,                      // region move
        0,  0,  0,  0,          // faceline type, colour, wrinkle, make
        33, 1,  0,              // hair type, colour, flip
        2,  8,  4,  3,  4, 2, 12,   // eye type, colour, scale, aspect, rotate, x, y
        6,  1,  4,  3,  6, 2, 10,   // eyebrow type, colour, scale, aspect, rotate, x, y
        1,  4,  9,              // nose type, scale, y
        23, 19, 4,  3,  13,     // mouth type, colour, scale, aspect, y
        8,  0,  0,  4,  10,     // beard colour, beard type, mustache type, scale, y
        0,  8,  4,  10,         // glass type, colour, scale, y
        0,  4,  2,  20,         // mole type, scale, x, y
        0,                      // reserved
    };
    memset(out, 0, 0x58);
    static const uint8_t create_id[16] = {0x52, 0x65, 0x63, 0x6f, 0x6d, 0x70, 0x4d, 0x69,
                                          0x80, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    memcpy(out, create_id, 16);  // (byte 8: the "valid" bits 10xxxxxx)
    const unsigned char *p = (const unsigned char *)(nickname && *nickname ? nickname : "Player");
    for (int n = 0; *p && n < 10; n++) {  // UTF-8 -> UTF-16 (BMP)
        uint32_t cp = *p++;
        if (cp >= 0xC0) {
            int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : 1;
            cp &= 0x3F >> extra;
            while (extra-- && (*p & 0xC0) == 0x80) cp = cp << 6 | (*p++ & 0x3F);
            if (cp > 0xFFFF) cp = '?';
        }
        out[16 + 2 * n] = (uint8_t)cp, out[17 + 2 * n] = (uint8_t)(cp >> 8);
    }
    memcpy(out + 0x26, face, sizeof face);
}

static bool one_mii(void) { return true; }

static bool (*g_remote)(uint8_t out[0x58]);
void mii_set_remote(bool (*fn)(uint8_t out[0x58])) { g_remote = fn; }

// The Mii the database holds: the local file's when it is valid, else the default. Returns a signature of it (changes with
// the Mii), which IsUpdated compares.
static uint64_t current_charinfo(uint8_t out[0x58]) {
    mii_default_charinfo(out, getenv("RECOMP_USER_NAME"));
    const char *path = getenv("RECOMP_MII_CHARINFO");
    int result = mii_read_charinfo_file(path && *path ? path : "mii.charinfo", out);
    int source = result == 1 ? 1 : 0;  // 0 the default, 1 the local file, 2 OCW's
    uint8_t remote[0x58];
    if (!source && g_remote && g_remote(remote)) memcpy(out, remote, 0x58), source = 2;
    static int last_result = 2, last_source = -1;
    if (result != last_result || source != last_source) {
        LOG("[mii] the database holds %s%s\n",
            source == 1 ? "the local mii.charinfo" : source == 2 ? "the player's Mii from OCW" : "the default Mii",
            result < 0 ? " (mii.charinfo is not a valid CharInfo: skipped)" : "");
        last_result = result, last_source = source;
    }
    uint64_t sig = 0xcbf29ce484222325ull;
    for (int i = 0; i < 0x58; i++) sig = (sig ^ out[i]) * 0x100000001b3ull;
    return sig;
}

// The game keeps its own copy of the player's profile Mii (loaded from its save) and uploads THAT to OpenCourse World a few
// seconds after it logs in, whatever this database served it: a save made while only the default Mii was served holds the
// default, and the upload then replaces the player's Mii on OCW with it. So when the Mii served is not the default one, the
// copies of the default Mii the game holds (found in its writable memory by the create id and the face data) are replaced
// with the served Mii, early and again from time to time; the game then uploads, and saves, the right one. Only exact copies
// of the default Mii are touched. RECOMP_MII_NO_HEAL turns it off.
extern int kernel_data_regions(uint64_t *addr, uint64_t *size, int max);
static void *mii_heal_thread(void *arg) {
    (void)arg;
    static uint64_t ra[1024], rs[1024];
    uint8_t def[0x58];
    mii_default_charinfo(def, getenv("RECOMP_USER_NAME"));
    uint64_t t0 = plat_now_ns();
    for (;;) {
        double age = (double)(plat_now_ns() - t0) / 1e9;
        plat_sleep_until_ns(plat_now_ns() + (age < 90 ? 1500000000ull : 20000000000ull));  // (often at first, then now and then)
        uint8_t cur[0x58];
        current_charinfo(cur);
        if (!memcmp(cur, def, sizeof cur)) continue;  // (the default is what is served: nothing to put right)
        int nr = kernel_data_regions(ra, rs, 1024), fixed = 0;
        for (int r = 0; r < nr; r++) {
            uint8_t *p = (uint8_t *)(uintptr_t)ra[r], *end = p + rs[r];
            while (p + 0x58 <= end && (p = memchr(p, def[0], (size_t)(end - p - 0x57))) != NULL) {
                if (!memcmp(p, def, 16) && !memcmp(p + 0x26, def + 0x26, 0x32)) memcpy(p, cur, 0x58), fixed++;
                p++;
            }
        }
        if (fixed) LOG("[mii] put the player's Mii in %d place%s where the game kept the default one (%.0f s)\n", fixed, fixed == 1 ? "" : "s", age);
    }
    return NULL;
}
static void mii_heal_start(void) {
    static bool started;
    if (started || getenv("RECOMP_MII_NO_HEAL")) return;
    started = true;
    pthread_t th;
    if (pthread_create(&th, NULL, mii_heal_thread, NULL) == 0) pthread_detach(th);
}

static void database(Service *s, IpcReq *req, IpcResp *resp) {
    switch (req->cmd) {
    case 6: case 7:  // BuildRandom, BuildDefault -> CharInfo: the default Mii
        LOG("[mii] %s asked for: answering with the default Mii\n", req->cmd == 6 ? "BuildRandom" : "BuildDefault");
        mii_default_charinfo(resp->data, getenv("RECOMP_USER_NAME"));
        resp_size(resp, 0x58);
        break;
    case 0: {  // IsUpdated(flag): the Mii changed since this session last looked (its state: the signature it saw)
        uint8_t now[0x58];
        uint64_t sig = current_charinfo(now);
        bool changed = (uint64_t)(uintptr_t)s->state != sig;
        s->state = (void *)(uintptr_t)sig;
        if (changed) LOG("[mii] IsUpdated: the Mii changed since the last look\n");
        OUT(uint8_t, 0) = changed;
        resp_size(resp, 1);
        break;
    }
    case 1: case 20: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // IsFullDatabase, IsBroken..
    case 2: OUT(uint32_t, 0) = one_mii(); resp_size(resp, 4); break;           // GetCount(flag): the one Mii
    case 3: case 4: {  // Get -> CharInfoElement[] (CharInfo + source), Get1 -> CharInfo[]; both return the count
        IpcBuf *b = out_buf(req, 0);
        size_t want = req->cmd == 3 ? 0x5c : 0x58;  // (CharInfoElement: CharInfo then u32 source, 0 = the database)
        int32_t n = 0;
        if (!one_mii()) b = NULL;  // (the empty database)
        if (b && b->addr && b->size >= want) {
            uint8_t *p = (uint8_t *)(uintptr_t)b->addr;
            memset(p, 0, want);
            s->state = (void *)(uintptr_t)current_charinfo(p);  // (what this session has seen)
            LOG("[mii] Get%s served the database's Mii\n", req->cmd == 3 ? "" : "1");
            mii_heal_start();
            n = 1;
        } else if (b) {
            LOG("[mii] Get%s: the game's buffer is %llu bytes, one entry needs %zu\n", req->cmd == 3 ? "" : "1",
                (unsigned long long)b->size, want);
        }
        OUT(int32_t, 0) = n;
        resp_size(resp, 4);
        break;
    }
    case 8: case 9:  // Get2 -> StoreDataElement[], Get3 -> StoreData[]: not built here
        LOG("[mii] Get%d (StoreData) asked for: answering \"none\" (the database's packed record is not built)\n",
            req->cmd == 8 ? 2 : 3);
        OUT(int32_t, 0) = 0;
        resp_size(resp, 4);
        break;
    case 22: break;                                                            // SetInterfaceVersion
    default: LOG("[mii] IDatabaseService cmd %u (unhandled, ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static void static_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: {  // GetDatabaseService: a session that has seen the Mii as it is now
        uint8_t now[0x58];
        ipc_return_object(resp, service_new("mii:IDatabaseService", database, (void *)(uintptr_t)current_charinfo(now)));
        break;
    }
    default: LOG("[mii] IStaticService cmd %u (ok)\n", req->cmd);
    }
}

SERVICE("mii:e", mii_e) { static_service(s, req, resp); }
SERVICE("mii:u", mii_u) { static_service(s, req, resp); }
