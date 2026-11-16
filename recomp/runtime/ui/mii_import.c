// Mii Editor > Import from a link (mii_import.h).
//
// A miicharacters.com Mii page has the Mii's own QR picture (the one the 3DS / Wii U Mii Maker reads). The picture is
// fetched and read (quirc), the 112 bytes in it are decrypted (AES-CCM with the key every 3DS Mii QR code uses, the same
// for all), and the 3DS Mii data inside (96 bytes, "StoreData", CRC-checked) is converted to the Switch's CharInfo.
// The conversion follows the owner's mii-editor (qr-import.mjs, with @pretendonetwork/mii-js for the StoreData's layout):
// the 3DS's colour numbers sit at fixed places in the console's longer lists (hair and eyebrow and beard 0 -> 8,
// eye + 8, mouth + 19, glasses 0 -> 8 else + 13), and every other field carries over as it is.
#include "mii_import.h"

#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../net/http.h"
#include "../rt.h"
#include "../services/mii_charinfo_file.h"
#include "../../third_party/quirc/quirc.h"
#include "../../third_party/stb/stb_image.h"

void mii_default_charinfo(uint8_t out[0x58], const char *nickname);  // services/mii.c

#define SITE "https://www.miicharacters.com"
#define MAX_PIXELS 4000000

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static MiiImportState g_state;
static uint8_t g_out[0x58];
static char g_name[48], g_why[160], g_url[96];
static uint8_t *g_img;  // the Mii's picture (a JPEG) as the site has it, handed over by mii_import_poll
static size_t g_img_len;

