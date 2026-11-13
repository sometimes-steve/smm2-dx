// The Search tab's data: courses from OpenCourse World, filtered here, and a random pick (issue #24).
//
// The service's REST API has no course search; it lists every course, newest first, a page at a time:
//   GET https://<domain>/api/latest/courses?offset=N&size=100   (100 is the most a page holds)
// so the whole list (about 30,000 courses, 300 pages) is read once, on four threads, into a compact list (32 bytes a course) and the
// filters run over that. "Unplayed" needs the player's own played list:
//   GET https://<domain>/api/makers/<maker code>/played?offset=N&size=100   ->  [{ "course_id": ... }]
// A pick is checked with the service before it is shown, since the list was read some time ago:
//   GET https://<domain>/api/courses/<course id>   ->  one course (title, owner, plays, tries, clears)
// and one that has been cleared since (when "uncleared" is asked) or deleted is dropped and another is drawn.
//
// The lists are kept in ocw-courses.bin (the data folder), so the next start has them at once: the course list is then only topped up
// (the newest pages, until one holds nothing new) and read in full again after three days or on a refresh; the played list the same way.
//
// Taking a course to the game: game_search_go leaves its ID for the next Course ID prompt (runtime/hle/swkbd.c starts it with it).
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../../runtime/game_rows.h"
#include "../../../runtime/net/http.h"
#include "../../../runtime/net/json.h"
#include "../../../runtime/settings.h"
#include "../../../third_party/stb/stb_image.h"
#include "mod.h"
#include "ocw.h"

#define SEARCH_PAGE 100
#define SEARCH_MAX_PAGES 1000  // (100,000 courses: more than the service has, a stop for a service that never ends a list)
#define SEARCH_WORKERS 4
#define SEARCH_TIMEOUT_MS 15000
#define PLAYED_MAX_PAGES 200   // (20,000 played courses)

typedef struct {
    char id[12];
    uint8_t style, theme, tag1, tag2, has_cc, dead, mark;  // mark: the full read that last saw it
    uint32_t clears, tries, ms;
} Entry;

#define CACHE_FILE "ocw-courses.bin"
#define CACHE_MAGIC 0x31435253u  // "SRC1"
#define CACHE_FULL_AFTER (3 * 24 * 3600)

static struct {
    pthread_mutex_t lock;
    Entry *a;
    size_t n, cap;
    uint64_t *played;  // sorted, packed IDs
    size_t np;
    bool started, played_started;
    bool fetching, played_loading, played_known;
    bool incremental, played_full;  // the walk running tops the list up / the next played walk reads everything
    uint8_t epoch;
    time_t retry, played_retry, saved;
    struct { uint64_t key; uint32_t idx; } *ht;  // (id -> place in a[]; open addressing)
    size_t ht_cap;
    char err[96];
    // the page walk
    atomic_uint gen;
    atomic_int next_page, end_page, workers, fails;
    // the pick
    int roll_state;
    bool rolling;
    SearchCourse roll_out;
    char roll_err[96];
    char pending[12];
    time_t pending_at;
} S = {.lock = PTHREAD_MUTEX_INITIALIZER};

static const char *const STYLES[] = {"SMB1", "SMB3", "SMW", "NSMBU", "SM3DW"};
static const char *const THEMES[] = {"Ground", "Underground", "Castle", "Airship", "Underwater", "Ghost House", "Snow", "Desert", "Sky", "Forest"};

static uint8_t index_of(const char *const *names, int n, const char *s) {
    for (int i = 0; i < n; i++) if (!strcmp(names[i], s)) return (uint8_t)i;
    return 255;
}

