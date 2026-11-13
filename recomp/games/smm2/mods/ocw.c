// OpenCourse World: the game's own online client against the player's own OCW account, as the Atmosphère / Ryujinx
// mod does it on a console (_vendor/ocw-client-mod). The player signs in at opencourse.world, accepts its terms and
// is given a domain and a personal token; those go in ocw-config.json in the data folder, next to smm2mods.ini:
//
//     { "domain": "server.opencourse.world", "token": "<22 characters>", "ip": "10.0.0.5" }
//
//   domain  the host written into the game, so the game looks it up, sends it as Host: and names it for SNI
//           (at most 29 characters: the string slot in the game's own binary). May itself be an address.
//   token   the account's token, exactly 22 characters, carried in the WebSocket handshake
//   ip      optional: the address `domain` resolves to, instead of asking the host resolver. This is how a
//           self-hosted or local OCW instance is reached, and how an IP-only set-up keeps a real name for SNI.
//
// The player's own Mii: the game's Mii database holds the Mii the player has set on OCW (read from OCW's public maker API)
// until a local mii.charinfo replaces it (services/mii.c). Whose it is is learned, not configured: the first user record
// the game fetches in a session is the player's own profile (see "the player's Mii" below). Before the game boots,
// the setup (ui/launcher.c, games/smm2/ocw_identity.py) asks OCW who the token belongs to and writes ocw-maker.txt, so
// the Mii the game asks for at boot is already the player's own; the learning below then only covers a failed lookup.
//
// Only two of the console mod's ten patches are needed here. Its other eight stub nn::account calls that reach real
// Nintendo services on a console; in the recomp those reach runtime/services/acc.c, which already reports a network
// service account and hands out a local ID token whenever the game's own sockets are on.
//
//   1. the domain, written over the game's own host string once the modules are loaded (below; the whole mapping is
//      read/write, runtime/loader.c)
//   2. the WebSocket handshake, built by the host instead of the game's own snprintf so the token rides along
//      (smm2_ocw_header, reached from the one patch in mods/patches.py)
//
// Nothing here does anything unless a usable ocw-config.json is present: the game is then unmodified and goes nowhere, exactly as before.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../../../runtime/game_rows.h"
#include "../../../runtime/net/json.h"
#include "../../../runtime/net/netcfg.h"
#include "../../../runtime/net/http.h"
#include "../../../runtime/kernel.h"
#include "../../../runtime/gamebuild.h"
#include "../../../runtime/net/online.h"
#include "../../../runtime/paths.h"
#include "../../../runtime/platform.h"
#include "../../../runtime/services/mii_charinfo_file.h"
#include "mod.h"
#include "../../../runtime/settings.h"
#include "ocw.h"

// The game's own host string (v3.0.3). The console mod writes the same 29 bytes at main+0x22cc307.
#define OCW_DOMAIN_OFF 0x22cc307
#define OCW_DOMAIN_MAX 29  // the slot's size: the domain is zero-padded to it, as the mod does
#define OCW_TOKEN_LEN 22   // the mod rejects any other length
#define OCW_MOD_VERSION "0.2"
#define OCW_GAME_VERSION "3.0.3"

static struct {
    bool active;                       // config read: the hook and the domain write are live
    char domain[OCW_DOMAIN_MAX + 1];
    char token[OCW_TOKEN_LEN + 1];
    char ip[64];
} g_ocw;

static char g_live_domain[OCW_DOMAIN_MAX + 1], g_live_token[OCW_TOKEN_LEN + 1], g_live_ip[64];  // what the game started with

static const char *config_path(void) {
    const char *e = getenv("RECOMP_OCW_CONFIG");
    return e && *e ? e : "ocw-config.json";
}

static void boards_start_at_boot(void);  // (below, with the leaderboards)
// Read and check ocw-config.json. False (logged, with what to do about it) leaves everything inert: unlike the
// console mod, which aborts the game when its config is missing, a bad config here must not stop the game.
static bool config_load_from(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG("[ocw] no %s in the data folder: Course World stays off\n", path);
        return false;
    }
    char text[2048];
    size_t n = fread(text, 1, sizeof text - 1, f);
    bool too_big = n == sizeof text - 1;
    fclose(f);
    if (too_big) {
        LOG("[ocw] %s is larger than %zu bytes: ignored\n", path, sizeof text - 1);
        return false;
    }
    text[n] = 0;
    Json *j = json_parse(text, n);
    if (!j) {
        LOG("[ocw] %s is not valid JSON: ignored\n", path);
        return false;
    }
    const char *domain = json_str(json_get(j, "domain"), "");
    const char *token = json_str(json_get(j, "token"), "");
    const char *ip = json_str(json_get(j, "ip"), "");
    bool ok = true;
    if (!*domain) {
        LOG("[ocw] %s has no \"domain\"\n", path);
        ok = false;
    } else if (strlen(domain) > OCW_DOMAIN_MAX) {
        LOG("[ocw] \"domain\" is %zu characters; the game's own slot holds %d\n", strlen(domain), OCW_DOMAIN_MAX);
        ok = false;
    }
    if (!*token) {
        LOG("[ocw] %s has no \"token\"\n", path);
        ok = false;
    } else if (strlen(token) != OCW_TOKEN_LEN) {
        LOG("[ocw] \"token\" is %zu characters; it should be %d\n", strlen(token), OCW_TOKEN_LEN);
        ok = false;
    }
    // Kept whatever the checks said: the menu shows and edits these buffers, and a half-filled config must not come
    // back as empty fields that are then written over the file. `ok` alone decides whether the game is touched.
    snprintf(g_ocw.domain, sizeof g_ocw.domain, "%s", domain);
    snprintf(g_ocw.token, sizeof g_ocw.token, "%s", token);
    snprintf(g_ocw.ip, sizeof g_ocw.ip, "%s", ip);
    json_free(j);
    return ok;
}

static bool config_load(void) { return config_load_from(config_path()); }

