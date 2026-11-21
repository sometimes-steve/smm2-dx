// Early course downloads from OpenCourse World (ocwproxy.h).
#include "ocwproxy.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hostsock.h"

#ifdef OCWPROXY_TEST
#define LOG(...) (fprintf(stderr, __VA_ARGS__), fflush(stderr))
#else
#include "../rt.h"
#include "netcfg.h"
#endif

#define MAX_ENTRIES 4
#define MAX_RESP (4u << 20)       // a response bigger than this is not kept (courses are 377,073 bytes)
#define STALL_MS 15000            // no byte from the server for this long: the early download is given up (the game then asks for itself)
#define KEEP_MS 90000             // an early download nobody asked for in this long is dropped
#define CONNECT_MS 8000
#define IDLE_MS 120000            // a game connection that sends nothing for this long is closed
#define MAX_CLIENTS 16

enum { E_FREE, E_FETCHING, E_DONE, E_FAILED };

typedef struct {
    uint32_t id;
    int state;
    uint8_t *buf;                 // the whole response as the server sent it: head, then the body
    size_t len, cap, head, total; // bytes held, capacity, length of the head, the full response's length (0 until it is known)
    bool chunked;                 // the body is in chunks (Transfer-Encoding: chunked): its end is found by following them, cpos is where the next one starts
    size_t cpos;
    uint64_t t0, t_done;          // ms
    int refs;                     // game connections being served from it
} Entry;

static Entry g_e[MAX_ENTRIES];
// The game's own request for a course, after the id (" HTTP/1.1\r\nHost: ...\r\n\r\n"): the early downloads send the same headers, because the
// server answers a request that looks different in a different way (chunked, which is not followed). Learned from the first request the
// game makes; until then a plain one is sent.
static char g_tmpl[1024];
static size_t g_tmpl_len;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static struct {
    bool ready, tried;
    uint32_t up_ip;
    uint16_t up_port, listen_port;
    char host[256];
    int clients;
} g;

static uint64_t now_ms(void) { return GetTickCount64(); }

// ---- sockets with timeouts (host sockets, non-blocking underneath)
static int wait_for(hsock_t s, short ev, int ms) {
    hpollfd f = {0};
    f.fd = s, f.events = ev;
    int r = hsock_poll(&f, 1, ms);
    return r > 0 ? 1 : r == 0 ? 0 : -1;
}