// "AAA-BBB-CCC" -> 45 bits (an ID's characters are digits and consonants: 31 of them); 0 for anything else
static uint64_t pack_id(const char *id) {
    static const char AL[] = "0123456789BCDFGHJKLMNPQRSTVWXY";
    uint64_t v = 1;  // (a leading 1: no real ID packs to 0)
    int k = 0;
    for (const char *p = id; *p; p++) {
        if (*p == '-') continue;
        const char *q = *p ? strchr(AL, *p) : NULL;
        if (!q) return 0;
        v = v << 5 | (uint64_t)(q - AL);
        k++;
    }
    return k == 9 ? v : 0;
}
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static bool parse_entry(const Json *e, Entry *o) {
    if (!e || json_bool(json_get(e, "deleted"), false)) return false;
    const char *id = json_str(json_get(e, "id"), "");
    if (strlen(id) != 11) return false;
    memset(o, 0, sizeof *o);
    memcpy(o->id, id, 11);
    o->style = index_of(STYLES, 5, json_str(json_get(e, "style"), ""));
    o->theme = index_of(THEMES, 10, json_str(json_get(e, "theme"), ""));
    o->tag1 = (uint8_t)json_num(json_get(e, "tag1"), 0);
    o->tag2 = (uint8_t)json_num(json_get(e, "tag2"), 0);
    o->has_cc = json_num(json_get(e, "clear_condition"), 0) != 0;
    o->clears = (uint32_t)json_num(json_get(e, "clears"), 0);
    o->tries = (uint32_t)json_num(json_get(e, "tries"), 0);
    o->ms = (uint32_t)json_num(json_get(e, "upload_time"), 0);
    return true;
}

static bool match(const Entry *e, const SearchFilter *f) {  // (under S.lock)
    if (e->dead) return false;
    if (f->uncleared && e->clears) return false;
    if (f->style >= 0 && e->style != f->style) return false;
    if (f->theme >= 0 && e->theme != f->theme) return false;
    if (f->tag > 0 && e->tag1 != f->tag && e->tag2 != f->tag) return false;
    if (f->condition == 1 && e->has_cc) return false;
    if (f->condition == 2 && !e->has_cc) return false;
    if (f->min_tries && e->tries < f->min_tries) return false;
    if (f->max_tries && e->tries > f->max_tries) return false;
    if (f->min_ms && e->ms < f->min_ms) return false;
    if (f->max_ms && e->ms > f->max_ms) return false;
    if (f->unplayed && S.np) {
        uint64_t k = pack_id(e->id);
        if (k && bsearch(&k, S.played, S.np, sizeof k, cmp_u64)) return false;
    }
    return true;
}

// "2025-12-19T22:19:44.582777Z" -> unix time (UTC); 0 when it is not that
static int64_t parse_time(const char *t) {
    int y, mo, d, h, mi, sec;
    if (sscanf(t, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6 || mo < 1 || mo > 12) return 0;
    y -= mo <= 2;  // days from civil (Howard Hinnant's)
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400, doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400 + h * 3600 + mi * 60 + sec;
}

static bool rest_get(const char *url, HttpResponse *r) {
    int st = http_request("GET", url, "Accept: application/json\r\n", NULL, 0, SEARCH_TIMEOUT_MS, r);
    if (st != 200) {
        http_free(r);
        return false;
    }
    return true;
}

// ---- the index of IDs, and the cache file
static void ht_put(uint64_t key, uint32_t idx) {  // (under S.lock; the table is kept at most half full)
    size_t m = S.ht_cap - 1, h = (size_t)(key * 0x9e3779b97f4a7c15ull >> 20) & m;
    while (S.ht[h].key) h = (h + 1) & m;
    S.ht[h].key = key, S.ht[h].idx = idx;
}
static void ht_rebuild(void) {  // (under S.lock)
    size_t cap = 1024;
    while (cap < (S.n + 1) * 2 + 4096) cap *= 2;
    free(S.ht);
    S.ht = calloc(cap, sizeof *S.ht);
    S.ht_cap = S.ht ? cap : 0;
    for (size_t i = 0; S.ht && i < S.n; i++) ht_put(pack_id(S.a[i].id), (uint32_t)i);
}
static Entry *ht_find(uint64_t key) {  // (under S.lock)
    if (!S.ht_cap) return NULL;
    size_t m = S.ht_cap - 1, h = (size_t)(key * 0x9e3779b97f4a7c15ull >> 20) & m;
    while (S.ht[h].key) {
        if (S.ht[h].key == key) return &S.a[S.ht[h].idx];
        h = (h + 1) & m;
    }
    return NULL;
}
// a course read from the service: a new one is added, one already known is brought up to date. True when it was new.
static bool merge_entry(const Entry *e) {  // (under S.lock)
    uint64_t key = pack_id(e->id);
    if (!key) return false;
    Entry *old = ht_find(key);
    if (old) {
        *old = *e;
        old->mark = S.epoch;
        return false;
    }
    if (S.n == S.cap) {
        size_t nc = S.cap ? S.cap * 2 : 4096;
        Entry *na = realloc(S.a, nc * sizeof *na);
        if (!na) return false;
        S.a = na, S.cap = nc;
    }
    if ((S.n + 1) * 2 > S.ht_cap) ht_rebuild();
    if (!S.ht_cap) return false;
    S.a[S.n] = *e;
    S.a[S.n].mark = S.epoch;
    ht_put(key, (uint32_t)S.n);
    S.n++;
    return true;
}

typedef struct { uint32_t magic; uint32_t n, np; uint64_t saved; char domain[64], maker[12]; } CacheHead;

static void cache_save(void) {  // (under S.lock)
    char domain[64], maker[12];
    if (!S.n || (S.fetching && !S.incremental) || !ocw_rest_target(domain, sizeof domain, maker)) return;  // (not a list half read)
    FILE *f = fopen(CACHE_FILE ".tmp", "wb");
    if (!f) return;
    CacheHead h;
    memset(&h, 0, sizeof h);
    h.magic = CACHE_MAGIC, h.n = (uint32_t)S.n, h.np = S.played_known ? (uint32_t)S.np : 0, h.saved = (uint64_t)S.saved;
    snprintf(h.domain, sizeof h.domain, "%s", domain);
    snprintf(h.maker, sizeof h.maker, "%s", maker);
    bool ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(S.a, sizeof *S.a, S.n, f) == S.n &&
              (!h.np || fwrite(S.played, sizeof *S.played, S.np, f) == S.np);
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(CACHE_FILE);
        ok = rename(CACHE_FILE ".tmp", CACHE_FILE) == 0;
    }
    if (!ok) remove(CACHE_FILE ".tmp");
}