// The modules are loaded and nothing of the game has run yet (runtime/mods.c mods_boot): write the domain into the
// game's own host string, and tell the network stack which one host it may reach.
void game_modules_loaded(void) {
    g_ocw.active = false;  // (an in-game reboot: the hook makes the game's own call again until the config is read)
    if (!config_load()) return;

    char *slot = (char *)(uintptr_t)MAIN_ADDR(OCW_DOMAIN_OFF);
    char was[OCW_DOMAIN_MAX + 1];
    memcpy(was, slot, OCW_DOMAIN_MAX);
    was[OCW_DOMAIN_MAX] = 0;
    for (int i = 0; i < OCW_DOMAIN_MAX; i++)
        if (was[i] && (was[i] < 0x20 || was[i] > 0x7e)) was[i] = '?';  // (log it, whatever it turned out to be)
    char buf[OCW_DOMAIN_MAX];
    memset(buf, 0, sizeof buf);
    memcpy(buf, g_ocw.domain, strlen(g_ocw.domain));  // zero-padded to the slot, as the console mod writes it
    memcpy(slot, buf, sizeof buf);
    LOG("[ocw] host \"%s\" written at main+%#x (was \"%s\")\n", g_ocw.domain, OCW_DOMAIN_OFF, was);

    net_log_secret(g_ocw.token);  // (never in a log: a log file is what a player attaches to a bug report)
    net_ocw_set_target(g_ocw.domain, g_ocw.ip[0] ? g_ocw.ip : NULL);
    g_ocw.active = true;
    snprintf(g_live_domain, sizeof g_live_domain, "%s", g_ocw.domain);
    snprintf(g_live_token, sizeof g_live_token, "%s", g_ocw.token);
    snprintf(g_live_ip, sizeof g_live_ip, "%s", g_ocw.ip);
    LOG("[ocw] on: the game's own online client against %s\n", g_ocw.domain);
    boards_start_at_boot();
}

bool ocw_active(void) { return g_ocw.active; }

// ---- Play History (My Info and a maker's page): the game's own GetUsers record comes from OCW's NEX side with no play
// statistics, so they are filled in from OCW's REST API, which holds each maker's totals:
//   GET https://<domain>/api/makers/<maker id>  ->  { "stats": { plays, clears, tries, deaths, ... } }  (public: no token
// is sent). The user record (0x1f0 bytes, world.c) holds the four numbers the screen shows in the block at +0xfc (slots below). The maker is read from the record the game just got (maker code +0x08, UTF-16
// without dashes), so the player's own record and any maker's page work alike. A failed request leaves the record
// exactly as the game wrote it.
#define OCW_USER_RING 0x68  // the user ring in the network-session object (online.c USER_RING)
#define OCW_STATS_OFF 0xfc  // the stats block of the record (u32 each): slot 0 is not a play count; the Play History reads slot 1
// courses played, slot 2 attempts, slot 3 courses cleared, slot 4 lives lost (the order of the server's own record: 624, 305, 2763,
// 155, 2595 for 305 played, 2763 attempts, 155 cleared, 2595 lives lost). The totals come as {played, cleared, attempts, lives lost}.
static const int OCW_STATS_SLOT[4] = {1, 3, 2, 4};
#define OCW_REST_TIMEOUT_MS 8000

// The maker ID ("AAA-BBB-CCC") of a user record, from its code; false when the record has none.
static bool record_maker_id(uint64_t e, char out[12]) {
    uint64_t buf = g64(e + 0x08);
    if (!buf || g32(e + 0x10) < 10) return false;
    char code[10];
    for (int i = 0; i < 9; i++) {
        uint16_t ch = *(volatile uint16_t *)(uintptr_t)(buf + 2 * i);
        if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z'))) return false;
        code[i] = (char)ch;
    }
    snprintf(out, 12, "%.3s-%.3s-%.3s", code, code + 3, code + 6);
    return true;
}

// {played, cleared, attempts, lives lost} of the maker, the service's own totals (fetch_profile below: the played list
// was paged, and summing the first page came out short); false when the request or its answer failed.
static bool fetch_profile(const char *maker, OnlineProfile *out);
static bool fetch_play_totals(const char *maker_id, uint32_t totals[4]) {
    OnlineProfile p;
    memset(&p, 0, sizeof p);
    kernel_host_wait_begin();  // (the calling guest thread gives its core up while it waits, as server.c does)
    bool ok = fetch_profile(maker_id, &p);
    kernel_host_wait_end();
    if (ok) totals[0] = p.played, totals[1] = p.cleared, totals[2] = p.attempts, totals[3] = p.lives_lost;
    return ok;
}

// main+0x14d2f00: a maker's page (GetUsers): the game's own wrapper, then the maker's Play History from OCW's REST API
static void get_user_page(Cpu *c) {
    uint64_t self = c->x[0];
    guest_call_orig(c, c->pc);
    ocw_fill_play_stats(c, self, c->x[0]);
}

// ---- the player's Mii (the Mii database, runtime/services/mii.c, asks for it when there is no local mii.charinfo):
//   GET https://<domain>/api/makers/<maker code>  (public: no token is sent)  ->  { ..., "mii": { "mii_data": "<base64>" } }
// Whose maker code: the player's own, which is learned from the game. Course World loads the player's own profile first
// (My Info), so the first user record the game fetches in a session is theirs (ocw_fill_play_stats sees every one). It is
// kept in ocw-maker.txt with a fingerprint of the domain and token (never the token itself): another account's is never
// used, and a later start has it before the game asks for its Mii at boot. Until it is known there is no Mii from OCW
// (the default one is served) and the database changes (IsUpdated) the moment it is. To forget it, delete the file.
// the 88 bytes of the player's CharInfo. Kept for a minute (the Mii is asked for several times in a row, and changes
// when the player picks another in Change face); a failed request keeps the last Mii that came, and is tried again
// after two minutes. Asked from the game's own thread, which gives its core up while the request waits.
#define OCW_MII_TTL_S 60
#define OCW_MII_RETRY_S 120
#define OCW_MII_TIMEOUT_MS 5000
static struct {
    pthread_mutex_t lock;
    char maker[12];  // whose Mii is held (a change in the menu starts over)
    uint8_t mii[0x58];
    bool have, busy;
    time_t next;
} g_mii = {.lock = PTHREAD_MUTEX_INITIALIZER};

#define OCW_SELF_FILE "ocw-maker.txt"
static struct { bool loaded, have; char maker[12]; } g_self;  // (under g_mii.lock)

// Names the account without keeping its token: 64-bit FNV-1a of the domain and the token.
static unsigned long long self_fingerprint(void) {
    unsigned long long h = 0xcbf29ce484222325ull;
    for (const char *p = g_ocw.domain; *p; p++) h = (h ^ (unsigned char)*p) * 0x100000001b3ull;
    h *= 0x100000001b3ull;
    for (const char *p = g_ocw.token; *p; p++) h = (h ^ (unsigned char)*p) * 0x100000001b3ull;
    return h;
}

static void self_load_locked(void) {  // the cache file, once; only an entry of this account counts
    if (g_self.loaded) return;
    g_self.loaded = true;
    g_self.have = false;
    FILE *f = fopen(OCW_SELF_FILE, "rb");
    if (!f) return;
    char line[64], code[16];
    unsigned long long fp;
    if (fgets(line, sizeof line, f) && sscanf(line, "%llx %11s", &fp, code) == 2 && strlen(code) == 11 &&
        fp == self_fingerprint()) {
        snprintf(g_self.maker, sizeof g_self.maker, "%s", code);
        g_self.have = true;
        LOG("[ocw] the player's maker code %s (from " OCW_SELF_FILE ")\n", code);
    }
    fclose(f);
}

