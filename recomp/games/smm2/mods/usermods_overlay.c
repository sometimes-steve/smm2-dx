// The player's installed mods (runtime/usermods.h, the Mods tab) in the game's RomFS: the runtime asks the game for
// folders to lay over it (game_romfs_overlay, runtime/game_hooks.c: none by default; runtime/services/fs.c and
// runtime/mods.c read it, one folder per line, the last winning). Without this the installed mods were never used.
//
// A mod's archives (.pack / .sarc) that the game has too are laid over the game's file member by member, not instead of
// it (the idea of upstream's romfs_overlay.c merge). A mod made for an older game version carries only that version's
// members; replacing the whole archive would take away what the updates added, and the game then asks for a member that
// is no longer there and stops (*** exception c0000005 accessing 0x8, with "Peachette over Toadette", GameBanana 252745).
//  - a member the mod has replaces the game's (at the game's alignment)
//  - a member only the mod has is added
//  - everything else stays the game's own
//  - several mods that touch the same archive stack in priority order (a later one wins)
// The merged archives go to mods-generated/romfs-usermods, the last folder of the overlay (it wins over the mods' own),
// and are made again only when the set of enabled mods changes. (Yaz0-packed .szs are not merged: they replace.)
#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mod.h"
#include "../../../runtime/platform.h"
#include "../../../runtime/usermods.h"

extern const char *g_romfs_dir;  // runtime/main.c: the game's own RomFS folder (extracted/romfs)

#define UM_DIR "mods-generated/romfs-usermods"
#define UM_ID UM_DIR ".id"

