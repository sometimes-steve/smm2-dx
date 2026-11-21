// A passive reader of OpenCourse World's WebSocket stream (runtime/services/ssl.c feeds it the plaintext the server sends): it follows the
// HTTP upgrade answer, the WebSocket frames and the PRUDP Lite packets in them, joins the fragments of each RMC message, and says what the
// server answered (protocol, method, parameters). It never changes a byte: the game gets the stream as it was. Header-only API, no game,
// no TLS, so a test can build it on its own (recomp/tools/test_ocw_proxy.py).
#pragma once
#include <stddef.h>
#include <stdint.h>

// One RMC response: the protocol id (0x73 is DataStore), the method, its call id, and the parameters (valid for the call only)
typedef void (*NexTapFn)(void *user, unsigned proto, unsigned method, uint32_t call, const uint8_t *params, size_t n);

typedef struct {
    uint8_t *buf;        // the stream not yet consumed
    size_t len, cap;
    uint8_t *msg;        // the fragments of the message being joined
    size_t mlen, mcap;
    int state;           // 0 waiting for the upgrade answer, 1 following frames, -1 not a stream this can follow (it ignores the rest)
} NexTap;

void nextap_feed(NexTap *t, const uint8_t *p, size_t n, NexTapFn fn, void *user);
void nextap_free(NexTap *t);