// The game fetched a user record: the first one of a session is the player's own.
static void self_learn(const char *maker) {
    pthread_mutex_lock(&g_mii.lock);
    self_load_locked();
    if (!g_self.have) {
        snprintf(g_self.maker, sizeof g_self.maker, "%s", maker);
        g_self.have = true;
        g_mii.next = 0;  // (read the Mii now)
        FILE *f = fopen(OCW_SELF_FILE, "wb");
        if (f) fprintf(f, "%016llx %s\n", self_fingerprint(), maker), fclose(f);
        LOG("[ocw] the player's maker code %s: the first profile the game fetched (kept in " OCW_SELF_FILE ")\n", maker);
    }
    pthread_mutex_unlock(&g_mii.lock);
}

static void self_forget(void) {  // the domain or token changed
    pthread_mutex_lock(&g_mii.lock);
    g_self.loaded = g_self.have = false;
    g_mii.have = false, g_mii.next = 0;
    pthread_mutex_unlock(&g_mii.lock);
}

static int b64_val(int c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
           : c == '+' ? 62 : c == '/' ? 63 : -1;
}

// Decodes base64 into out (cap bytes); the length, or -1 when it is not (all of it) base64 or does not fit.
static int b64_decode(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *s && *s != '='; s++) {
        int v = b64_val((unsigned char)*s);
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v;
        if ((bits += 6) >= 8) {
            bits -= 8;
            if (n == cap) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)n;
}

static bool fetch_player_mii(const char *domain, const char *maker, uint8_t out[0x58]) {
    char url[256];
    snprintf(url, sizeof url, "https://%s/api/makers/%s", domain, maker);
    HttpResponse r;
    kernel_host_wait_begin();
    int st = http_request("GET", url, "Accept: application/json\r\n", NULL, 0, OCW_MII_TIMEOUT_MS, &r);
    kernel_host_wait_end();
    if (st != 200) {
        LOG("[ocw] Mii of %s: %s\n", maker, st ? "HTTP error" : r.error);
        http_free(&r);
        return false;
    }
    Json *j = json_parse(r.body, r.len);
    http_free(&r);
    const char *b64 = j ? json_str(json_get(json_get(j, "mii"), "mii_data"), "") : "";
    uint8_t data[0x58];
    int n = b64_decode(b64, data, sizeof data);
    bool ok = n == 0x58 && mii_charinfo_valid(data);
    json_free(j);
    if (!ok) LOG("[ocw] Mii of %s: the answer has no usable 88-byte Mii\n", maker);
    else memcpy(out, data, sizeof data);
    return ok;
}

static bool ocw_player_mii(uint8_t out[0x58]) {
    if (!g_ocw.active) return false;
    char maker[12], domain[sizeof g_ocw.domain];
    snprintf(domain, sizeof domain, "%s", g_ocw.domain);
    pthread_mutex_lock(&g_mii.lock);
    self_load_locked();
    if (!g_self.have) {  // (not known yet: the default Mii)
        pthread_mutex_unlock(&g_mii.lock);
        return false;
    }
    snprintf(maker, sizeof maker, "%s", g_self.maker);
    if (strcmp(maker, g_mii.maker)) {  // (another maker than the one held)
        snprintf(g_mii.maker, sizeof g_mii.maker, "%s", maker);
        g_mii.have = false, g_mii.next = 0;
    }
    time_t now = time(NULL);
    bool fetch = !g_mii.busy && now >= g_mii.next;
    if (fetch) g_mii.busy = true;
    pthread_mutex_unlock(&g_mii.lock);
    if (fetch) {  // (outside the lock: HTTP)
        uint8_t got[0x58];
        bool ok = fetch_player_mii(domain, maker, got);
        pthread_mutex_lock(&g_mii.lock);
        g_mii.busy = false;
        if (!strcmp(maker, g_mii.maker)) {
            g_mii.next = now + (ok ? OCW_MII_TTL_S : OCW_MII_RETRY_S);
            if (ok) {
                if (!g_mii.have || memcmp(g_mii.mii, got, sizeof got)) LOG("[ocw] the Mii of %s read from OCW\n", maker);
                memcpy(g_mii.mii, got, sizeof got), g_mii.have = true;
            }
        }
        pthread_mutex_unlock(&g_mii.lock);
    }
    pthread_mutex_lock(&g_mii.lock);
    bool have = g_mii.have && !strcmp(maker, g_mii.maker);
    if (have) memcpy(out, g_mii.mii, 0x58);
    pthread_mutex_unlock(&g_mii.lock);
    return have;
}

// ---- the Mii the game uploads. The game keeps the player's profile in its network-session object, and sends it to OCW a few
// seconds after logging in (NEX DataStoreSMM2 RegisterUser, method 47, and SyncUserProfile, 49): the request wrappers copy
// 88 bytes of the object into the request, from self+0x73878 (RegisterUser, main+0x14ce500) and self+0x73910 (SyncUserProfile,
// main+0x14cef40). A fresh save has no profile Mii there (all zeros), and what it sent replaced the player's Mii on OCW with a
// blank one. So just before either wrapper builds its request, a blank slot, or one that holds the default Mii, gets the
// player's Mii from OCW (the one the Mii database serves). A Mii the player picked in the game is never touched.
#define OCW_REGISTER_USER MAIN_ADDR(0x14ce500)
#define OCW_SYNC_PROFILE MAIN_ADDR(0x14cef40)
#define OCW_MII_REGISTER_OFF 0x73878
#define OCW_MII_SYNC_OFF 0x73910
extern void mii_default_charinfo(uint8_t out[0x58], const char *nickname);  // (runtime/services/mii.c)

