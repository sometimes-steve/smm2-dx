// A passive reader of OpenCourse World's WebSocket stream (nextap.h).
#include "nextap.h"

#include <stdlib.h>
#include <string.h>

#define TAP_MAX_STREAM (1 << 20)   // unconsumed bytes held at most (a frame bigger than this makes the tap give up)
#define TAP_MAX_MSG (256 << 10)    // one joined message at most

static int grow(uint8_t **b, size_t *cap, size_t need) {
    if (need <= *cap) return 1;
    size_t nc = *cap ? *cap : 4096;
    while (nc < need) nc *= 2;
    uint8_t *nb = realloc(*b, nc);
    if (!nb) return 0;
    *b = nb, *cap = nc;
    return 1;
}

// One PRUDP Lite packet's payload (n bytes, unmasked): a DATA packet without the ACK flag that carries payload. Returns the payload's
// offset and size and the fragment id (0 is the last fragment, 1 the first of a split message).
static int prudp_data(const uint8_t *f, size_t n, size_t *off, size_t *size, unsigned *frag) {
    if (n < 12 || f[0] != 0x80) return 0;
    size_t o = f[1], ps = (size_t)f[2] | (size_t)f[3] << 8;
    if (!ps || 12 + o + ps != n) return 0;
    if ((f[8] & 0x0f) != 2 || ((f[8] >> 4) & 1)) return 0;
    *off = 12 + o, *size = ps, *frag = f[7];
    return 1;
}

static void message(NexTap *t, NexTapFn fn, void *user) {
    const uint8_t *m = t->msg;
    size_t n = t->mlen;
    if (n < 6) return;
    uint32_t size = (uint32_t)m[0] | (uint32_t)m[1] << 8 | (uint32_t)m[2] << 16 | (uint32_t)m[3] << 24;
    if (size + 4 != n) return;  // (not one whole message)
    unsigned pid = m[4];
    if (pid & 0x80) return;     // (a request: the server only sends those for its own protocols)
    if (m[5] != 1 || n < 14) return;  // (an error answer, or too short to carry a method)
    uint32_t call = (uint32_t)m[6] | (uint32_t)m[7] << 8 | (uint32_t)m[8] << 16 | (uint32_t)m[9] << 24;
    uint32_t method = (uint32_t)m[10] | (uint32_t)m[11] << 8 | (uint32_t)m[12] << 16 | (uint32_t)m[13] << 24;
    fn(user, pid, method & 0x7fff, call, m + 14, n - 14);
}

void nextap_feed(NexTap *t, const uint8_t *p, size_t n, NexTapFn fn, void *user) {
    if (t->state < 0 || !n) return;
    if (t->len + n > TAP_MAX_STREAM || !grow(&t->buf, &t->cap, t->len + n)) { t->state = -1; return; }
    memcpy(t->buf + t->len, p, n);
    t->len += n;
    size_t pos = 0;
    if (t->state == 0) {  // the HTTP answer to the upgrade ("HTTP/1.1 101 ..."), then frames
        static const char UP[] = "HTTP/1.1 101";
        size_t k = t->len < sizeof UP - 1 ? t->len : sizeof UP - 1;
        if (memcmp(t->buf, UP, k)) { t->state = -1; return; }
        size_t i = 0;
        for (; i + 4 <= t->len; i++)
            if (!memcmp(t->buf + i, "\r\n\r\n", 4)) break;
        if (i + 4 > t->len) return;  // (not all of it here yet)
        pos = i + 4;
        t->state = 1;
    }
    while (t->len - pos >= 2) {
        const uint8_t *f = t->buf + pos;
        size_t l = f[1] & 0x7f, h = 2;
        if (l == 126) {
            if (t->len - pos < 4) break;
            l = (size_t)f[2] << 8 | f[3], h = 4;
        } else if (l == 127) {
            if (t->len - pos < 10) break;
            if (f[2] | f[3] | f[4] | f[5] | f[6]) { t->state = -1; return; }  // (over 16 MB: not something to follow)
            l = (size_t)f[7] << 16 | (size_t)f[8] << 8 | f[9], h = 10;
        }
        if (f[1] & 0x80) h += 4;  // (a mask: the server does not send one)
        if (l > TAP_MAX_MSG) { t->state = -1; return; }
        if (t->len - pos < h + l) break;
        if (f[0] == 0x82 && !(f[1] & 0x80)) {  // a binary frame without a mask: a PRUDP packet
            size_t off, ps;
            unsigned frag;
            if (prudp_data(f + h, l, &off, &ps, &frag)) {
                if (frag == 1) t->mlen = 0;  // (the first of a split message; fragment 0 is the last, or the only one)
                if (t->mlen + ps > TAP_MAX_MSG || !grow(&t->msg, &t->mcap, t->mlen + ps)) {
                    t->mlen = 0;
                } else {
                    memcpy(t->msg + t->mlen, f + h + off, ps);
                    t->mlen += ps;
                    if (frag == 0) {
                        message(t, fn, user);
                        t->mlen = 0;
                    }
                }
            }
        }
        pos += h + l;
    }
    if (pos) {
        memmove(t->buf, t->buf + pos, t->len - pos);
        t->len -= pos;
    }
}

void nextap_free(NexTap *t) {
    free(t->buf);
    free(t->msg);
    memset(t, 0, sizeof *t);
}
