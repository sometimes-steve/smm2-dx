// Maker flags (issue #35): helpers for services/ssl.c that choose a country for a maker and put it into OpenCourse World's answers.
//
// OpenCourse World does not store where a maker is from: every user record in its answers has an empty country (a string of length 0).
// The game draws a maker's flag from that country, as a two-letter code ("JP", "US"), so the flag is given to the game here, in the
// answer, before it is parsed. Header-only so a test can include it on its own (no game, no mbedTLS).
//
//   nexc_pick       the country for one maker: the player's own flag if it is theirs and set, else a guess from the script of the name
//                   (kana is Japan, hangul is Korea, ...), else a pick that is the same for the same maker every time and that falls
//                   on the US and Japan most often. Only ever a code the game has a flag for (NEXC_FLAGS).
//   nexc_swap_in    one whole answer (the reassembled RMC message, not the packets it came in) in, the same answer out with a country
//                   in every user record that has none, and every length that grew corrected. 0 when the answer is not one it knows
//                   (it then goes to the game as it is).
#ifndef NEXCOUNTRY_H
#define NEXCOUNTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The flags in the game's own flag file (Model/Lyt_NationalFlag, textures.bntx): there is none for China, Taiwan or Saudi Arabia.
static const char *const NEXC_FLAGS[] = {
    "AE", "AL", "AO", "AR", "AT", "AU", "AW", "AZ", "BA", "BB", "BE", "BG", "BN", "BO", "BR", "BS", "BW", "BY", "BZ", "CA", "CH", "CL",
    "CO", "CY", "CZ", "DE", "DK", "DO", "EC", "EE", "EG", "ES", "FI", "FR", "GB", "GH", "GR", "GT", "HN", "HR", "HU", "ID", "IE", "IL",
    "IN", "IS", "IT", "JM", "JO", "JP", "KR", "KW", "KZ", "LB", "LT", "LU", "LV", "MA", "MD", "MG", "MK", "ML", "MN", "MR", "MT", "MU",
    "MX", "MY", "MZ", "NA", "NG", "NI", "NL", "NO", "NZ", "OM", "PA", "PE", "PG", "PH", "PK", "PL", "PT", "PY", "QA", "RO", "RS", "RU",
    "RW", "SC", "SE", "SG", "SI", "SK", "SR", "SV", "SZ", "TD", "TH", "TN", "TR", "TT", "TZ", "UA", "UG", "US", "VE", "VN", "ZM", "ZW",
};

// The same flags by name, in the same order (for the menu).
static const char *const NEXC_NAMES[] = {
    "United Arab Emirates",
    "Albania",
    "Angola",
    "Argentina",
    "Austria",
    "Australia",
    "Aruba",
    "Azerbaijan",
    "Bosnia and Herzegovina",
    "Barbados",
    "Belgium",
    "Bulgaria",
    "Brunei",
    "Bolivia",
    "Brazil",
    "Bahamas",
    "Botswana",
    "Belarus",
    "Belize",
    "Canada",
    "Switzerland",
    "Chile",
    "Colombia",
    "Cyprus",
    "Czechia",
    "Germany",
    "Denmark",
    "Dominican Republic",
    "Ecuador",
    "Estonia",
    "Egypt",
    "Spain",
    "Finland",
    "France",
    "United Kingdom",
    "Ghana",
    "Greece",
    "Guatemala",
    "Honduras",
    "Croatia",
    "Hungary",
    "Indonesia",
    "Ireland",
    "Israel",
    "India",
    "Iceland",
    "Italy",
    "Jamaica",
    "Jordan",
    "Japan",
    "South Korea",
    "Kuwait",
    "Kazakhstan",
    "Lebanon",
    "Lithuania",
    "Luxembourg",
    "Latvia",
    "Morocco",
    "Moldova",
    "Madagascar",
    "North Macedonia",
    "Mali",
    "Mongolia",
    "Mauritania",
    "Malta",
    "Mauritius",
    "Mexico",
    "Malaysia",
    "Mozambique",
    "Namibia",
    "Nigeria",
    "Nicaragua",
    "Netherlands",
    "Norway",
    "New Zealand",
    "Oman",
    "Panama",
    "Peru",
    "Papua New Guinea",
    "Philippines",
    "Pakistan",
    "Poland",
    "Portugal",
    "Paraguay",
    "Qatar",
    "Romania",
    "Serbia",
    "Russia",
    "Rwanda",
    "Seychelles",
    "Sweden",
    "Singapore",
    "Slovenia",
    "Slovakia",
    "Suriname",
    "El Salvador",
    "Eswatini",
    "Chad",
    "Thailand",
    "Tunisia",
    "Turkey",
    "Trinidad and Tobago",
    "Tanzania",
    "Ukraine",
    "Uganda",
    "United States",
    "Venezuela",
    "Vietnam",
    "Zambia",
    "Zimbabwe",
};