// ---------------------------------------------------------------- AES-128 (encrypting a block is all CCM's decryption needs)
static uint8_t g_sbox[256];
static void sbox_init(void) {
    if (g_sbox[0]) return;
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));  // p * 3
        q ^= (uint8_t)(q << 1), q ^= (uint8_t)(q << 2), q ^= (uint8_t)(q << 4);  // q / 3
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)));
        g_sbox[p] = (uint8_t)(x ^ 0x63);
    } while (p != 1);
    g_sbox[0] = 0x63;
}
static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
static void aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    sbox_init();
    uint8_t rk[176];
    memcpy(rk, key, 16);
    uint8_t rc = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
        if (i % 16 == 0) {
            uint8_t t0 = t[0];
            t[0] = (uint8_t)(g_sbox[t[1]] ^ rc), t[1] = g_sbox[t[2]], t[2] = g_sbox[t[3]], t[3] = g_sbox[t0];
            rc = xt(rc);
        }
        for (int k = 0; k < 4; k++) rk[i + k] = (uint8_t)(rk[i + k - 16] ^ t[k]);
    }
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[i]);
    for (int r = 1; r <= 10; r++) {
        uint8_t t[16];
        for (int c = 0; c < 4; c++)  // SubBytes + ShiftRows
            for (int row = 0; row < 4; row++) t[c * 4 + row] = g_sbox[s[((c + row) & 3) * 4 + row]];
        if (r < 10) {
            for (int c = 0; c < 4; c++) {  // MixColumns
                uint8_t *a = t + c * 4, a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                a[0] = (uint8_t)(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
                a[1] = (uint8_t)(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
                a[2] = (uint8_t)(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
                a[3] = (uint8_t)(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
            }
        }
        for (int i = 0; i < 16; i++) s[i] = (uint8_t)(t[i] ^ rk[r * 16 + i]);
    }
    memcpy(out, s, 16);
}

// ---------------------------------------------------------------- QR payload -> StoreData
// The 112 bytes: an 8 byte nonce, 88 bytes of encrypted data (the 96 byte StoreData without the nonce's place and its last
// 8 bytes, which are zero), a 16 byte tag. The tag is not checked here: the StoreData's own CRC16 is.
static bool unwrap(const uint8_t *qr, int len, uint8_t sd[96]) {
    static const uint8_t key[16] = {0x59, 0xfc, 0x81, 0x7e, 0x64, 0x46, 0xea, 0x61, 0x90, 0x34, 0x7b, 0x20, 0xe9, 0xbd, 0xce, 0x52};
    if (len != 112) return false;
    uint8_t plain[96];
    for (int b = 0; b < 6; b++) {  // CCM's counter mode: block b + 1
        uint8_t ctr[16] = {2}, ks[16];  // (flags: 3 byte counter), the nonce (8 bytes + 4 zero), the counter
        memcpy(ctr + 1, qr, 8);
        ctr[15] = (uint8_t)(b + 1);
        aes128_encrypt(key, ctr, ks);
        for (int i = 0; i < 16; i++) {
            int at = b * 16 + i;
            plain[at] = at < 88 ? (uint8_t)(qr[8 + at] ^ ks[i]) : 0;
        }
    }
    memcpy(sd, plain, 12);
    memcpy(sd + 12, qr, 8);
    memcpy(sd + 20, plain + 12, 76);
    return true;
}

static unsigned crc16(const uint8_t *d, int n) {  // CRC-16/XMODEM
    unsigned crc = 0;
    for (int i = 0; i < n; i++) {
        crc ^= (unsigned)d[i] << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc & 0xFFFF;
}

// n bits from bit position `at` of the StoreData, the first bit being the lowest (the layout of mii-js's BitStream).
static unsigned bits(const uint8_t *d, int at, int n) {
    unsigned v = 0;
    for (int i = 0; i < n; i++) v |= (unsigned)((d[(at + i) >> 3] >> ((at + i) & 7)) & 1) << i;
    return v;
}

static void utf8_name(const uint8_t *d, char *out, size_t cap) {  // up to 10 UTF-16 characters
    size_t o = 0;
    for (int i = 0; i < 10; i++) {
        unsigned cp = d[2 * i] | (unsigned)d[2 * i + 1] << 8;
        if (!cp) break;
        if (cp >= 0xD800 && cp < 0xE000) cp = '?';
        int n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : 3;
        if (o + (size_t)n + 1 > cap) break;
        if (n == 1) out[o++] = (char)cp;
        else if (n == 2) out[o++] = (char)(0xC0 | cp >> 6), out[o++] = (char)(0x80 | (cp & 0x3F));
        else out[o++] = (char)(0xE0 | cp >> 12), out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)), out[o++] = (char)(0x80 | (cp & 0x3F));
    }
    out[o] = 0;
}

// StoreData -> CharInfo (the fields' bit places are mii-js's decode()).
static bool convert(const uint8_t sd[96], uint8_t out[0x58], char name[48], char *why, size_t cap) {
    if ((unsigned)(sd[94] << 8 | sd[95]) != crc16(sd, 94)) {
        snprintf(why, cap, "The Mii's QR code didn't check out (damaged data)");
        return false;
    }
    if (sd[0] != 0 && sd[0] != 3) {
        snprintf(why, cap, "That QR code isn't a Mii from a 3DS or Wii U");
        return false;
    }
    mii_default_charinfo(out, "Mii");
    utf8_name(sd + 26, name, 48);
    unsigned hair_color, brow_color, beard_color, glass_color;
#define F(at, n) bits(sd, at, n)
    out[0x27] = (uint8_t)F(202, 4);                    // favourite colour
    out[0x28] = (uint8_t)F(192, 1);                    // gender
    out[0x29] = (uint8_t)(sd[46] > 127 ? 127 : sd[46]);  // height
    out[0x2A] = (uint8_t)(sd[47] > 127 ? 127 : sd[47]);  // build
    out[0x2D] = (uint8_t)F(385, 4);                    // face shape
    out[0x2E] = (uint8_t)F(389, 3);                    // skin colour
    out[0x2F] = (uint8_t)F(392, 4);                    // wrinkles
    out[0x30] = (uint8_t)F(396, 4);                    // make-up
    out[0x31] = (uint8_t)F(400, 8);                    // hair type
    hair_color = F(408, 3);
    out[0x32] = (uint8_t)(hair_color ? hair_color : 8);
    out[0x33] = (uint8_t)F(411, 1);                    // hair flipped
    out[0x34] = (uint8_t)F(416, 6);                    // eyes
    out[0x35] = (uint8_t)(F(422, 3) + 8);
    out[0x36] = (uint8_t)F(425, 4);
    out[0x37] = (uint8_t)F(429, 3);
    out[0x38] = (uint8_t)F(432, 5);
    out[0x39] = (uint8_t)F(437, 4);
    out[0x3A] = (uint8_t)F(441, 5);
    out[0x3B] = (uint8_t)F(448, 5);                    // eyebrows
    brow_color = F(453, 3);
    out[0x3C] = (uint8_t)(brow_color ? brow_color : 8);
    out[0x3D] = (uint8_t)F(456, 4);
    out[0x3E] = (uint8_t)F(460, 3);
    out[0x3F] = (uint8_t)F(464, 4);
    out[0x40] = (uint8_t)F(469, 4);
    out[0x41] = (uint8_t)F(473, 5);
    out[0x42] = (uint8_t)F(480, 5);                    // nose
    out[0x43] = (uint8_t)F(485, 4);
    out[0x44] = (uint8_t)F(489, 5);
    out[0x45] = (uint8_t)F(496, 6);                    // mouth
    out[0x46] = (uint8_t)(F(502, 3) + 19);
    out[0x47] = (uint8_t)F(505, 4);
    out[0x48] = (uint8_t)F(509, 3);
    out[0x49] = (uint8_t)F(512, 5);
    out[0x4C] = (uint8_t)F(517, 3);                    // moustache
    out[0x4B] = (uint8_t)F(528, 3);                    // beard
    beard_color = F(531, 3);
    out[0x4A] = (uint8_t)(beard_color ? beard_color : 8);
    out[0x4D] = (uint8_t)F(534, 4);
    out[0x4E] = (uint8_t)F(538, 5);
    out[0x4F] = (uint8_t)F(544, 4);                    // glasses
    glass_color = F(548, 3);
    out[0x50] = (uint8_t)(glass_color ? glass_color + 13 : 8);
    out[0x51] = (uint8_t)F(551, 4);
    out[0x52] = (uint8_t)F(555, 5);
    out[0x53] = (uint8_t)F(560, 1);                    // mole
    out[0x54] = (uint8_t)F(561, 4);
    out[0x55] = (uint8_t)F(565, 5);
    out[0x56] = (uint8_t)F(570, 5);
#undef F
    if (!mii_charinfo_valid(out)) {
        snprintf(why, cap, "The Mii in that QR code didn't come out as a valid Mii");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- the picture
static bool read_qr(const uint8_t *img, size_t len, uint8_t sd[96], char *why, size_t cap) {
    int w, h, c;
    uint8_t *px = stbi_load_from_memory(img, (int)len, &w, &h, &c, 1);
    if (!px || (long)w * h > MAX_PIXELS) {
        snprintf(why, cap, "The QR picture couldn't be read");
        if (px) stbi_image_free(px);
        return false;
    }
    bool ok = false;
    struct quirc *q = quirc_new();
    if (q && quirc_resize(q, w, h) == 0) {
        memcpy(quirc_begin(q, NULL, NULL), px, (size_t)w * h);
        quirc_end(q);
        for (int i = 0, n = quirc_count(q); i < n && !ok; i++) {
            struct quirc_code code;
            struct quirc_data *data = malloc(sizeof *data);
            if (!data) break;
            quirc_extract(q, i, &code);
            if (quirc_decode(&code, data) == QUIRC_SUCCESS) ok = unwrap(data->payload, data->payload_len, sd);
            free(data);
        }
    }
    if (q) quirc_destroy(q);
    stbi_image_free(px);
    if (!ok) snprintf(why, cap, "No Mii QR code found in the picture");
    return ok;
}

// A picture of the page: <prefix><name>.jpg (a link or an <img>): /miis/qr_large/ the QR code, /miis/large/ the Mii.
static bool find_path(const char *html, const char *prefix, char *path, size_t cap) {
    for (const char *p = strstr(html, prefix); p; p = strstr(p + 1, prefix)) {
        size_t n = 0;
        while (isalnum((unsigned char)p[n]) || strchr("/_.-", p[n])) n++;
        if (n && n < cap) {
            memcpy(path, p, n), path[n] = 0;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- the link, the request
static void *mii_import_thread(void *unused);

int mii_import_start(const char *link, char *why, int cap) {
    const char *p = NULL;
    for (const char *q = link; *q; q++)
        if (!strncasecmp(q, "miicharacters.com/mii/", 22)) { p = q + 22; break; }
    int id = 0, n = 0;
    while (p && isdigit((unsigned char)p[n]) && n < 9) id = id * 10 + (p[n++] - '0');
    if (!p || !n || id <= 0) {
        snprintf(why, (size_t)cap, "That isn't a miicharacters.com Mii link (like " SITE "/mii/23844)");
        return 0;
    }
    pthread_mutex_lock(&g_lock);
    if (g_state == MII_IMP_BUSY) {
        pthread_mutex_unlock(&g_lock);
        snprintf(why, (size_t)cap, "Still reading the last link");
        return 0;
    }
    g_state = MII_IMP_BUSY;
    snprintf(g_url, sizeof g_url, SITE "/mii/%d", id);
    pthread_mutex_unlock(&g_lock);
    pthread_t t;
    if (pthread_create(&t, NULL, mii_import_thread, NULL)) {
        pthread_mutex_lock(&g_lock);
        g_state = MII_IMP_IDLE;
        pthread_mutex_unlock(&g_lock);
        snprintf(why, (size_t)cap, "Couldn't start reading the link");
        return 0;
    }
    pthread_detach(t);
    return 1;
}

static int get(const char *url, const char *accept, HttpResponse *r, char *why, size_t cap) {
    char hdr[96];
    snprintf(hdr, sizeof hdr, "Accept: %s\r\n", accept);
    int st = http_request("GET", url, hdr, NULL, 0, 20000, r);
    if (st == 200 && r->body) return 1;
    if (st == 404) snprintf(why, cap, "miicharacters.com has no Mii with that number");
    else if (st) snprintf(why, cap, "miicharacters.com answered %d", st);
    else snprintf(why, cap, "Couldn't reach miicharacters.com (%s)", r->error);
    return 0;
}

static void *mii_import_thread(void *unused) {
    (void)unused;
    char url[96];
    pthread_mutex_lock(&g_lock);
    snprintf(url, sizeof url, "%s", g_url);
    pthread_mutex_unlock(&g_lock);
    uint8_t d[0x58], sd[96];
    char name[48] = "", why[160] = "", path[128];
    bool ok = false;
    HttpResponse r, ri;
    char img[160];
    uint8_t *picture = NULL;
    size_t picture_len = 0;
    if (get(url, "text/html", &r, why, sizeof why)) {
        if (!find_path(r.body, "/miis/qr_large/", path, sizeof path)) snprintf(why, sizeof why, "That page has no Mii QR picture (a Wii-only Mii?)");
        else {
            snprintf(img, sizeof img, SITE "%s", path);
            if (get(img, "image/*", &ri, why, sizeof why)) {
                ok = read_qr((const uint8_t *)ri.body, ri.len, sd, why, sizeof why) && convert(sd, d, name, why, sizeof why);
                http_free(&ri);
            } else http_free(&ri);
            if (ok && find_path(r.body, "/miis/large/", path, sizeof path)) {  // (the picture: not needed for the Mii)
                snprintf(img, sizeof img, SITE "%s", path);
                char ignore[160];
                if (get(img, "image/*", &ri, ignore, sizeof ignore) && ri.len < 2000000) {
                    picture = malloc(ri.len);
                    if (picture) memcpy(picture, ri.body, ri.len), picture_len = ri.len;
                }
                http_free(&ri);
            }
        }
    }
    http_free(&r);
    LOG("[mii] import %s: %s\n", url, ok ? name : why);
    pthread_mutex_lock(&g_lock);
    free(g_img);
    g_img = ok ? picture : NULL, g_img_len = ok ? picture_len : 0;
    if (!ok) free(picture);
    if (ok) memcpy(g_out, d, sizeof d), snprintf(g_name, sizeof g_name, "%s", name);
    else snprintf(g_why, sizeof g_why, "%s", why);
    g_state = ok ? MII_IMP_DONE : MII_IMP_ERROR;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

MiiImportState mii_import_poll(uint8_t out[0x58], char name[48], char why[160], uint8_t **img, size_t *img_len) {
    pthread_mutex_lock(&g_lock);
    MiiImportState s = g_state;
    if (s == MII_IMP_DONE) {
        memcpy(out, g_out, 0x58), snprintf(name, 48, "%s", g_name);
        *img = g_img, *img_len = g_img_len;
        g_img = NULL, g_img_len = 0;
    }
    if (s == MII_IMP_ERROR) snprintf(why, 160, "%s", g_why);
    if (s == MII_IMP_DONE || s == MII_IMP_ERROR) g_state = MII_IMP_IDLE;
    pthread_mutex_unlock(&g_lock);
    return s;
}