// ---------------------------------------------------------------- files
typedef struct { uint8_t *d; size_t n; } Blob;
static bool read_file(const char *path, Blob *b) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b->d = malloc(n > 0 ? (size_t)n : 1);
    b->n = n > 0 ? fread(b->d, 1, (size_t)n, f) : 0;
    fclose(f);
    return n >= 0 && b->n == (size_t)n;
}
static void mkdirs(const char *file_path) {  // (the folders above a file)
    char p[1200];
    snprintf(p, sizeof p, "%s", file_path);
    for (char *s = p; *s; s++)
        if (*s == '/' && s > p) {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
}
static bool write_file(const char *path, const void *d, size_t n) {
    mkdirs(path);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(d, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

// ---------------------------------------------------------------- SARC (little endian; as the Switch games' packs)
typedef struct { char *name; const uint8_t *d; uint32_t n, align; uint32_t hash; } Member;
typedef struct { Member *m; int n; } Sarc;

static uint32_t u16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | u16(p + 2) << 16; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }
static uint32_t align_of(uint32_t start) {  // (the largest power of two the member's start is a multiple of, 4..0x2000)
    uint32_t a = 4;
    while (a < 0x2000 && !(start & (a * 2 - 1))) a *= 2;
    return a;
}
static void sarc_free(Sarc *s) {
    for (int i = 0; i < s->n; i++) free(s->m[i].name);
    free(s->m);
    s->m = NULL, s->n = 0;
}
// d must stay alive as long as the members are used
static bool sarc_parse(const uint8_t *d, size_t len, Sarc *s) {
    memset(s, 0, sizeof *s);
    if (len < 0x20 || memcmp(d, "SARC", 4) || u16(d + 6) != 0xFEFF || memcmp(d + 0x14, "SFAT", 4)) return false;
    uint32_t data_off = u32(d + 0xC), count = u16(d + 0x1A);
    size_t sfnt = 0x20 + (size_t)count * 16;
    if (sfnt + 8 > len || memcmp(d + sfnt, "SFNT", 4) || data_off > len) return false;
    s->m = calloc(count ? count : 1, sizeof *s->m);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *nd = d + 0x20 + (size_t)i * 16;
        uint32_t attr = u32(nd + 4), start = u32(nd + 8), end = u32(nd + 12);
        size_t no = sfnt + 8 + (size_t)(attr & 0xFFFFFF) * 4;
        if (!(attr >> 24) || no >= len || end < start || (size_t)data_off + end > len) { sarc_free(s); return false; }
        size_t nl = strnlen((const char *)d + no, len - no);
        if (no + nl >= len) { sarc_free(s); return false; }
        Member *m = &s->m[s->n++];
        m->name = malloc(nl + 1);
        memcpy(m->name, d + no, nl), m->name[nl] = 0;
        m->d = d + data_off + start, m->n = end - start, m->align = align_of(start), m->hash = u32(nd);
    }
    return true;
}
static int by_hash(const void *a, const void *b) {
    uint32_t x = ((const Member *)a)->hash, y = ((const Member *)b)->hash;
    return x < y ? -1 : x > y;
}
static uint8_t *sarc_write(Member *m, int n, size_t *out_len) {
    qsort(m, (size_t)n, sizeof *m, by_hash);
    size_t names = 0;
    uint32_t max_align = 4;
    for (int i = 0; i < n; i++) {
        names += strlen(m[i].name) + 1;
        names = (names + 3) & ~(size_t)3;
        if (m[i].align > max_align) max_align = m[i].align;
    }
    size_t head = 0x20 + (size_t)n * 16 + 8 + names;
    size_t data_off = (head + max_align - 1) & ~(size_t)(max_align - 1);
    size_t pos = 0;
    for (int i = 0; i < n; i++) pos = ((pos + m[i].align - 1) & ~(size_t)(m[i].align - 1)) + m[i].n;
    size_t total = data_off + pos;
    uint8_t *o = calloc(total ? total : 1, 1);
    memcpy(o, "SARC", 4), put16(o + 4, 0x14), put16(o + 6, 0xFEFF), put32(o + 8, (uint32_t)total), put32(o + 0xC, (uint32_t)data_off),
        put16(o + 0x10, 0x100);
    memcpy(o + 0x14, "SFAT", 4), put16(o + 0x18, 0xC), put16(o + 0x1A, (uint32_t)n), put32(o + 0x1C, 0x65);
    size_t sfnt = 0x20 + (size_t)n * 16;
    memcpy(o + sfnt, "SFNT", 4), put16(o + sfnt + 4, 8);
    size_t no = 0;
    pos = 0;
    for (int i = 0; i < n; i++) {
        pos = (pos + m[i].align - 1) & ~(size_t)(m[i].align - 1);
        uint8_t *nd = o + 0x20 + (size_t)i * 16;
        put32(nd, m[i].hash), put32(nd + 4, 0x01000000u | (uint32_t)(no / 4)), put32(nd + 8, (uint32_t)pos),
            put32(nd + 12, (uint32_t)(pos + m[i].n));
        memcpy(o + sfnt + 8 + no, m[i].name, strlen(m[i].name));
        no += strlen(m[i].name) + 1;
        no = (no + 3) & ~(size_t)3;
        memcpy(o + data_off + pos, m[i].d, m[i].n);
        pos += m[i].n;
    }
    *out_len = total;
    return o;
}

// Lay mod archive `mod` over `cur` (both SARC files in memory); a new buffer (malloc'd), or NULL when either is not one
static uint8_t *sarc_merge(const Blob *cur, const Blob *mod, size_t *out_len, int *game_n, int *replaced, int *added) {
    Sarc a, b;
    if (!sarc_parse(cur->d, cur->n, &a)) return NULL;
    if (!sarc_parse(mod->d, mod->n, &b)) { sarc_free(&a); return NULL; }
    *game_n = a.n, *replaced = *added = 0;
    Member *all = malloc(sizeof *all * (size_t)(a.n + b.n + 1));
    int n = 0;
    for (int i = 0; i < a.n; i++) all[n++] = a.m[i];
    for (int i = 0; i < b.n; i++) {
        int k = 0;
        for (; k < a.n; k++)
            if (!strcmp(all[k].name, b.m[i].name)) break;
        if (k < a.n) {  // (the game's alignment stays; "replaced" counts the members that really differ from the game's)
            if (all[k].n != b.m[i].n || memcmp(all[k].d, b.m[i].d, b.m[i].n)) (*replaced)++;
            all[k].d = b.m[i].d, all[k].n = b.m[i].n;
        }
        else all[n++] = b.m[i], (*added)++;
    }
    uint8_t *o = sarc_write(all, n, out_len);
    free(all);
    sarc_free(&a), sarc_free(&b);  // (the names were copied into the members; the data is still in cur / mod)
    return o;
}

// ---------------------------------------------------------------- the mods' archives
static bool is_archive(const char *name) {
    size_t l = strlen(name);
    return (l > 5 && !strcmp(name + l - 5, ".pack")) || (l > 5 && !strcmp(name + l - 5, ".sarc"));
}
static void merge_file(const char *id, const char *rel) {
    char mod_path[1200], out_path[1200], base_path[1200];
    snprintf(mod_path, sizeof mod_path, USERMODS_DIR "/%s/romfs/%s", id, rel);
    snprintf(out_path, sizeof out_path, UM_DIR "/%s", rel);
    snprintf(base_path, sizeof base_path, "%s/%s", g_romfs_dir, rel);
    Blob cur = {0}, mod = {0};
    bool earlier = read_file(out_path, &cur);  // (an earlier mod's merge of it)
    if (!earlier && !read_file(base_path, &cur)) { free(cur.d); return; }  // (the game has no such file: the mod's joins as it is)
    if (!read_file(mod_path, &mod)) { free(cur.d); return; }
    size_t n = 0;
    int game_n = 0, replaced = 0, added = 0;
    uint8_t *o = sarc_merge(&cur, &mod, &n, &game_n, &replaced, &added);
    if (o) {
        write_file(out_path, o, n);
        LOG("[usermods] %s: %s joins the game's (%d members there: %d replaced, %d added)\n", id, rel, game_n, replaced, added);
        free(o);
    } else if (earlier) {
        write_file(out_path, mod.d, mod.n);  // (not a SARC: this mod's file wins over the earlier merge)
    }
    free(cur.d), free(mod.d);
}
static void walk(const char *id, const char *root, const char *sub) {
    char dir[1200];
    snprintf(dir, sizeof dir, "%s%s%s", root, sub[0] ? "/" : "", sub);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char rel[1200], full[1300];
        snprintf(rel, sizeof rel, "%s%s%s", sub, sub[0] ? "/" : "", de->d_name);
        snprintf(full, sizeof full, "%s/%s", root, rel);
        struct stat st;
        if (stat(full, &st)) continue;
        if (S_ISDIR(st.st_mode)) walk(id, root, rel);
        else if (S_ISREG(st.st_mode) && is_archive(de->d_name)) merge_file(id, rel);
    }
    closedir(d);
}
// Made again when the set of enabled mods (or the game's files) is not what the folder was made from
static bool merge_user_archives(void) {
    if (!g_romfs_dir) return false;
    char ident[8192], saved[8192] = "";
    usermods_identity(ident, sizeof ident);
    strncat(ident, g_romfs_dir, sizeof ident - strlen(ident) - 1);
    Blob b = {0};
    if (read_file(UM_ID, &b)) {
        size_t n = b.n < sizeof saved - 1 ? b.n : sizeof saved - 1;
        memcpy(saved, b.d, n), saved[n] = 0;
        free(b.d);
    }
    struct stat st;
    if (!strcmp(saved, ident) && !stat(UM_DIR, &st)) return true;  // (as made before)
    plat_remove_tree(UM_DIR);
    for (int i = 0, n = usermods_count(); i < n; i++) {
        UserMod m;
        if (!usermods_get(i, &m) || !m.enabled || !m.has_romfs) continue;
        char root[1200];
        snprintf(root, sizeof root, USERMODS_DIR "/%s/romfs", m.id);
        walk(m.id, root, "");  // (in priority order: a later mod's merge is over an earlier one's)
    }
    write_file(UM_ID, ident, strlen(ident));
    return !stat(UM_DIR, &st);
}