static void put_player_mii(uint64_t self, uint64_t off, const char *which) {
    if (!g_ocw.active || !self) return;
    uint8_t mine[0x58], def[0x58];
    if (!ocw_player_mii(mine)) {
        LOG("[ocw] profile upload (%s): no Mii from OCW to put in\n", which);
        return;
    }
    mii_default_charinfo(def, getenv("RECOMP_USER_NAME"));
    if (!memcmp(mine, def, sizeof def)) return;  // (OCW has the default one too: nothing better to put in)
    uint8_t *p = (uint8_t *)(uintptr_t)(self + off);
    bool blank = true;
    for (int i = 0; i < 16; i++) blank &= p[i] == 0;
    bool is_default = !memcmp(p, def, 16) && !memcmp(p + 0x26, def + 0x26, 0x32);
    if (blank || is_default) {
        memcpy(p, mine, 0x58);
        LOG("[ocw] profile upload (%s): the game's Mii was %s: the player's Mii from OCW goes up instead\n", which,
            blank ? "blank" : "the default one");
    } else {
        LOG("[ocw] profile upload (%s): the game's own Mii goes up as it is\n", which);
    }
}
static void register_user(Cpu *c) {
    put_player_mii(c->x[0], OCW_MII_REGISTER_OFF, "registration");
    guest_call_orig(c, c->pc);
}
static void sync_user_profile(Cpu *c) {
    put_player_mii(c->x[0], OCW_MII_SYNC_OFF, "profile sync");
    guest_call_orig(c, c->pc);
}

// Overrides are registered before the game's first instruction runs.
__attribute__((constructor)) static void ocw_register(void) {
    mii_set_remote(ocw_player_mii);
    hle_override(MAIN_ADDR(0x14d2f00), get_user_page, "mod: GetUsers (maker page): OCW's Play History");
    hle_override(OCW_REGISTER_USER, register_user, "mod: RegisterUser: the player's Mii from OCW when the game's is blank");
    hle_override(OCW_SYNC_PROFILE, sync_user_profile, "mod: SyncUserProfile: the player's Mii from OCW when the game's is blank");
}

// ---- issue #13: some makers' Miis come from OCW blank (all zeros) or otherwise not a CharInfo the game can use, and show up
// as a broken Mii wherever that maker appears. The user record the game just fetched holds the Mii at +0x9c (88 bytes of
// CharInfo; read off the records of makers with good and bad Miis). With the setting on, a bad one of ANOTHER maker (the
// player's own is handled with the rest of their Mii) is replaced by a generated Mii seeded from the maker code and a number
// chosen per session: the same maker looks the same all session, and different in the next one.
#define OCW_RECORD_SIZE 0x1f0
#define OCW_RECORD_MII 0x9c
extern void mii_default_charinfo(uint8_t out[0x58], const char *nickname);  // (runtime/services/mii.c)

static bool mii_is_bad(const uint8_t m[0x58]) {
    bool zero = true;
    for (int i = 0; i < 0x58 && zero; i++) zero = m[i] == 0;
    return zero || !mii_charinfo_valid(m);
}

static uint32_t mii_rng(uint64_t *st) {  // xorshift64*
    *st ^= *st >> 12, *st ^= *st << 25, *st ^= *st >> 27;
    return (uint32_t)((*st * 0x2545F4914F6CDD1Dull) >> 32);
}
static int mii_pick(uint64_t *st, int lo, int hi) { return lo + (int)(mii_rng(st) % (uint32_t)(hi - lo + 1)); }

// Chosen once per run of the app: a maker with a bad Mii gets a different generated Mii in each session, the same one for the
// whole session (the Mii is a function of the maker code and this).
static uint64_t mii_session_salt(void) {
    static uint64_t salt;
    if (!salt) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        salt = ((uint64_t)t.tv_sec * 1000003ull) ^ ((uint64_t)t.tv_nsec << 20) ^ (uint64_t)(uintptr_t)&t;
        if (!salt) salt = 1;
    }
    return salt;
}

// A valid random Mii, the same for the same maker code within a session. Types and colours are drawn from their whole (valid) ranges; sizes
// and positions stay near the default's so the face keeps its proportions. Always passes mii_charinfo_valid.
static void mii_generate(const char *maker, uint8_t out[0x58]) {
    uint64_t st = 0xcbf29ce484222325ull ^ mii_session_salt();
    for (const char *p = maker; *p; p++) st = (st ^ (unsigned char)*p) * 0x100000001b3ull;
    if (!st) st = 1;
    mii_default_charinfo(out, "Mii");
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)mii_rng(&st);  // (the create ID: random, with the "valid" bits kept)
    for (int i = 9; i < 16; i++) out[i] = (uint8_t)mii_rng(&st);
    out[8] = (uint8_t)(0x80 | (mii_rng(&st) & 0x3f));
    uint8_t *f = out + 0x26;
    f[1] = (uint8_t)mii_pick(&st, 0, 11);                              // favourite colour
    f[2] = (uint8_t)mii_pick(&st, 0, 1);                               // gender
    f[3] = (uint8_t)mii_pick(&st, 30, 100), f[4] = (uint8_t)mii_pick(&st, 30, 100);  // height, build
    f[7] = (uint8_t)mii_pick(&st, 0, 11), f[8] = (uint8_t)mii_pick(&st, 0, 9);  // face shape, skin
    f[9] = mii_pick(&st, 0, 9) < 8 ? 0 : (uint8_t)mii_pick(&st, 1, 11);   // wrinkles, mostly none
    f[10] = mii_pick(&st, 0, 9) < 7 ? 0 : (uint8_t)mii_pick(&st, 1, 11);  // makeup
    f[11] = (uint8_t)mii_pick(&st, 0, 131), f[12] = (uint8_t)mii_pick(&st, 0, 7), f[13] = (uint8_t)mii_pick(&st, 0, 1);  // hair
    f[14] = (uint8_t)mii_pick(&st, 0, 59), f[15] = (uint8_t)mii_pick(&st, 0, 5);  // eyes: type, colour
    f[16] = (uint8_t)mii_pick(&st, 3, 5), f[17] = 3, f[18] = (uint8_t)mii_pick(&st, 3, 5);  // scale, aspect, rotation
    f[19] = (uint8_t)mii_pick(&st, 1, 3), f[20] = (uint8_t)mii_pick(&st, 11, 13);  // x, y
    f[21] = (uint8_t)mii_pick(&st, 0, 23), f[22] = f[12];             // eyebrows: type, colour (the hair's)
    f[23] = (uint8_t)mii_pick(&st, 3, 5), f[24] = 3, f[25] = (uint8_t)mii_pick(&st, 5, 7);
    f[26] = (uint8_t)mii_pick(&st, 1, 3), f[27] = (uint8_t)mii_pick(&st, 9, 11);
    f[28] = (uint8_t)mii_pick(&st, 0, 17), f[29] = (uint8_t)mii_pick(&st, 3, 5), f[30] = (uint8_t)mii_pick(&st, 8, 10);  // nose
    f[31] = (uint8_t)mii_pick(&st, 0, 35), f[32] = (uint8_t)mii_pick(&st, 0, 4);  // mouth: type, colour
    f[33] = (uint8_t)mii_pick(&st, 3, 5), f[34] = 3, f[35] = (uint8_t)mii_pick(&st, 12, 14);
    f[36] = f[12];                                                     // beard colour (the hair's)
    f[37] = mii_pick(&st, 0, 9) < 8 ? 0 : (uint8_t)mii_pick(&st, 1, 5);   // beard, mostly none
    f[38] = mii_pick(&st, 0, 9) < 8 ? 0 : (uint8_t)mii_pick(&st, 1, 5);   // moustache
    f[39] = 4, f[40] = 10;
    f[41] = mii_pick(&st, 0, 9) < 8 ? 0 : (uint8_t)mii_pick(&st, 1, 8);   // glasses, mostly none
    f[42] = (uint8_t)mii_pick(&st, 0, 5), f[43] = 4, f[44] = 10;
    f[45] = mii_pick(&st, 0, 9) < 9 ? 0 : 1, f[46] = 4, f[47] = 2, f[48] = 20;  // a mole, rarely
    f[49] = 0;
    if (!mii_charinfo_valid(out) || (out[8] & 0xc0) != 0x80) mii_default_charinfo(out, "Mii");  // (cannot happen: kept as a guard)
}