// the file's lists, once, when they are of this service (and for the played list, this player)
static void cache_load(void) {  // (under S.lock)
    char domain[64], maker[12];
    if (!ocw_rest_target(domain, sizeof domain, maker)) return;
    FILE *f = fopen(CACHE_FILE, "rb");
    if (!f) return;
    CacheHead h;
    if (fread(&h, sizeof h, 1, f) == 1 && h.magic == CACHE_MAGIC && !strcmp(h.domain, domain) && h.n && h.n < 1000000 && h.np < 10000000) {
        Entry *a = malloc(h.n * sizeof *a);
        uint64_t *pl = h.np && maker[0] && !strcmp(h.maker, maker) ? malloc(h.np * sizeof *pl) : NULL;
        if (a && fread(a, sizeof *a, h.n, f) == h.n) {
            S.a = a, S.n = S.cap = h.n, S.saved = (time_t)h.saved;
            for (size_t i = 0; i < S.n; i++) S.a[i].mark = 0;
            if (pl && fread(pl, sizeof *pl, h.np, f) == h.np) S.played = pl, S.np = h.np, S.played_known = true, pl = NULL;
            a = NULL;
            ht_rebuild();
            LOG("[search] %zu courses and %zu played courses from " CACHE_FILE "\n", S.n, S.np);
        }
        free(a), free(pl);
    }
    fclose(f);
}