// ---------------------------------------------------------------- the name font's fallback
// A maker named in simplified Chinese showed one of its characters as a "?" box. The game draws names and titles with the
// font chain MessageFont (Font/Fcpx.szs): its own two Japanese gothics, with nothing for simplified-only characters; only its
// Chinese variant, MessageFont_CNzh, goes on to the simplified Chinese font (DFHEI5A). The Chinese chain is the same fonts
// first and that font last, so using it for every language changes no character that shows today and adds the ones that
// were "?". Made from the player's own Fcpx.szs into mods-generated/romfs-fixes (laid under the mods in the overlay, so a mod
// that has its own Fcpx.szs still wins), never edited in place (the extracted files can be hard links to the dump) and never
// shipped. RECOMP_NO_FONT_FALLBACK=1 leaves the game's own.
// The game has such a chain for each of its Asian languages, and each adds one more font (simplified Chinese DFHEI5A,
// traditional Chinese DFPT_3, Korean AsiaKDREAM4-R). A chain with all of them is not made: the file is a linked binary
// format that is not understood well enough to add to, and a wrong edit would break every line of text. So the setting
// "Name Fallback Font" (General > Appearance) picks which of the game's own chains every language uses.
#define FIX_DIR "mods-generated/romfs-fixes"
#define FIX_ID FIX_DIR ".id"
#define FIX_FILE FIX_DIR "/Font/Fcpx.szs"
#define FIX_VERSION "fcpx-2"
#include "../../../runtime/settings.h"