// The server's answer is fixed before the game parses it (runtime/services/ssl.c): this decides for one record. The player's
// own is left alone; the Mii is generated from the maker code (or the name when the answer gave none), so it is the same for
// the whole session.
bool game_own_maker(char out[10]) {
    if (!g_ocw.active) return false;
    pthread_mutex_lock(&g_mii.lock);
    self_load_locked();
    bool ok = g_self.have && strlen(g_self.maker) == 11;  // ("NRX-G48-CC8")
    if (ok) snprintf(out, 10, "%.3s%.3s%.3s", g_self.maker, g_self.maker + 4, g_self.maker + 8);
    pthread_mutex_unlock(&g_mii.lock);
    return ok;
}

// The service's REST address and the player's own maker code ("AAA-BBB-CCC", "" when not learned yet), for the Search tab (search.c).
// False with the service off.
bool ocw_rest_target(char domain[], size_t domain_cap, char maker[12]) {
    if (!g_ocw.active || !g_ocw.domain[0]) return false;
    snprintf(domain, domain_cap, "%s", g_ocw.domain);
    maker[0] = 0;
    pthread_mutex_lock(&g_mii.lock);
    self_load_locked();
    if (g_self.have && strlen(g_self.maker) == 11) snprintf(maker, 12, "%s", g_self.maker);
    pthread_mutex_unlock(&g_mii.lock);
    return true;
}

bool game_replace_bad_mii(const char *maker, const char *name, uint8_t out[0x58]) {
    if (!g_settings.replace_bad_mii || !g_ocw.active) return false;
    char id[12];
    if (strlen(maker) == 9) snprintf(id, sizeof id, "%.3s-%.3s-%.3s", maker, maker + 3, maker + 6);
    else snprintf(id, sizeof id, "%.11s", name);
    pthread_mutex_lock(&g_mii.lock);
    self_load_locked();
    bool own = g_self.have && !strcmp(g_self.maker, id);
    pthread_mutex_unlock(&g_mii.lock);
    if (own) return false;
    mii_generate(id, out);
    LOG("[ocw] the Mii OCW sent for %s (%s) is blank: a generated one goes to the game instead\n", id, name);
    return true;
}

static void mii_fix_records(Cpu *c, uint64_t no) {
    if (!g_settings.replace_bad_mii) return;
    char self[12] = "";
    pthread_mutex_lock(&g_mii.lock);
    self_load_locked();
    if (g_self.have) snprintf(self, sizeof self, "%s", g_self.maker);
    pthread_mutex_unlock(&g_mii.lock);
    netobj_lock(c);
    uint64_t e = g64(no + OCW_USER_RING);
    int n = (int32_t)g32(no + OCW_USER_RING + 0x10);
    if (e && n > 0) {
        if (n > 8) n = 8;
        for (int i = 0; i < n; i++) {
            uint64_t rec = e + (uint64_t)i * OCW_RECORD_SIZE;
            char id[12];
            if (!record_maker_id(rec, id) || !strcmp(id, self)) continue;  // (not the player's own)
            uint8_t cur[0x58];
            memcpy(cur, (const void *)(uintptr_t)(rec + OCW_RECORD_MII), sizeof cur);
            if (!mii_is_bad(cur)) continue;
            uint8_t fresh[0x58];
            mii_generate(id, fresh);
            memcpy((void *)(uintptr_t)(rec + OCW_RECORD_MII), fresh, sizeof fresh);
            LOG("[ocw] the Mii OCW has for %s is not usable: a generated one is shown instead\n", id);
        }
    }
    netobj_unlock(c);
}

// After a GetUsers wrapper of the game ran (no = the network-session object, ret = what the wrapper returned): the
// first record of its user ring gets the maker's play history. Only with OCW on and the game's own request done.
void ocw_fill_play_stats(Cpu *c, uint64_t no, uint64_t ret) {
    if (!g_ocw.active) return;
    if (g32(no + 0x60) != 0 || (uint8_t)ret != wrapper_ok() || (int32_t)g32(no + OCW_USER_RING + 0x10) < 1) return;
    uint64_t keep = c->x[0];
    char id[12], again[12];
    netobj_lock(c);
    uint64_t e = g64(no + OCW_USER_RING);
    bool known = e && record_maker_id(e, id);
    netobj_unlock(c);
    if (known) self_learn(id);  // (the first record of a session is the player's own)
    mii_fix_records(c, no);
    uint32_t totals[4];
    if (known && fetch_play_totals(id, totals)) {  // (outside the lock: HTTP)
        netobj_lock(c);
        e = g64(no + OCW_USER_RING);
        if (e && record_maker_id(e, again) && !strcmp(id, again))
            for (int i = 0; i < 4; i++) s32(e + OCW_STATS_OFF + 4 * OCW_STATS_SLOT[i], totals[i]);
        netobj_unlock(c);
        LOG("[ocw] play history of %s: %u played, %u cleared, %u attempts, %u lives lost\n", id, totals[0], totals[1],
            totals[2], totals[3]);
    } else if (!known) {
        LOG("[ocw] play history: the record has no maker code\n");
    }
    c->x[0] = keep;
}

// ---- ocw-config.json written back (the overlay menu's Mods tab, below). The file stays the one place the settings
// live, so editing it by hand and setting them in the menu are the same thing.
static void config_save(void) {
    const char *path = config_path();
    FILE *f = fopen(path, "w");
    if (!f) {
        LOG("[ocw] cannot write %s\n", path);
        return;
    }
    fprintf(f, "{\n  \"domain\": \"%s\",\n  \"token\": \"%s\"", g_ocw.domain, g_ocw.token);
    if (g_ocw.ip[0]) fprintf(f, ",\n  \"ip\": \"%s\"", g_ocw.ip);
    fprintf(f, "\n}\n");
    fclose(f);
    LOG("[ocw] %s written (domain \"%s\"%s%s, token %s)\n", path, g_ocw.domain, g_ocw.ip[0] ? " at " : "", g_ocw.ip,
        g_ocw.token[0] ? "set" : "not set");
}