// ---- the course list
static void *page_worker(void *arg) {
    unsigned gen = (unsigned)(uintptr_t)arg;
    char domain[64], maker[12];
    if (!ocw_rest_target(domain, sizeof domain, maker)) goto done;
    for (;;) {
        int page = atomic_fetch_add(&S.next_page, 1);
        if (page >= atomic_load(&S.end_page) || atomic_load(&S.gen) != gen) break;
        char url[256];
        snprintf(url, sizeof url, "https://%s/api/latest/courses?offset=%d&size=%d", domain, page * SEARCH_PAGE, SEARCH_PAGE);
        HttpResponse r;
        Json *j = NULL;
        for (int tries = 0; tries < 3 && !j; tries++) {
            if (!rest_get(url, &r)) continue;
            j = json_parse(r.body, r.len);
            http_free(&r);
        }
        if (!j || j->type != JSON_ARR) {
            json_free(j);
            if (atomic_fetch_add(&S.fails, 1) >= 5) {  // (the service is not answering: stop)
                atomic_store(&S.end_page, 0);
                pthread_mutex_lock(&S.lock);
                if (atomic_load(&S.gen) == gen) snprintf(S.err, sizeof S.err, "OpenCourse World did not answer the course list");
                pthread_mutex_unlock(&S.lock);
            }
            continue;
        }
        size_t n = json_len(j);
        Entry *tmp = n ? malloc(n * sizeof *tmp) : NULL;
        size_t m = 0;
        for (size_t i = 0; i < n && tmp; i++)
            if (parse_entry(json_at(j, i), &tmp[m])) m++;
        json_free(j);
        size_t fresh = 0;
        pthread_mutex_lock(&S.lock);
        if (atomic_load(&S.gen) == gen)
            for (size_t i = 0; i < m; i++) fresh += merge_entry(&tmp[i]);
        bool caught_up = S.incremental && !fresh;
        pthread_mutex_unlock(&S.lock);
        free(tmp);
        int end = n < SEARCH_PAGE || caught_up ? page + 1 : 0;  // (the last page; or a top-up has caught up)
        for (int old = atomic_load(&S.end_page); end && end < old && !atomic_compare_exchange_weak(&S.end_page, &old, end);) {}
    }
done:
    if (atomic_fetch_sub(&S.workers, 1) == 1) {
        pthread_mutex_lock(&S.lock);
        if (atomic_load(&S.gen) == gen) {
            if (!S.err[0] && !S.incremental) {  // (a full read: what it did not see is gone from the service)
                size_t w = 0;
                for (size_t i = 0; i < S.n; i++)
                    if (S.a[i].mark == S.epoch) S.a[w++] = S.a[i];
                if (w != S.n) S.n = w, ht_rebuild();
            }
            S.fetching = false;
            if (!S.err[0]) S.saved = time(NULL);
            S.retry = time(NULL) + (S.err[0] ? 30 : 600);
            LOG("[search] %zu courses (%s)%s%s\n", S.n, S.incremental ? "topped up" : "read in full", S.err[0] ? ": " : "", S.err);
            if (!S.err[0]) cache_save();
        }
        pthread_mutex_unlock(&S.lock);
    }
    return NULL;
}

static void start_list(bool full) {  // (under S.lock)
    char domain[64], maker[12];
    if (S.fetching || !ocw_rest_target(domain, sizeof domain, maker)) return;
    unsigned gen = atomic_fetch_add(&S.gen, 1) + 1;
    S.incremental = !full && S.n > 0;
    if (!S.incremental) S.epoch++;
    S.err[0] = 0, S.fetching = true, S.started = true;
    int workers = S.incremental ? 1 : SEARCH_WORKERS;  // (a top-up is a page or two, one after the other)
    atomic_store(&S.next_page, 0), atomic_store(&S.end_page, SEARCH_MAX_PAGES), atomic_store(&S.fails, 0);
    atomic_store(&S.workers, workers);
    for (int i = 0; i < workers; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, page_worker, (void *)(uintptr_t)gen)) {
            if (atomic_fetch_sub(&S.workers, 1) == 1) S.fetching = false;
            continue;
        }
        pthread_detach(t);
    }
}

// ---- the player's own played list
static void *played_worker(void *arg) {
    char maker[12];
    snprintf(maker, sizeof maker, "%s", (const char *)arg);
    free(arg);
    char domain[64], own[12];
    bool ok = ocw_rest_target(domain, sizeof domain, own) && own[0];
    pthread_mutex_lock(&S.lock);
    bool incr = S.played_known && !S.played_full;  // (topping up: what is known is kept, and the walk stops at a page of nothing new)
    S.played_full = false;
    size_t n = incr ? S.np : 0, cap = n;
    uint64_t *ids = n ? malloc(n * sizeof *ids) : NULL;
    if (n && ids) memcpy(ids, S.played, n * sizeof *ids);
    else n = cap = 0;
    pthread_mutex_unlock(&S.lock);
    size_t known = n;
    for (int page = 0; ok && page < PLAYED_MAX_PAGES; page++) {
        char url[256];
        snprintf(url, sizeof url, "https://%s/api/makers/%s/played?offset=%d&size=%d", domain, maker, page * SEARCH_PAGE, SEARCH_PAGE);
        HttpResponse r;
        Json *j = NULL;
        for (int tries = 0; tries < 3 && !j; tries++) {
            if (!rest_get(url, &r)) continue;
            j = json_parse(r.body, r.len);
            http_free(&r);
        }
        if (!j || j->type != JSON_ARR) {
            json_free(j);
            ok = false;
            break;
        }
        size_t m = json_len(j), fresh = 0;
        for (size_t i = 0; i < m; i++) {
            uint64_t k = pack_id(json_str(json_get(json_at(j, i), "course_id"), ""));
            if (!k) continue;
            if (known && bsearch(&k, ids, known, sizeof k, cmp_u64)) continue;  // (known: ids[0..known) is sorted)
            if (n == cap) {
                uint64_t *na = realloc(ids, (cap ? cap * 2 : 1024) * sizeof *na);
                if (!na) { ok = false; break; }
                ids = na, cap = cap ? cap * 2 : 1024;
            }
            ids[n++] = k, fresh++;
        }
        json_free(j);
        if (m < SEARCH_PAGE || (incr && !fresh)) break;
    }
    if (ok) qsort(ids, n, sizeof *ids, cmp_u64);
    pthread_mutex_lock(&S.lock);
    S.played_loading = false;
    if (ok) {
        free(S.played);
        S.played = ids, S.np = n, S.played_known = true, ids = NULL;
        S.played_retry = time(NULL) + 1800;
        LOG("[search] %zu played courses (%s)\n", n, incr ? "topped up" : "read in full");
        cache_save();
    } else {
        S.played_retry = time(NULL) + 30;
        LOG("[search] the played list could not be read\n");
    }
    pthread_mutex_unlock(&S.lock);
    free(ids);
    return NULL;
}