static inline bool nexc_has_flag(const char *code) {
    if (!code || !code[0] || !code[1] || code[2]) return false;
    for (size_t i = 0; i < sizeof NEXC_FLAGS / sizeof NEXC_FLAGS[0]; i++)
        if (code[0] == NEXC_FLAGS[i][0] && code[1] == NEXC_FLAGS[i][1]) return true;
    return false;
}

static inline uint32_t nexc_hash(const char *s) {  // FNV-1a
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

// ---- a guess from the script the name is written in
static inline size_t nexc_utf8(const uint8_t *s, size_t len, size_t i, uint32_t *cp) {  // length of the character at i, 0 if not UTF-8
    uint8_t b = s[i];
    size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
    if (!n || i + n > len) return 0;
    uint32_t c = n == 1 ? b : b & (0xffu >> (n + 1));
    for (size_t k = 1; k < n; k++) {
        if ((s[i + k] & 0xc0) != 0x80) return 0;
        c = c << 6 | (s[i + k] & 0x3f);
    }
    *cp = c;
    return n;
}

// Returns a country, or NULL when the name says nothing (plain Latin letters, digits, symbols). `seed` picks among countries that share
// a script (Arabic). The guess is a language's, not a place's: Japanese kana is Japan, but Cyrillic is Russia (or Ukraine for the
// letters only Ukrainian has) whoever wrote it; Chinese characters alone give Japan, since the game has no flag for China or Taiwan.
static inline const char *nexc_by_script(const uint8_t *name, size_t len, const char *seed) {
    unsigned kana = 0, hangul = 0, han = 0, thai = 0, cyr = 0, ukr = 0, greek = 0, hebrew = 0, arabic = 0, deva = 0, viet = 0;
    unsigned tr = 0, pl = 0, pt = 0, es = 0, de = 0;
    uint32_t cp;
    for (size_t i = 0, n; i < len; i += n) {
        if (!(n = nexc_utf8(name, len, i, &cp))) return NULL;  // (not UTF-8: no guess)
        if ((cp >= 0x3040 && cp <= 0x30ff) || (cp >= 0x31f0 && cp <= 0x31ff) || (cp >= 0xff66 && cp <= 0xff9f)) kana++;
        else if ((cp >= 0xac00 && cp <= 0xd7af) || (cp >= 0x1100 && cp <= 0x11ff) || (cp >= 0x3130 && cp <= 0x318f)) hangul++;
        else if ((cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0x3400 && cp <= 0x4dbf)) han++;
        else if (cp >= 0x0e00 && cp <= 0x0e7f) thai++;
        else if (cp == 0x0454 || cp == 0x0456 || cp == 0x0457 || cp == 0x0491 || cp == 0x0404 || cp == 0x0406 || cp == 0x0407 || cp == 0x0490) cyr++, ukr++;
        else if (cp >= 0x0400 && cp <= 0x04ff) cyr++;
        else if (cp >= 0x0370 && cp <= 0x03ff) greek++;
        else if (cp >= 0x0590 && cp <= 0x05ff) hebrew++;
        else if ((cp >= 0x0600 && cp <= 0x06ff) || (cp >= 0x0750 && cp <= 0x077f)) arabic++;
        else if (cp >= 0x0900 && cp <= 0x097f) deva++;
        else if (cp >= 0x1ea0 && cp <= 0x1ef9) viet++;  // (Latin letters with the stacked marks only Vietnamese uses)
        else if (cp == 0x011f || cp == 0x011e || cp == 0x015f || cp == 0x015e || cp == 0x0131 || cp == 0x0130) tr++;
        else if (cp == 0x0142 || cp == 0x0105 || cp == 0x0119 || cp == 0x015b || cp == 0x017a || cp == 0x017c || cp == 0x0107 || cp == 0x0144) pl++;
        else if (cp == 0x00e3 || cp == 0x00f5 || cp == 0x00c3 || cp == 0x00d5) pt++;
        else if (cp == 0x00f1 || cp == 0x00d1 || cp == 0x00bf || cp == 0x00a1) es++;
        else if (cp == 0x00df) de++;
    }
    if (kana) return "JP";
    if (hangul) return "KR";
    if (han) return "JP";
    if (thai) return "TH";
    if (viet) return "VN";
    if (cyr) return ukr ? "UA" : "RU";
    if (greek) return "GR";
    if (hebrew) return "IL";
    if (deva) return "IN";
    if (arabic) {
        static const char *const ar[] = {"EG", "AE", "MA", "JO", "LB", "KW", "QA", "OM"};
        return ar[nexc_hash(seed ? seed : "") % (sizeof ar / sizeof ar[0])];
    }
    if (tr) return "TR";
    if (pl) return "PL";
    if (pt) return "BR";
    if (es) return "ES";
    if (de) return "DE";
    return NULL;
}

// ---- the pick for a maker with no hint: the same for the same maker, every time. US 35%, Japan 30%, the rest 35% spread over the
// countries makers most often come from.
static inline const char *nexc_random(const char *seed) {
    static const struct { const char *code; unsigned weight; } W[] = {
        {"US", 35}, {"JP", 30}, {"GB", 4}, {"CA", 4}, {"MX", 3}, {"BR", 3}, {"DE", 3}, {"FR", 3}, {"ES", 3},
        {"KR", 2}, {"AU", 2}, {"IT", 2}, {"NL", 2}, {"PL", 1}, {"SE", 1}, {"PH", 1}, {"NZ", 1},
    };
    unsigned r = nexc_hash(seed ? seed : "") % 100, acc = 0;
    for (size_t i = 0; i < sizeof W / sizeof W[0]; i++)
        if (r < (acc += W[i].weight)) return W[i].code;
    return "US";
}

// The country for one maker. `maker` is the maker's code as the answer has it (nine characters), `name` the name's UTF-8 bytes;
// `own_maker` / `own_flag` are the player's own maker code and chosen flag (either may be NULL or empty).
static inline const char *nexc_pick(const char *maker, const uint8_t *name, size_t name_len, const char *own_maker, const char *own_flag) {
    if (own_maker && own_maker[0] && own_flag && nexc_has_flag(own_flag) && maker && !strcmp(maker, own_maker)) return own_flag;
    const char *guess = nexc_by_script(name, name_len, maker);
    if (guess && nexc_has_flag(guess)) return guess;
    return nexc_random(maker);
}

// ---- putting it into an answer
// An RMC response is: u32 size (of what follows), u8 protocol, u8 success, u32 call id, u32 method id (| 0x8000), then the result. The
// leaderboards and the maker pages (DataStore, protocol 0x73; many methods answer this way, so the layout is what decides, not the
// method) answer with a list of user records and then, for a board, a list of ranks: u32
// count, then per record a structure header (u8 version, u32 length of what follows) and its content: u64 pid, maker code (u16 length,
// characters, NUL), name (the same), a structure (u8 version, u32 length, content), a buffer (u16 length, bytes: the Mii), the country
// (u16 length, characters, NUL: length 0 when there is none), the rest. A country of "JP" is the bytes 03 00 'J' 'P' 00 where there were 00 00.
#define NEXC_PROTO_DATASTORE 0x73
#define NEXC_GROW 3  // bytes a record grows by

static inline uint32_t nexc_rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint16_t nexc_rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline void nexc_wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

// Where the country's length is in one record's content `c` (`n` bytes): 0 if the record is not laid out as above.
static inline size_t nexc_country_at(const uint8_t *c, size_t n, const uint8_t **code, size_t *code_len, const uint8_t **name, size_t *name_len) {
    size_t o = 8;  // (the pid)
    if (o + 2 > n) return 0;
    size_t cl = nexc_rd16(c + o);
    if (cl != 10 || o + 2 + cl > n || c[o + 2 + cl - 1] != 0) return 0;
    *code = c + o + 2, *code_len = cl - 1;
    for (size_t k = 0; k < cl - 1; k++)
        if (!((c[o + 2 + k] >= '0' && c[o + 2 + k] <= '9') || (c[o + 2 + k] >= 'A' && c[o + 2 + k] <= 'Z'))) return 0;
    o += 2 + cl;
    if (o + 2 > n) return 0;
    size_t nl = nexc_rd16(c + o);
    if (nl < 1 || nl > 200 || o + 2 + nl > n || c[o + 2 + nl - 1] != 0) return 0;
    *name = c + o + 2, *name_len = nl - 1;
    o += 2 + nl;
    if (o + 5 > n) return 0;
    size_t sl = nexc_rd32(c + o + 1);
    if (o + 5 + sl > n) return 0;
    o += 5 + sl;
    if (o + 2 > n) return 0;
    size_t ql = nexc_rd16(c + o);
    if (o + 2 + ql > n) return 0;
    o += 2 + ql;
    if (o + 2 > n) return 0;
    return o;
}

// `in`: one whole RMC response (`n` bytes). `out`: room for `cap` bytes (n + 3 per record is enough). Returns the new length, with the
// number of records given a country in *changed; 0 (and *changed 0) when the answer is not one of these or nothing needed one.
static inline size_t nexc_swap_in(const uint8_t *in, size_t n, uint8_t *out, size_t cap, const char *own_maker, const char *own_flag, int *changed) {
    *changed = 0;
    if (n < 14 + 4 || nexc_rd32(in) != n - 4 || in[4] != NEXC_PROTO_DATASTORE || in[5] != 1) return 0;
    size_t count = nexc_rd32(in + 14);
    if (count == 0 || count > 1000) return 0;
    // first pass: lay the records out and count the ones that need a country (nothing is written until all are known to fit)
    size_t pos = 18, need = 0;
    for (size_t r = 0; r < count; r++) {
        if (pos + 5 > n) return 0;
        size_t len = nexc_rd32(in + pos + 1);
        if (pos + 5 + len > n) return 0;
        const uint8_t *code, *name;
        size_t code_len, name_len;
        size_t o = nexc_country_at(in + pos + 5, len, &code, &code_len, &name, &name_len);
        if (!o) return 0;
        if (nexc_rd16(in + pos + 5 + o) == 0) need++;
        pos += 5 + len;
    }
    if (!need || n + need * NEXC_GROW > cap) return 0;
    // second pass: copy, with the country put in
    memcpy(out, in, 18);
    size_t w = 18;
    pos = 18;
    for (size_t r = 0; r < count; r++) {
        size_t len = nexc_rd32(in + pos + 1);
        const uint8_t *c = in + pos + 5, *code, *name;
        size_t code_len, name_len;
        size_t o = nexc_country_at(c, len, &code, &code_len, &name, &name_len);
        if (nexc_rd16(c + o) != 0) {  // (it has one already)
            memcpy(out + w, in + pos, 5 + len);
            w += 5 + len, pos += 5 + len;
            continue;
        }
        char maker[12];
        memcpy(maker, code, code_len), maker[code_len] = 0;
        const char *country = nexc_pick(maker, name, name_len, own_maker, own_flag);
        memcpy(out + w, in + pos, 5);                      // (the structure header ...)
        nexc_wr32(out + w + 1, (uint32_t)(len + NEXC_GROW));  // (... with its length grown)
        memcpy(out + w + 5, c, o);                         // (everything up to the country)
        out[w + 5 + o] = 3, out[w + 5 + o + 1] = 0, out[w + 5 + o + 2] = (uint8_t)country[0], out[w + 5 + o + 3] = (uint8_t)country[1], out[w + 5 + o + 4] = 0;
        memcpy(out + w + 5 + o + 5, c + o + 2, len - o - 2);  // (and the rest)
        w += 5 + len + NEXC_GROW, pos += 5 + len;
        (*changed)++;
    }
    memcpy(out + w, in + pos, n - pos);  // (what follows the list: the ranks)
    w += n - pos;
    nexc_wr32(out, (uint32_t)(w - 4));
    return w;
}

#endif