// ---- the Mods tab's own section (runtime/game_rows.h, drawn by runtime/ui/menu.cpp). It is always shown, so the
// domain and token can be entered before anything works. Changing any of them writes the file back, so the file stays
// the one place they live.
static bool rows_read;  // the config has been read into the buffers for the menu

int ocw_rows(GameModRow *out, int room) {
    if (!rows_read) rows_read = true, config_load();  // (ignores its own complaints: empty fields are fine here)
    // (named fields, not positions: the row struct is shared and has grown once already)
    static const GameModRow rows[] = {
        {.label = "OpenCourse World"},  // (a section title: no value, no text)
        {.label = "Domain",
         .desc = "OCW's server, as opencourse.world gives it (at most 29 characters: the size of the slot in the "
                 "game's own code). May be an address instead. Takes effect at the next start.",
         .text = g_ocw.domain, .cap = sizeof g_ocw.domain, .hint = "not set", .max_len = OCW_DOMAIN_MAX},
        {.label = "Token",
         .desc = "Your personal OCW token, 22 characters, from the config opencourse.world gives you. Not shown "
                 "until you activate this row, hidden again when the menu closes, and never written to a log. Takes "
                 "effect at the next start.",
         .text = g_ocw.token, .cap = sizeof g_ocw.token, .hint = "not set", .secret = true,
         .max_len = OCW_TOKEN_LEN},
    };
    int n = (int)(sizeof rows / sizeof *rows);
    if (room < n) return 0;
    for (int i = 0; i < n; i++) out[i] = rows[i];
    return n;
}

void ocw_rows_changed(void) {
    // (trim what a text field may have collected: the menu caps the length, these catch stray spaces)
    for (char *p = g_ocw.domain + strlen(g_ocw.domain); p > g_ocw.domain && p[-1] == ' ';) *--p = 0;
    for (char *p = g_ocw.ip + strlen(g_ocw.ip); p > g_ocw.ip && p[-1] == ' ';) *--p = 0;
    self_forget();  // (another account may be meant now: its maker is learned again, or read back from its own cache)
    config_save();
    if (g_ocw.token[0] && strlen(g_ocw.token) != OCW_TOKEN_LEN)
        LOG("[ocw] the token is %zu characters; OCW's are %d - check it\n", strlen(g_ocw.token), OCW_TOKEN_LEN);
    if (!(g_ocw.domain[0] && g_ocw.token[0])) LOG("[ocw] the domain and the token are not both set yet\n");
}

// The Online tab (runtime/game_rows.h): the status shown by its globe, and a config file picked there.
int game_online_state(void) {
    if (!rows_read) rows_read = true, config_load();
    if (!g_ocw.domain[0] || strlen(g_ocw.token) != OCW_TOKEN_LEN) return 0;
    bool same = g_ocw.active && !strcmp(g_ocw.domain, g_live_domain) && !strcmp(g_ocw.token, g_live_token) &&
                !strcmp(g_ocw.ip, g_live_ip);
    if (!same) return 2;
    return net_ocw_reachable() == 0 ? 3 : 1;  // (not known yet counts as online: it is asked again within a frame or two)
}

// ---- the player's profile for the Online tab: the maker code learned above, and the played totals and name from OCW's
// public REST API, fetched on a thread of their own (the menu draws every frame and must not wait for HTTP)
static struct {
    pthread_mutex_t lock;
    OnlineProfile p;
    time_t next;
    time_t styles_next;  // the clears per style are read again not before
} g_prof = {.lock = PTHREAD_MUTEX_INITIALIZER};

// GET /api/makers/<code>: { "username": ..., "stats": { plays, clears, tries, deaths, world_records, first_clears } }. These are
// the service's own totals (the played list stops short of them: it is paged and holds only what is listed).
static bool fetch_profile(const char *maker, OnlineProfile *out) {
    char url[256];
    snprintf(url, sizeof url, "https://%s/api/makers/%s", g_ocw.domain, maker);
    HttpResponse r;
    int st = http_request("GET", url, "Accept: application/json\r\n", NULL, 0, OCW_REST_TIMEOUT_MS, &r);
    if (st != 200) {
        LOG("[ocw] profile of %s: %s\n", maker, st ? "HTTP error" : r.error);
        http_free(&r);
        return false;
    }
    Json *j = json_parse(r.body, r.len);
    http_free(&r);
    const Json *stats = j ? json_get(j, "stats") : NULL;
    bool ok = stats && stats->type == JSON_OBJ;
    if (ok) {
        out->played = (unsigned)json_num(json_get(stats, "plays"), 0);
        out->cleared = (unsigned)json_num(json_get(stats, "clears"), 0);
        out->attempts = (unsigned)json_num(json_get(stats, "tries"), 0);
        out->lives_lost = (unsigned)json_num(json_get(stats, "deaths"), 0);
        out->world_records = (unsigned)json_num(json_get(stats, "world_records"), 0);
        out->first_clears = (unsigned)json_num(json_get(stats, "first_clears"), 0);
    }
    if (j) snprintf(out->name, sizeof out->name, "%s", json_str(json_get(j, "username"), ""));
    if (g_settings.clean_usernames || g_settings.strip_discord_tags) {  // (the menu's font cannot draw the console's symbols either: cleaned as the game's names are)
        extern int username_clean(uint8_t *s, size_t len);  // runtime/services/ssl.c
        extern int username_strip_tag(uint8_t *s, size_t len);
        size_t n = strlen(out->name);
        if (g_settings.clean_usernames) username_clean((uint8_t *)out->name, n);
        if (g_settings.strip_discord_tags) username_strip_tag((uint8_t *)out->name, n);
        while (n && out->name[n - 1] == ' ') out->name[--n] = 0;
    }
    json_free(j);
    return ok;
}