static void start_played(void) {  // (under S.lock)
    char domain[64], maker[12];
    if (S.played_loading || !ocw_rest_target(domain, sizeof domain, maker) || !maker[0]) return;
    char *arg = malloc(12);
    if (!arg) return;
    memcpy(arg, maker, 12);
    pthread_t t;
    S.played_loading = true, S.played_started = true;
    if (pthread_create(&t, NULL, played_worker, arg)) {
        S.played_loading = false;
        free(arg);
        return;
    }
    pthread_detach(t);
}

// The lists come from the file at the first call; the course list is read in full when there is none or it is old, else topped up,
// and topped up again every ten minutes; the played list every half hour.
static void ensure(void) {  // (under S.lock)
    time_t now = time(NULL);
    if (!S.started && !S.fetching) {
        cache_load();
        start_list(S.n == 0 || now - S.saved > CACHE_FULL_AFTER);
    } else if (!S.fetching && now >= S.retry) {
        start_list(S.n == 0);
    }
    if (!S.played_loading && (!S.played_started || now >= S.played_retry)) start_played();
}

// Started with the game (ocw.c, where the boards are started): the lists are read now, not when the tab is first opened. The played list
// waits for the player's maker code, which the game learns from its first profile.
static void *boot_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < 120; i++) {
        pthread_mutex_lock(&S.lock);
        ensure();
        bool done = S.played_started;
        pthread_mutex_unlock(&S.lock);
        if (done) break;
        sleep(1);
    }
    return NULL;
}

void search_boot(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, boot_thread, NULL) == 0) pthread_detach(t);
}

void game_search_status(SearchStatus *out) {
    memset(out, 0, sizeof *out);
    char domain[64], maker[12];
    out->online = ocw_rest_target(domain, sizeof domain, maker);
    if (!out->online) return;
    pthread_mutex_lock(&S.lock);
    ensure();
    out->loading = S.fetching && !S.incremental;
    out->played_loading = S.played_loading, out->played_known = S.played_known;
    out->loaded = (unsigned)S.n;
    snprintf(out->error, sizeof out->error, "%s", S.err);
    pthread_mutex_unlock(&S.lock);
}

unsigned game_search_count(const SearchFilter *f) {
    unsigned c = 0;
    pthread_mutex_lock(&S.lock);
    for (size_t i = 0; i < S.n; i++) c += match(&S.a[i], f);
    pthread_mutex_unlock(&S.lock);
    return c;
}

void game_search_refresh(void) {
    pthread_mutex_lock(&S.lock);
    if (!S.fetching) start_list(true);
    if (!S.played_loading) S.played_full = true, start_played();
    pthread_mutex_unlock(&S.lock);
}

// ---- a pick
static uint64_t rng(void) {
    static uint64_t s;
    if (!s) s = (uint64_t)time(NULL) * 0x9e3779b97f4a7c15ull ^ (uint64_t)(uintptr_t)&s;
    s ^= s << 13, s ^= s >> 7, s ^= s << 17;
    return s;
}

static void clean_text(char *s) {  // (the menu's font draws what the game's names get cleaned of no better)
    extern int username_clean(uint8_t *s, size_t len);  // runtime/services/ssl.c
    if (g_settings.clean_usernames) username_clean((uint8_t *)s, strlen(s));
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
}