static bool send_all(hsock_t s, const uint8_t *p, size_t n) {
    while (n) {
        int w = send(s, (const char *)p, HSOCK_IOLEN(n > 65536 ? 65536 : n), MSG_NOSIGNAL);
        if (w > 0) { p += w, n -= (size_t)w; continue; }
        if (w < 0 && hsock_would_block(hsock_errno())) {
            if (wait_for(s, POLLOUT, IDLE_MS) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

// bytes (> 0), 0 the far end closed, -1 failed, -2 nothing arrived in ms
static int recv_some(hsock_t s, uint8_t *b, size_t n, int ms) {
    for (;;) {
        int r = recv(s, (char *)b, HSOCK_IOLEN(n), 0);
        if (r >= 0) return r;
        if (!hsock_would_block(hsock_errno())) return -1;
        int w = wait_for(s, POLLIN, ms);
        if (w == 0) return -2;
        if (w < 0) return -1;
    }
}

static hsock_t up_connect(void) {
    hsock_startup();
    hsock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == HSOCK_BAD) return HSOCK_BAD;
    hsock_nonblock(s);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(g.up_ip);
    a.sin_port = htons(g.up_port);
    int r = connect(s, (struct sockaddr *)&a, sizeof a);
    if (r != 0 && !hsock_would_block(hsock_errno())) { hsock_close(s); return HSOCK_BAD; }
    if (r != 0) {
        if (wait_for(s, POLLOUT, CONNECT_MS) <= 0) { hsock_close(s); return HSOCK_BAD; }
        int e = 0;
        hsocklen_t n = sizeof e;
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&e, &n) != 0 || e) { hsock_close(s); return HSOCK_BAD; }
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return s;
}

// ---- the early downloads
static void entry_reset(Entry *e) {
    free(e->buf);
    memset(e, 0, sizeof *e);
}

// "Content-Length: 123" in a response head (n bytes, ending at the blank line); -1 none; -2 chunked (OpenCourse World's server, behind Caddy,
// answers every course so); -3 some other Transfer-Encoding (not followed).
static long head_length(const char *h, size_t n, int *status) {
    *status = 0;
    if (n < 12 || memcmp(h, "HTTP/1.", 7)) return -1;
    *status = atoi(h + 9);
    long cl = -1;
    for (size_t i = 0; i + 16 < n; i++) {
        if (h[i] != '\n') continue;
        if (!_strnicmp(h + i + 1, "content-length:", 15)) cl = atol(h + i + 16);
        else if (!_strnicmp(h + i + 1, "transfer-encoding:", 18)) {
            const char *v = h + i + 19;
            while (v < h + n && (*v == ' ' || *v == '\r' )) v++;
            return !_strnicmp(v, "chunked", 7) ? -2 : -3;
        }
    }
    return cl;
}

// Follows the chunks of a body that starts at e->head: where the whole response ends (the index just after the last chunk's blank line),
// or 0 when not all of it is here yet, or -1 when the framing is wrong. e->cpos moves on as chunks are seen whole. (the lock held)
static long chunks_end(Entry *e) {
    const uint8_t *b = e->buf;
    size_t n = e->len, pos = e->cpos;
    for (;;) {
        size_t i = pos;
        while (i + 1 < n && !(b[i] == '\r' && b[i + 1] == '\n')) i++;
        if (i + 1 >= n) return 0;                      // (the size line is not all here)
        if (i == pos || i - pos > 16) return -1;       // (empty or too long to be a size)
        size_t size = 0;
        for (size_t k = pos; k < i; k++) {
            int d = b[k] >= '0' && b[k] <= '9' ? b[k] - '0' : b[k] >= 'a' && b[k] <= 'f' ? b[k] - 'a' + 10 : b[k] >= 'A' && b[k] <= 'F' ? b[k] - 'A' + 10 : -1;
            if (d < 0) return -1;                      // (a chunk extension is not expected: ";" and the like are not followed)
            size = size * 16 + (size_t)d;
            if (size > MAX_RESP) return -1;
        }
        size_t data = i + 2;
        if (size == 0) {                               // the last chunk: optional trailer lines, then a blank line
            for (size_t t = data; t + 1 < n; t++) {
                if (b[t] == '\r' && b[t + 1] == '\n') {
                    if (t == data) return (long)(t + 2);   // (no trailers: this is the blank line)
                    data = t + 2;                          // (a trailer line, the next one starts here)
                    t++;
                }
            }
            return 0;
        }
        if (data + size + 2 > n) return 0;             // (the chunk is not all here)
        if (b[data + size] != '\r' || b[data + size + 1] != '\n') return -1;
        pos = data + size + 2;
        e->cpos = pos;
    }
}

// text for the log: line ends as "|", anything unprintable as "."
static void flat(char *out, size_t cap, const uint8_t *b, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 2 < cap; i++) {
        if (b[i] == '\r') continue;
        out[o++] = b[i] == '\n' ? '|' : b[i] >= 0x20 && b[i] < 0x7f ? (char)b[i] : '.';
    }
    out[o] = 0;
}

static void fetch_fail(Entry *e, const char *why) {
    pthread_mutex_lock(&g_mu);
    char head[260];
    flat(head, sizeof head, e->buf, e->len < 220 ? e->len : 220);
    LOG("[ocw-http] early download of %u failed: %s (%zu bytes in, %llu ms): the game asks for itself\n    the answer began: %s\n", e->id, why, e->len,
        (unsigned long long)(now_ms() - e->t0), head);
    e->state = E_FAILED;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static void *fetch_main(void *arg) {
    Entry *e = arg;
    hsock_t s = up_connect();
    if (s == HSOCK_BAD) { fetch_fail(e, "could not connect"); return NULL; }
    char req[1200];
    int rn;
    pthread_mutex_lock(&g_mu);
    if (g_tmpl_len) rn = snprintf(req, sizeof req, "GET /level_data/%u%.*s", e->id, (int)g_tmpl_len, g_tmpl);
    else rn = snprintf(req, sizeof req, "GET /level_data/%u HTTP/1.1\r\nHost: %s\r\nAccept: */*\r\n\r\n", e->id, g.host);
    pthread_mutex_unlock(&g_mu);
    if (!send_all(s, (const uint8_t *)req, (size_t)rn)) { hsock_close(s); fetch_fail(e, "could not send"); return NULL; }
    uint8_t tmp[16384];
    for (;;) {
        int r = recv_some(s, tmp, sizeof tmp, STALL_MS);
        if (r <= 0) {
            hsock_close(s);
            fetch_fail(e, r == -2 ? "stalled" : "the connection ended early");
            return NULL;
        }
        pthread_mutex_lock(&g_mu);
        if (e->len + (size_t)r > MAX_RESP) { pthread_mutex_unlock(&g_mu); hsock_close(s); fetch_fail(e, "too big"); return NULL; }
        if (e->len + (size_t)r > e->cap) {
            size_t nc = e->cap ? e->cap * 2 : 65536;
            while (nc < e->len + (size_t)r) nc *= 2;
            uint8_t *nb = realloc(e->buf, nc);
            if (!nb) { pthread_mutex_unlock(&g_mu); hsock_close(s); fetch_fail(e, "out of memory"); return NULL; }
            e->buf = nb, e->cap = nc;
        }
        memcpy(e->buf + e->len, tmp, (size_t)r);
        e->len += (size_t)r;
        if (!e->total) {
            const char *h = (const char *)e->buf;
            size_t hl = 0;
            for (size_t i = 0; i + 4 <= e->len; i++)
                if (!memcmp(h + i, "\r\n\r\n", 4)) { hl = i + 4; break; }
            if (hl) {
                int st;
                long cl = head_length(h, hl, &st);
                if (st != 200 || cl == -3 || cl == -1 || (cl >= 0 && (size_t)cl + hl > MAX_RESP)) {
                    pthread_mutex_unlock(&g_mu);
                    hsock_close(s);
                    char why[48];
                    snprintf(why, sizeof why, "answered %d%s", st, cl == -3 ? " (an encoding that is not followed)" : cl == -1 ? " (no length)" : "");
                    fetch_fail(e, why);
                    return NULL;
                }
                e->head = hl;
                if (cl == -2) e->chunked = true, e->cpos = hl;
                else e->total = hl + (size_t)cl;
            }
        }
        if (e->chunked && !e->total) {
            long end = chunks_end(e);
            if (end < 0) { pthread_mutex_unlock(&g_mu); hsock_close(s); fetch_fail(e, "the chunks are not in order"); return NULL; }
            if (end > 0) e->total = (size_t)end;
        }
        bool done = e->total && e->len >= e->total;
        if (done) {
            e->len = e->total;
            e->state = E_DONE;
            e->t_done = now_ms();
            LOG("[ocw-http] early download of %u done: %zu bytes in %llu ms\n", e->id, e->len, (unsigned long long)(e->t_done - e->t0));
        }
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
        if (done) break;
    }
    hsock_close(s);
    return NULL;
}

static void ensure_init(void);

void ocwproxy_prefetch(uint32_t id) {
    ensure_init();
    if (!g.ready || !id) return;
    pthread_mutex_lock(&g_mu);
    uint64_t now = now_ms();
    Entry *slot = NULL;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        Entry *e = &g_e[i];
        if (e->state != E_FREE && e->id == id && e->state != E_FAILED) { pthread_mutex_unlock(&g_mu); return; }  // (already on its way)
        if (e->state != E_FREE && !e->refs && e->state != E_FETCHING && now - e->t0 > KEEP_MS) entry_reset(e);  // (nobody asked: dropped)
    }
    for (int i = 0; i < MAX_ENTRIES && !slot; i++)
        if (g_e[i].state == E_FREE) slot = &g_e[i];
    for (int i = 0; i < MAX_ENTRIES && !slot; i++)  // (else the oldest one that is finished and not being served)
        if (!g_e[i].refs && g_e[i].state != E_FETCHING && (!slot || g_e[i].t0 < slot->t0)) slot = &g_e[i];
    if (!slot) { pthread_mutex_unlock(&g_mu); return; }
    entry_reset(slot);
    slot->id = id, slot->state = E_FETCHING, slot->t0 = now;
    pthread_t th;
    if (pthread_create(&th, NULL, fetch_main, slot) != 0) { slot->state = E_FREE; pthread_mutex_unlock(&g_mu); return; }
    pthread_detach(th);
    LOG("[ocw-http] early download of %u started\n", id);
    pthread_mutex_unlock(&g_mu);
}

// ---- the game's connections
static bool read_head(hsock_t c, uint8_t *b, size_t cap, size_t *len) {  // until the blank line; false: closed, too long or idle
    for (;;) {
        for (size_t i = 0; i + 4 <= *len; i++)
            if (!memcmp(b + i, "\r\n\r\n", 4)) return true;
        if (*len >= cap) return false;
        int r = recv_some(c, b + *len, cap - *len, IDLE_MS);
        if (r <= 0) return false;
        *len += (size_t)r;
    }
}

// The game's connection goes straight to the server from here on: what it already sent, then both ways until one side ends.
static void relay(hsock_t c, const uint8_t *first, size_t n) {
    hsock_t u = up_connect();
    if (u == HSOCK_BAD) return;
    if (n && !send_all(u, first, n)) { hsock_close(u); return; }
    uint8_t b[16384];
    for (;;) {
        hpollfd f[2] = {{0}, {0}};
        f[0].fd = c, f[0].events = POLLIN, f[1].fd = u, f[1].events = POLLIN;
        int r = hsock_poll(f, 2, IDLE_MS);
        if (r <= 0) break;
        bool end = false;
        for (int i = 0; i < 2 && !end; i++) {
            if (!(f[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            int k = recv(i ? u : c, (char *)b, sizeof b, 0);
            if (k <= 0 && !(k < 0 && hsock_would_block(hsock_errno()))) { end = true; break; }
            if (k > 0 && !send_all(i ? c : u, b, (size_t)k)) end = true;
        }
        if (end) break;
    }
    hsock_close(u);
}

// The game asked for /level_data/<id> and an early download of it exists: send it as it arrives. 1 served, 0 not possible (nothing was
// sent: the request goes to the server), -1 broke off part-way (the connection is closed, as if the server had dropped it).
static int serve(Entry *e, hsock_t c, uint32_t id) {
    size_t pos = 0;
    uint64_t t_ask = now_ms();
    pthread_mutex_lock(&g_mu);
    size_t have = e->len, total = e->total;
    int st = e->state;
    uint64_t age = t_ask - e->t0;
    pthread_mutex_unlock(&g_mu);
    LOG("[ocw-http] the game asks for %u: early download %s, %zu of %zu bytes in, %llu ms after it began\n", id,
        st == E_DONE ? "done" : st == E_FAILED ? "failed" : "still arriving", have, total, (unsigned long long)age);
    uint8_t chunk[32768];
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (e->len <= pos && e->state == E_FETCHING) {
            struct timespec ts;
            timespec_get(&ts, TIME_UTC);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_cv, &g_mu, &ts);
        }
        size_t avail = e->len - pos;
        int state = e->state;
        if (avail > sizeof chunk) avail = sizeof chunk;
        if (avail) memcpy(chunk, e->buf + pos, avail);
        pthread_mutex_unlock(&g_mu);
        if (avail) {
            if (!send_all(c, chunk, avail)) return -1;
            pos += avail;
            continue;
        }
        if (state == E_DONE) break;
        return pos ? -1 : 0;  // (it failed: before the first byte the request can still go to the server)
    }
    LOG("[ocw-http] %u sent to the game: %zu bytes, %llu ms after it asked\n", id, pos, (unsigned long long)(now_ms() - t_ask));
    return 1;
}

static Entry *acquire(uint32_t id) {
    pthread_mutex_lock(&g_mu);
    Entry *r = NULL;
    for (int i = 0; i < MAX_ENTRIES; i++)
        if (g_e[i].state != E_FREE && g_e[i].id == id && g_e[i].state != E_FAILED) { r = &g_e[i]; r->refs++; break; }
    pthread_mutex_unlock(&g_mu);
    return r;
}

static void release(Entry *e) {
    pthread_mutex_lock(&g_mu);
    e->refs--;
    pthread_mutex_unlock(&g_mu);
}

static void *client_main(void *arg) {
    hsock_t c = (hsock_t)(uintptr_t)arg;
    hsock_nonblock(c);
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    uint8_t req[8192];
    size_t len = 0;
    for (;;) {
        if (!read_head(c, req, sizeof req, &len)) break;
        size_t hl = 0;
        for (size_t i = 0; i + 4 <= len; i++)
            if (!memcmp(req + i, "\r\n\r\n", 4)) { hl = i + 4; break; }
        uint32_t id = 0;
        bool level = len > 17 && !memcmp(req, "GET /level_data/", 16);
        if (level) {
            const char *p = (const char *)req + 16;
            while (*p >= '0' && *p <= '9' && id < 429496729u) id = id * 10 + (uint32_t)(*p++ - '0');
            level = id && p[0] == ' ' && !strncmp(p, " HTTP/1.", 8);
        }
        if (level) {  // (its headers, for the early downloads to send as well)
            const char *rest = (const char *)req + 16;
            while (*rest >= '0' && *rest <= '9') rest++;
            size_t rl = hl - (size_t)(rest - (const char *)req);
            pthread_mutex_lock(&g_mu);
            if (!g_tmpl_len && rl < sizeof g_tmpl) {
                memcpy(g_tmpl, rest, rl);
                g_tmpl_len = rl;
                // (the request line and the NAMES of the headers: a value can be the player's own id)
                char text[300];
                size_t o = 0;
                for (size_t i = 0; i < hl && o + 3 < sizeof text;) {
                    size_t e = i;
                    while (e < hl && req[e] != '\n') e++;
                    size_t k = i;
                    while (k < e && req[k] != ':' && req[k] != '\r' && !(i && req[k] == ' ')) k++;
                    if (i && (k == e || k == i)) { i = e + 1; continue; }  // (the blank line at the end)
                    for (size_t j = i; j < k && o + 3 < sizeof text; j++) text[o++] = (char)(req[j] >= 0x20 && req[j] < 0x7f ? req[j] : '.');
                    if (i) text[o++] = ':';
                    text[o++] = '|';
                    i = e + 1;
                }
                text[o] = 0;
                LOG("[ocw-http] the game's request, to be repeated for the early downloads (header values not shown): %s\n", text);
            }
            pthread_mutex_unlock(&g_mu);
        }
        Entry *e = level ? acquire(id) : NULL;
        if (!e) {
            if (level) LOG("[ocw-http] the game asks for %u: no early download, the request goes on as it is\n", id);
            relay(c, req, len);
            break;
        }
        int r = serve(e, c, id);
        release(e);
        if (r == 0) { relay(c, req, len); break; }
        if (r < 0) break;
        memmove(req, req + hl, len - hl);  // (a further request on the same connection)
        len -= hl;
    }
    hsock_close(c);
    pthread_mutex_lock(&g_mu);
    g.clients--;
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

static void *accept_main(void *arg) {
    hsock_t ls = (hsock_t)(uintptr_t)arg;
    for (;;) {
        hsock_t c = accept(ls, NULL, NULL);
        if (c == HSOCK_BAD) {  // (the listener is gone: the game connects as before)
            pthread_mutex_lock(&g_mu);
            g.ready = false;
            pthread_mutex_unlock(&g_mu);
            return NULL;
        }
        pthread_mutex_lock(&g_mu);
        bool room = g.clients < MAX_CLIENTS;
        if (room) g.clients++;
        pthread_mutex_unlock(&g_mu);
        pthread_t th;
        if (!room || pthread_create(&th, NULL, client_main, (void *)(uintptr_t)c) != 0) {
            if (room) { pthread_mutex_lock(&g_mu); g.clients--; pthread_mutex_unlock(&g_mu); }
            hsock_close(c);
            continue;
        }
        pthread_detach(th);
    }
}

static bool listen_start(void) {
    hsock_startup();
    hsock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == HSOCK_BAD) return false;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7f000001);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 8) != 0) { hsock_close(ls); return false; }
    hsocklen_t n = sizeof a;
    if (getsockname(ls, (struct sockaddr *)&a, &n) != 0) { hsock_close(ls); return false; }
    g.listen_port = ntohs(a.sin_port);
    pthread_t th;
    if (pthread_create(&th, NULL, accept_main, (void *)(uintptr_t)ls) != 0) { hsock_close(ls); return false; }
    pthread_detach(th);
    return true;
}