// GET /api/makers/<code>/played?offset=N: the courses the maker has played, ten to a page, each with "cleared" and the course's
// "style" (SMB1, SMB3, SMW, NSMBU, SM3DW). The cleared ones counted per style, in ui/history.h order. False when a page fails.
#define OCW_PLAYED_PAGES 150  // (1500 courses: a longer list is counted as far as that)
static bool fetch_style_clears(const char *maker, unsigned out[5]) {
    memset(out, 0, 5 * sizeof *out);
    for (int page = 0; page < OCW_PLAYED_PAGES; page++) {
        char url[256];
        snprintf(url, sizeof url, "https://%s/api/makers/%s/played?offset=%d", g_ocw.domain, maker, page * 10);
        HttpResponse r;
        int st = http_request("GET", url, "Accept: application/json\r\n", NULL, 0, OCW_REST_TIMEOUT_MS, &r);
        if (st != 200) {
            LOG("[ocw] played list of %s: %s\n", maker, st ? "HTTP error" : r.error);
            http_free(&r);
            return false;
        }
        Json *j = json_parse(r.body, r.len);
        http_free(&r);
        size_t n = j && j->type == JSON_ARR ? json_len(j) : 0;
        for (size_t i = 0; i < n; i++) {
            const Json *e = json_at(j, i);
            if (!json_bool(json_get(e, "cleared"), false)) continue;
            const char *style = json_str(json_get(json_get(e, "course"), "style"), "");
            int k = !strcmp(style, "SMB1") ? 0 : !strcmp(style, "SMB3") ? 1 : !strcmp(style, "SMW") ? 2
                  : !strcmp(style, "NSMBU") ? 3 : !strcmp(style, "SM3DW") ? 4 : -1;
            if (k >= 0) out[k]++;
        }
        json_free(j);
        if (n < 10) return j != NULL || page > 0 || n == 0;  // (the last page)
    }
    return true;
}

static void *profile_thread(void *arg) {
    (void)arg;
    char maker[12];
    OnlineProfile got;
    memset(&got, 0, sizeof got);
    pthread_mutex_lock(&g_prof.lock);
    snprintf(maker, sizeof maker, "%s", g_prof.p.maker);
    pthread_mutex_unlock(&g_prof.lock);
    bool ok = fetch_profile(maker, &got);
    pthread_mutex_lock(&g_prof.lock);
    g_prof.p.busy = false;
    g_prof.next = time(NULL) + (ok ? 60 : 30);
    if (!strcmp(maker, g_prof.p.maker)) {  // (not another account's by now)
        if (ok) {
            g_prof.p.have_stats = true;
            g_prof.p.played = got.played, g_prof.p.cleared = got.cleared, g_prof.p.attempts = got.attempts;
            g_prof.p.lives_lost = got.lives_lost, g_prof.p.world_records = got.world_records;
            g_prof.p.first_clears = got.first_clears;
        }
        if (got.name[0]) snprintf(g_prof.p.name, sizeof g_prof.p.name, "%s", got.name);
    }
    bool styles_due = ok && time(NULL) >= g_prof.styles_next;
    pthread_mutex_unlock(&g_prof.lock);
    if (styles_due) {  // (a longer walk, and the numbers move slowly: not on every refresh)
        unsigned sc[5];
        bool got_styles = fetch_style_clears(maker, sc);
        pthread_mutex_lock(&g_prof.lock);
        g_prof.styles_next = time(NULL) + (got_styles ? 600 : 60);
        if (got_styles && !strcmp(maker, g_prof.p.maker)) {
            g_prof.p.have_styles = true;
            memcpy(g_prof.p.style_clears, sc, sizeof sc);
        }
        pthread_mutex_unlock(&g_prof.lock);
    }
    return NULL;
}

void game_online_profile(OnlineProfile *out) {
    char maker[12] = "";
    pthread_mutex_lock(&g_mii.lock);
    self_load_locked();
    bool known = g_self.have && g_ocw.active;
    if (known) snprintf(maker, sizeof maker, "%s", g_self.maker);
    pthread_mutex_unlock(&g_mii.lock);
    pthread_mutex_lock(&g_prof.lock);
    if (!known) {
        memset(&g_prof.p, 0, sizeof g_prof.p);
    } else {
        if (strcmp(g_prof.p.maker, maker)) {  // (another account: start over)
            bool busy = g_prof.p.busy;
            memset(&g_prof.p, 0, sizeof g_prof.p);
            g_prof.p.busy = busy, g_prof.next = 0;
            snprintf(g_prof.p.maker, sizeof g_prof.p.maker, "%s", maker);
        }
        g_prof.p.known = true;
        if (!g_prof.p.busy && time(NULL) >= g_prof.next) {
            pthread_t th;
            g_prof.p.busy = true;
            if (pthread_create(&th, NULL, profile_thread, NULL) == 0) pthread_detach(th);
            else g_prof.p.busy = false, g_prof.next = time(NULL) + 30;
        }
    }
    *out = g_prof.p;
    pthread_mutex_unlock(&g_prof.lock);
}

// The leaderboards (games/smm2/ocw_leaderboards.py): run in the background, half a minute or so as it walks the boards, and the
// answer kept in ocw-boards.txt with the account's fingerprint, so the last one shows at once on the next start
#define OCW_BOARDS_FILE "ocw-boards.txt"
#define SEP "\\"
#define OCW_BOARDS_MAX_S 600  // a run that has not finished in this long is stopped
static const char *const BOARD_IDS[ONLINE_BOARDS] = {"weekly", "alltime", "clears", "first", "record",
                                                     "endless0", "endless1", "endless2", "endless3"};
static struct {
    pthread_mutex_t lock;
    OnlineBoards b;
    bool loaded;     // the file was read once
} g_boards = {.lock = PTHREAD_MUTEX_INITIALIZER};

// Reads the file (this account's only): the age of the answer in seconds, or -1 when there is none
static long boards_read(OnlineBoards *out) {
    FILE *f = fopen(OCW_BOARDS_FILE, "rb");
    if (!f) return -1;
    unsigned long long fp = 0;
    long long when = 0;
    OnlineBoards b;
    memset(&b, 0, sizeof b);
    char line[96];
    bool ok = fgets(line, sizeof line, f) && sscanf(line, "%llx %lld", &fp, &when) == 2 && fp == self_fingerprint();
    while (ok && fgets(line, sizeof line, f)) {
        char id[16], rank[16];
        unsigned value = 0;
        if (sscanf(line, "%15s %15s %u", id, rank, &value) < 2) continue;
        for (int i = 0; i < ONLINE_BOARDS; i++) {
            if (strcmp(id, BOARD_IDS[i])) continue;
            b.rank[i] = rank[0] == '?' ? -1 : rank[0] == '~' ? -2 : rank[0] == '-' ? 0 : atoi(rank);
            b.value[i] = value;
        }
    }
    fclose(f);
    if (!ok) return -1;
    b.have = true;
    *out = b;
    long age = (long)(time(NULL) - (time_t)when);
    return age < 0 ? 0 : age;
}