static void *roll_thread(void *arg) {
    SearchFilter f = *(SearchFilter *)arg;
    free(arg);
    char domain[64], maker[12];
    SearchCourse out;
    memset(&out, 0, sizeof out);
    int state = 3;
    char err[96] = "";
    if (!ocw_rest_target(domain, sizeof domain, maker)) {
        state = 4, snprintf(err, sizeof err, "No online service is set up");
        goto finish;
    }
    pthread_mutex_lock(&S.lock);
    bool need_played = f.unplayed && !S.played_known;
    pthread_mutex_unlock(&S.lock);
    if (need_played) {
        state = 4, snprintf(err, sizeof err, "Your played list is not loaded yet, so \"unplayed\" cannot be told");
        goto finish;
    }
    for (int attempt = 0; attempt < 12; attempt++) {
        Entry pick = {{0}};
        size_t idx = 0;
        unsigned gen, seen = 0;
        pthread_mutex_lock(&S.lock);
        gen = atomic_load(&S.gen);
        for (size_t i = 0; i < S.n; i++) {  // (one pass: each match is kept with a chance of one in the number met so far)
            if (!match(&S.a[i], &f)) continue;
            if (rng() % ++seen == 0) pick = S.a[i], idx = i;
        }
        pthread_mutex_unlock(&S.lock);
        if (!seen) {
            state = 3;
            goto finish;
        }
        char url[256];
        snprintf(url, sizeof url, "https://%s/api/courses/%s", domain, pick.id);
        HttpResponse r;
        int st = http_request("GET", url, "Accept: application/json\r\n", NULL, 0, SEARCH_TIMEOUT_MS, &r);
        Json *j = st == 200 ? json_parse(r.body, r.len) : NULL;
        bool gone = st == 404 || st == 500 || (j && json_bool(json_get(j, "deleted"), false));  // (a course that is not there: a 500 as well)
        http_free(&r);
        memset(&out, 0, sizeof out);
        memcpy(out.id, pick.id, 12);
        out.style = pick.style, out.theme = pick.theme, out.tag1 = pick.tag1, out.tag2 = pick.tag2;
        out.clears = pick.clears, out.tries = pick.tries, out.clear_check_ms = pick.ms;
        if (j && !gone) {
            out.verified = true;
            out.created = parse_time(json_str(json_get(j, "created"), ""));
            out.clears = (uint32_t)json_num(json_get(j, "clears"), 0);
            out.plays = (uint32_t)json_num(json_get(j, "plays"), 0);
            out.tries = (uint32_t)json_num(json_get(j, "tries"), 0);
            snprintf(out.title, sizeof out.title, "%s", json_str(json_get(j, "title"), ""));
            snprintf(out.owner, sizeof out.owner, "%s", json_str(json_get(json_get(j, "owner"), "username"), ""));
            clean_text(out.title), clean_text(out.owner);
        }
        json_free(j);
        if (gone || (out.verified && f.uncleared && out.clears)) {  // (gone, or cleared since the list was read: draw another)
            pthread_mutex_lock(&S.lock);
            if (atomic_load(&S.gen) == gen && idx < S.n && !strcmp(S.a[idx].id, pick.id)) {
                if (gone) S.a[idx].dead = 1;
                else S.a[idx].clears = out.clears;
            }
            pthread_mutex_unlock(&S.lock);
            continue;
        }
        state = 2;
        goto finish;
    }
    state = 4, snprintf(err, sizeof err, "Every pick had been cleared or removed since the list was read: refresh the list");
finish:
    pthread_mutex_lock(&S.lock);
    S.roll_state = state, S.rolling = false;
    if (state == 2) S.roll_out = out;
    snprintf(S.roll_err, sizeof S.roll_err, "%s", err);
    pthread_mutex_unlock(&S.lock);
    return NULL;
}

void game_search_roll(const SearchFilter *f) {
    pthread_mutex_lock(&S.lock);
    if (S.rolling) {
        pthread_mutex_unlock(&S.lock);
        return;
    }
    SearchFilter *arg = malloc(sizeof *arg);
    pthread_t t;
    if (arg) {
        *arg = *f;
        S.rolling = true, S.roll_state = 1;
        if (pthread_create(&t, NULL, roll_thread, arg)) S.rolling = false, S.roll_state = 4, snprintf(S.roll_err, sizeof S.roll_err, "Could not start"), free(arg);
        else pthread_detach(t);
    }
    pthread_mutex_unlock(&S.lock);
}