static uint8_t *yaz0_decode(const uint8_t *s, size_t n, size_t *out_n) {  // NULL when it isn't a Yaz0 file
    if (n < 16 || memcmp(s, "Yaz0", 4)) return NULL;
    size_t size = (size_t)s[4] << 24 | (size_t)s[5] << 16 | (size_t)s[6] << 8 | s[7];
    if (size > (64u << 20)) return NULL;
    uint8_t *o = malloc(size ? size : 1);
    size_t i = 16, k = 0;
    while (k < size && i < n) {
        uint8_t code = s[i++];
        for (int b = 0; b < 8 && k < size; b++) {
            if (code & (0x80 >> b)) {
                if (i >= n) goto bad;
                o[k++] = s[i++];
            } else {
                if (i + 1 >= n) goto bad;
                uint8_t b1 = s[i], b2 = s[i + 1];
                i += 2;
                size_t dist = ((size_t)(b1 & 0xF) << 8 | b2) + 1, len = b1 >> 4;
                if (len == 0) {
                    if (i >= n) goto bad;
                    len = (size_t)s[i++] + 0x12;
                } else len += 2;
                if (dist > k) goto bad;
                for (size_t c = 0; c < len && k < size; c++, k++) o[k] = o[k - dist];
            }
        }
    }
    if (k != size) goto bad;
    *out_n = size;
    return o;
bad:
    free(o);
    return NULL;
}

// Yaz0 without compression: every group is eight literal bytes (the game's decoder reads it as any other)
static uint8_t *yaz0_store(const uint8_t *d, size_t n, size_t *out_n) {
    size_t groups = (n + 7) / 8, total = 16 + n + groups;
    uint8_t *o = calloc(total, 1);
    memcpy(o, "Yaz0", 4);
    o[4] = (uint8_t)(n >> 24), o[5] = (uint8_t)(n >> 16), o[6] = (uint8_t)(n >> 8), o[7] = (uint8_t)n;
    size_t w = 16;
    for (size_t i = 0; i < n; i += 8) {
        o[w++] = 0xFF;
        size_t c = n - i < 8 ? n - i : 8;
        memcpy(o + w, d + i, c);
        w += c;
    }
    *out_n = w;
    return o;
}