static void *boards_thread(void *arg) {
    (void)arg;
    char py[1100], script[1100];
    bool ok = false;
    if (gamebuild_python(py, sizeof py) &&
        rt_resource("tools" SEP "games" SEP "smm2" SEP "ocw_leaderboards.py", "games/smm2/ocw_leaderboards.py", script, sizeof script)) {
        // what the player has of the counts that First Clear and World Record are ordered by, when the service has said:
        // a player with none is not on those boards, and the walk of them is skipped
        char fc[16] = "", wr[16] = "";
        pthread_mutex_lock(&g_prof.lock);
        if (g_prof.p.have_stats) {
            snprintf(fc, sizeof fc, "%u", g_prof.p.first_clears);
            snprintf(wr, sizeof wr, "%u", g_prof.p.world_records);
        }
        pthread_mutex_unlock(&g_prof.lock);
        const char *av[12] = {py, "-I", script, "--config", config_path(), "--out", OCW_BOARDS_FILE};
        int ac = 7;
        if (fc[0]) av[ac++] = "--first-clears", av[ac++] = fc, av[ac++] = "--world-records", av[ac++] = wr;
        av[ac] = NULL;
        PlatProc p = plat_spawn(av, NULL, false);
        if (p) {
            time_t t0 = time(NULL);
            int rc;
            while ((rc = plat_proc_wait(p, false)) < 0) {
                if (time(NULL) - t0 > OCW_BOARDS_MAX_S) {
                    plat_proc_kill(p);
                    rc = -1;
                    break;
                }
                struct timespec ts = {0, 500 * 1000 * 1000};
                nanosleep(&ts, NULL);
                OnlineBoards part;  // (written after each round of pages: the top of every board shows long before the end)
                if (boards_read(&part) >= 0 && time(NULL) - t0 < OCW_BOARDS_MAX_S) {
                    pthread_mutex_lock(&g_boards.lock);
                    part.busy = true;
                    g_boards.b = part;
                    pthread_mutex_unlock(&g_boards.lock);
                }
            }
            ok = rc == 0;
        }
    }
    OnlineBoards got;
    bool have = ok && boards_read(&got) >= 0;
    pthread_mutex_lock(&g_boards.lock);
    g_boards.b.busy = false;
    if (have) g_boards.b = got, g_boards.b.busy = false;
    pthread_mutex_unlock(&g_boards.lock);
    LOG("[ocw] leaderboards: %s\n", have ? "read" : "no answer");
    return NULL;
}

// Started when the game goes online (ocw.c's setup): once the service answers and the profile numbers are in, the boards are
// asked for, so the Achievements tab is filled by the time it is opened. After that only the tab's refresh button asks again.
static void *boards_boot_thread(void *arg) {
    for (int i = 0; i < 180 && !net_online(); i++) sleep(1);  // (the service may take a while to answer, or be down)
    for (int i = 0; i < 20; i++) {  // (the counts that two boards go by come with the profile)
        OnlineProfile pr;
        game_online_profile(&pr);
        if (pr.have_stats) break;
        sleep(1);
    }
    return boards_thread(arg);
}

static void boards_start(void *(*run)(void *)) {  // (under g_boards.lock)
    pthread_t th;
    g_boards.b.busy = true;
    if (pthread_create(&th, NULL, run, NULL) == 0) pthread_detach(th);
    else g_boards.b.busy = false;
}

static void boards_start_at_boot(void) {
    search_boot();
    pthread_mutex_lock(&g_boards.lock);
    if (!g_boards.b.busy) boards_start(boards_boot_thread);
    pthread_mutex_unlock(&g_boards.lock);
}

void game_online_boards_refresh(void) {
    pthread_mutex_lock(&g_boards.lock);
    if (g_ocw.domain[0] && !g_boards.b.busy && net_online()) boards_start(boards_thread);
    pthread_mutex_unlock(&g_boards.lock);
}

void game_online_boards(OnlineBoards *out) {
    pthread_mutex_lock(&g_boards.lock);
    if (!g_boards.loaded) {  // (what the last run found, until a new answer comes)
        g_boards.loaded = true;
        if (!rows_read) rows_read = true, config_load();
        OnlineBoards old;
        if (g_ocw.domain[0] && boards_read(&old) >= 0) {
            bool busy = g_boards.b.busy;
            g_boards.b = old;
            g_boards.b.busy = busy;
        }
    }
    *out = g_boards.b;
    pthread_mutex_unlock(&g_boards.lock);
}

bool game_online_import(const char *path) {
    if (!rows_read) rows_read = true, config_load();
    char keep[sizeof g_ocw];  // (a file that isn't a usable config changes nothing)
    memcpy(keep, &g_ocw, sizeof g_ocw);
    if (!config_load_from(path)) {
        memcpy(&g_ocw, keep, sizeof g_ocw);
        return false;
    }
    self_forget();
    config_save();
    return true;
}

// The WebSocket handshake (mods/patches.py: the one patch, in place of the game's own snprintf call). Same arguments
// as the call it replaces, in the same registers:
//   x0 buffer, x1 buffer length, x2 format (not used: the header is ours), x3 path, x4 host, x5 key, x6 protocol
// The protocol argument is used as the name of the header the token rides in, as the console mod does. Returns
// false when OCW is off, and then the generated code makes the game's own call instead.
bool smm2_ocw_header(Cpu *c) {
    if (!g_ocw.active) return false;
    char *buffer = (char *)(uintptr_t)c->x[0];
    size_t len = (size_t)c->x[1];
    const char *path = (const char *)(uintptr_t)c->x[3];
    const char *host = (const char *)(uintptr_t)c->x[4];
    const char *key = (const char *)(uintptr_t)c->x[5];
    const char *protocol = (const char *)(uintptr_t)c->x[6];
    if (!buffer || !len) return false;
    int wrote = snprintf(buffer, len,
                         "GET %s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "Upgrade: websocket\r\n"
                         "Connection: upgrade\r\n"
                         "Sec-WebSocket-Key: %s\r\n"
                         "Sec-WebSocket-Version: 13\r\n"
                         "%s: %s\r\n"
                         "Mod-Version: %s\r\n"
                         "Game-Version: %s\r\n"
                         "\r\n",
                         path ? path : "/", host ? host : g_ocw.domain, key ? key : "", protocol ? protocol : "Token",
                         g_ocw.token, OCW_MOD_VERSION, OCW_GAME_VERSION);
    c->x[0] = (uint64_t)(uint32_t)wrote;  // (snprintf's own return: the length it would have written)
    MLOG("ocw: handshake for %s (%d bytes, token carried in \"%s\")\n", path ? path : "/", wrote,
         protocol ? protocol : "Token");
    return true;
}
