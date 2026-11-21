// Early course downloads from OpenCourse World (issue #3).
//
// To play a course the game asks the server for one (DataStore method 79: the answer names the course's data id), then makes two more calls
// before it opens a plain-HTTP connection to the same host and fetches GET /level_data/<id> (a 377,073 byte file). The id is known from the
// answer to the first call, about 0.6 s before the game starts the download. This module starts that download as soon as the id is known
// and hands it to the game when it asks.
//
//   ocwproxy_prefetch   the id is known (runtime/services/ssl.c, from the server's answer): fetch it in the background
//   ocwproxy_route      where the game's connection to ip:port should really go: its connections to OCW's port 80 go to a proxy on
//                       the loopback address, which answers GET /level_data/<id> from the early download (while it is still arriving)
//                       and passes every other request, and any it has no download for, straight on to the server
//
// Nothing the game sends or receives is changed: the same bytes the server would send, the same calls on the WebSocket, so play history and
// challenge counts are what they were. Without the proxy (RECOMP_OCW_PREFETCH=0, or a failure to start it) the game connects as before.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void ocwproxy_prefetch(uint32_t data_id);
bool ocwproxy_route(uint32_t ip, uint16_t port, uint32_t *to_ip, uint16_t *to_port);

#ifdef OCWPROXY_TEST
// (tests) the proxy in front of a server at up_ip:up_port (host order) with the Host header `host`; returns the loopback port it listens on, 0 on failure
uint16_t ocwproxy_test_start(uint32_t up_ip, uint16_t up_port, const char *host);
#endif