static const char *chain_for(int name_font) {  // the game's chain whose last font is the one wanted
    return name_font == NAMEFONT_TRADITIONAL ? "MessageFont_TWzh.bfcpx" : name_font == NAMEFONT_KOREAN ? "MessageFont_KRko.bfcpx"
                                                                                                        : "MessageFont_CNzh.bfcpx";
}

static bool font_fix_make(void) {
    char base[1200];
    snprintf(base, sizeof base, "%s/Font/Fcpx.szs", g_romfs_dir);
    Blob src = {0};
    if (!read_file(base, &src)) { free(src.d); return false; }
    size_t raw_n = 0;
    uint8_t *raw = yaz0_decode(src.d, src.n, &raw_n);
    free(src.d);
    if (!raw) return false;
    Sarc a;
    bool ok = false;
    if (sarc_parse(raw, raw_n, &a)) {
        int plain = -1, cn = -1;
        for (int i = 0; i < a.n; i++) {
            if (!strcmp(a.m[i].name, "MessageFont.bfcpx")) plain = i;
            if (!strcmp(a.m[i].name, chain_for(g_settings.name_font))) cn = i;
        }
        if (plain >= 0 && cn >= 0) {
            a.m[plain].d = a.m[cn].d, a.m[plain].n = a.m[cn].n;
            size_t out_n = 0, st_n = 0;
            uint8_t *out = sarc_write(a.m, a.n, &out_n);
            uint8_t *packed = out ? yaz0_store(out, out_n, &st_n) : NULL;
            if (packed) ok = write_file(FIX_FILE, packed, st_n);
            free(out), free(packed);
        }
        sarc_free(&a);
    }
    free(raw);
    return ok;
}

// The folder to lay under the mods, or false (not wanted, or the game's file is not as expected: the game's own is used)
static bool font_fix(void) {
    const char *off = getenv("RECOMP_NO_FONT_FALLBACK");
    if ((off && atoi(off)) || !g_romfs_dir || g_settings.name_font == NAMEFONT_GAME) return false;
    char ident[1300], saved[1300] = "";
    snprintf(ident, sizeof ident, "%s|" FIX_VERSION "|%d", g_romfs_dir, g_settings.name_font);
    Blob b = {0};
    if (read_file(FIX_ID, &b)) {
        size_t n = b.n < sizeof saved - 1 ? b.n : sizeof saved - 1;
        memcpy(saved, b.d, n), saved[n] = 0;
        free(b.d);
    }
    struct stat st;
    if (!strcmp(saved, ident) && !stat(FIX_FILE, &st)) return true;  // (as made before)
    plat_remove_tree(FIX_DIR);
    if (!font_fix_make()) {
        LOG("[romfs-fixes] Font/Fcpx.szs: not as expected, the game's own is used\n");
        return false;
    }
    write_file(FIX_ID, ident, strlen(ident));
    LOG("[romfs-fixes] Font/Fcpx.szs: names use the game's %s font chain in every language\n", chain_for(g_settings.name_font));
    return true;
}

// ---------------------------------------------------------------- the overlay
static char g_overlay[8192];
static bool g_built;

// made once per boot (the runtime asks for every file it looks up), again after an in-place reboot
RT_ON_RESET(usermods_overlay_reset) { g_built = false; }

const char *game_romfs_overlay(void) {
    if (!g_built) {
        size_t l = 0;
        if (font_fix()) l += (size_t)snprintf(g_overlay, sizeof g_overlay, FIX_DIR "\n");  // (first: the mods win over it)
        size_t lm = usermods_overlay(g_overlay + l, sizeof g_overlay - l);
        l += lm;
        if (lm && merge_user_archives() && l + sizeof UM_DIR + 2 < sizeof g_overlay)
            l += (size_t)snprintf(g_overlay + l, sizeof g_overlay - l, UM_DIR "\n");  // (last: it wins)
        g_built = true;
    }
    return g_overlay[0] ? g_overlay : NULL;
}