int game_search_result(SearchCourse *out, char err[96]) {
    pthread_mutex_lock(&S.lock);
    int st = S.roll_state;
    if (st == 2) *out = S.roll_out;
    snprintf(err, 96, "%s", S.roll_err);
    pthread_mutex_unlock(&S.lock);
    return st;
}

// ---- the picture of a course: GET https://<domain>/api/one_screen_thumbnail/<course id> (a 640 x 360 JPEG), the last eight kept
#define IMG_RING 8
static struct { char id[12]; uint8_t *px; int w, h; int st; } IMG[IMG_RING];  // st: 0 free, 1 being fetched, 2 there, 3 failed (under S.lock)
static int img_next;

static void *img_thread(void *arg) {
    char *id = arg;
    char domain[64], maker[12], url[256];
    HttpResponse r;
    uint8_t *px = NULL;
    int w = 0, h = 0, n, st = 0;
    if (ocw_rest_target(domain, sizeof domain, maker)) {
        snprintf(url, sizeof url, "https://%s/api/one_screen_thumbnail/%s", domain, id);
        st = http_request("GET", url, NULL, NULL, 0, SEARCH_TIMEOUT_MS, &r);
        if (st == 200 && r.body) px = stbi_load_from_memory((const unsigned char *)r.body, (int)r.len, &w, &h, &n, 4);
        http_free(&r);
    }
    pthread_mutex_lock(&S.lock);
    for (int i = 0; i < IMG_RING; i++)
        if (IMG[i].st == 1 && !strcmp(IMG[i].id, id)) {
            IMG[i].px = px, IMG[i].w = w, IMG[i].h = h, IMG[i].st = px ? 2 : 3;
            px = NULL;
            break;
        }
    pthread_mutex_unlock(&S.lock);
    stbi_image_free(px);
    free(id);
    return NULL;
}

const uint8_t *game_search_image(const char *id, int *w, int *h) {
    const uint8_t *out = NULL;
    *w = *h = 0;
    pthread_mutex_lock(&S.lock);
    int i = 0;
    for (; i < IMG_RING; i++)
        if (IMG[i].st && !strcmp(IMG[i].id, id)) break;
    if (i == IMG_RING) {  // not asked for yet: the oldest place is given up (one still being fetched is skipped)
        for (int k = 0; k < IMG_RING; k++) {
            int j = (img_next + k) % IMG_RING;
            if (IMG[j].st == 1) continue;
            i = j, img_next = (j + 1) % IMG_RING;
            break;
        }
        if (i < IMG_RING) {
            stbi_image_free(IMG[i].px);
            memset(&IMG[i], 0, sizeof IMG[i]);
            snprintf(IMG[i].id, sizeof IMG[i].id, "%s", id);
            IMG[i].st = 1;
            char *arg = strdup(id);
            pthread_t t;
            if (!arg || pthread_create(&t, NULL, img_thread, arg)) IMG[i].st = 3, free(arg);
            else pthread_detach(t);
        }
    }
    if (i < IMG_RING) {
        if (IMG[i].st == 2) out = IMG[i].px, *w = IMG[i].w, *h = IMG[i].h;
        else if (IMG[i].st == 3) *w = -1;
    }
    pthread_mutex_unlock(&S.lock);
    return out;
}

// ---- to the game
void game_search_go(const char *id) {
    pthread_mutex_lock(&S.lock);
    snprintf(S.pending, sizeof S.pending, "%s", id ? id : "");
    S.pending_at = time(NULL);
    pthread_mutex_unlock(&S.lock);
}

// ---- taking a course to the game: Course World's own screens, driven as a player would (the same way Skip Mode Select drives the
// title screen: presses through the game's test-button injection, a touch through the touch screen's, skip_intro.c).
//   1. B a few times: whatever Course World page is open closes back to the hub (B on the hub does nothing)
//   2. a touch on the hub's ID button (a touch needs holding for a second, a quick tap is not taken); the game's Course ID prompt is
//      then answered with the course's ID (runtime/hle/swkbd.c, game_search_pending), and the game finds the course
//   and it stops there, on the course's page (Play is the player's to press)
#define SCENE_MGR MAIN_ADDR(0x2a390d8)  // *: the scene manager; +0x33C target scene (u32), +0x344 change under way (skip_intro.c)
#define SCENE_COURSE_WORLD 1            // (target scene while Course World is open: pages and all)
extern uint64_t renderer_present_count(void);
extern uint64_t g_test_buttons, g_test_buttons_until;  // runtime/game_hooks.c, read by runtime/hle/input.c
extern void touch_test(int32_t x, int32_t y, bool down);  // runtime/hle/touch.c: a finger at game pixels (1280 x 720)
static _Atomic bool g_launching;
static _Atomic float g_launch_progress;
extern _Atomic int g_present_hidden;  // runtime/nvn/objects.c: frames run unpaced and unseen (the loading screen shows instead)
extern volatile int g_audio_muted;    // runtime/services/audio.c