#ifdef OCWPROXY_TEST
uint16_t ocwproxy_test_start(uint32_t up_ip, uint16_t up_port, const char *host) {
    g.up_ip = up_ip, g.up_port = up_port;
    snprintf(g.host, sizeof g.host, "%s", host);
    g.tried = true;
    g.ready = listen_start();
    return g.ready ? g.listen_port : 0;
}
static void ensure_init(void) {}
#else
static void ensure_init(void) {
    pthread_mutex_lock(&g_mu);
    bool first = !g.tried;
    g.tried = true;
    pthread_mutex_unlock(&g_mu);
    if (!first) return;
    const char *off = getenv("RECOMP_OCW_PREFETCH");
    if (off && off[0] == '0') { LOG("[ocw-http] early downloads are off (RECOMP_OCW_PREFETCH=0)\n"); return; }
    char host[256];
    uint32_t ip;
    if (!net_ocw_host(host, sizeof host) || !host[0] || !net_resolve(host, &ip)) return;
    uint16_t port = net_dest_port(ip, 80);
    pthread_mutex_lock(&g_mu);
    g.up_ip = ip, g.up_port = port;
    snprintf(g.host, sizeof g.host, "%s", host);
    pthread_mutex_unlock(&g_mu);
    if (!listen_start()) { LOG("[ocw-http] the early-download proxy could not start: the game connects as before\n"); return; }
    char t[16];
    ip_to_str(ip, t);
    LOG("[ocw-http] early downloads on: the game's connections to %s:80 go through 127.0.0.1:%u\n", t, g.listen_port);
    g.ready = true;
}
#endif

bool ocwproxy_route(uint32_t ip, uint16_t port, uint32_t *to_ip, uint16_t *to_port) {
    if (port != 80) return false;
    ensure_init();
    if (!g.ready || ip != g.up_ip) return false;
    *to_ip = 0x7f000001, *to_port = g.listen_port;
    return true;
}