// the black loading screen Skip Mode Select shows (skip_intro.c game_splash asks): the frames run fast and unseen while Course World is driven
bool course_launch_splash(float *progress) {
    if (!atomic_load(&g_launching)) return false;
    *progress = atomic_load(&g_launch_progress);
    return true;
}

bool course_launch_busy(void) { return atomic_load(&g_launching); }  // (the player's own input is dropped meanwhile: skip_intro.c)

static void wait_frames(unsigned n) {  // presented frames (the game may run unpaced: a press is held for frames, not time)
    uint64_t until = renderer_present_count() + n;
    while (renderer_present_count() < until) {
        atomic_store(&g_present_hidden, 300);
        usleep(10000);
    }
}
static void touch_hold(int x, int y) {  // (a touch is taken only when held about a second of the game's time)
    touch_test(x, y, true);
    wait_frames(60);
    touch_test(x, y, false);
}

static void *launch_thread(void *arg) {
    char id[12];
    snprintf(id, sizeof id, "%s", (char *)arg);
    free(arg);
    LOG("[search] taking %s to the game\n", id);
    atomic_store(&g_launch_progress, 0.05f);
    atomic_store(&g_present_hidden, 300), g_audio_muted = 1;
    for (int i = 0; i < 3; i++) {  // (1)
        atomic_store(&g_launch_progress, 0.05f + 0.1f * i);
        g_test_buttons = 1u << 1, g_test_buttons_until = renderer_present_count() + 6;  // B
        wait_frames(30);
    }
    atomic_store(&g_launch_progress, 0.4f);
    touch_hold(1207, 187);  // (2) the ID button
    atomic_store(&g_launch_progress, 0.65f);
    for (int i = 0; i < 100; i++) {  // (the game's Course ID prompt takes the ID: the course's page follows by itself)
        usleep(100000);
        pthread_mutex_lock(&S.lock);
        bool asked = S.pending[0] == 0;
        pthread_mutex_unlock(&S.lock);
        if (asked) {
            atomic_store(&g_launch_progress, 0.85f);
            wait_frames(90);  // (the course's page comes up unseen, then the window shows it)
            goto done;
        }
    }
    LOG("[search] the Course ID prompt did not open\n");
    pthread_mutex_lock(&S.lock);
    S.pending[0] = 0;
    pthread_mutex_unlock(&S.lock);
done:
    atomic_store(&g_present_hidden, 0), g_audio_muted = 0;
    atomic_store(&g_launching, false);
    return NULL;
}

// Take a course to the game (its page in Course World). False (and msg says why) when Course World is not what the game is showing, or it is already going.
bool game_search_launch(const char *id, char msg[96]) {
    uint64_t sm = g64(SCENE_MGR);
    if (!sm || g32(sm + 0x33c) != SCENE_COURSE_WORLD) {
        snprintf(msg, 96, "Open Course World in the game first");
        return false;
    }
    if (atomic_exchange(&g_launching, true)) {
        snprintf(msg, 96, "Already on its way");
        return false;
    }
    game_search_go(id);
    char *arg = strdup(id);
    pthread_t t;
    if (!arg || pthread_create(&t, NULL, launch_thread, arg)) {
        free(arg);
        atomic_store(&g_launching, false);
        snprintf(msg, 96, "Could not start");
        return false;
    }
    pthread_detach(t);
    msg[0] = 0;
    return true;
}

bool game_search_pending(char out[12]) {
    pthread_mutex_lock(&S.lock);
    bool have = S.pending[0] != 0 && time(NULL) - S.pending_at < 900;  // (not one left from long ago)
    if (have) snprintf(out, 12, "%s", S.pending);
    S.pending[0] = 0;
    pthread_mutex_unlock(&S.lock);
    return have;
}
