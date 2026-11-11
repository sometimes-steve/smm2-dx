// The game's own network stack (runtime/services/bsd.c sockets, sfdnsres.c names, nsd.c, ssl.c TLS, acc.c token),
// talking to one backend: OpenCourse World (games/smm2/mods/ocw.c reads ocw-config.json in the data folder and calls
// net_ocw_set_target). Deny by default: nothing here ever asks the host's DNS for a name the game gives, and a game
// socket reaches only OCW's one host (and loopback). OCW is the only backend that reaches a public host, and it
// reaches exactly the one name that config names - every other name still fails and every other address is still
// refused. Its certificate is checked: the chain must end in a public root the runtime carries (net/roots.c:
// Let's Encrypt's ISRG Root X1 / X2 among them) or one the system bundle has, and match the host name
// (tls_trust=roots, the default). OCW is to be open source and self-hostable, so tls_trust=any still turns the check
// off for a server of one's own - a LAN instance or OCW itself under development, where there is no certificate a
// public root can vouch for.
//
// Settings: environment variable, else the game's settings (game_net_config: smm2mods.ini for SMM2):
//   RECOMP_NET_TLS_TRUST      tls_trust=         roots (default): the chain must end in a public root the runtime
//                                                carries or the system bundle has, and match the host name;
//                                                any: accept any certificate (a self-hosted ocw server)
//   RECOMP_NET_LOG            net_log=1          log every socket, name, TLS and token call
//   RECOMP_NET_LOG_DATA       net_log_data=1     ... and the TLS plaintext itself (the first 160 bytes of each read
//                                                and write): noisy, and off even with net_log=1. Registered secrets
//                                                are hidden from it (net_log_secret)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { NET_TRUST_ANY = 1, NET_TRUST_ROOTS = 3 } NetTrust;

bool net_real_sockets(void);           // the game's sockets reach the host's network stack (to OCW's host only)
bool net_log_on(void);
bool net_log_data(void);  // ... and the TLS plaintext (net_log_data=1; off even with net_log=1)
// A log line with what must not be read from it taken out: every "PID=<digits>" (any case; NEX station URLs carry
// it) becomes "PID=<hidden>", and so does any string registered with net_log_secret (an account token). Always
// NUL-terminated within cap; a cut digit run is dropped, never printed.
void net_log_redact(const char *in, char *out, size_t cap);
// A secret that must never appear in a log, registered before the game runs (a log file is what a player attaches
// to a bug report). Shorter than 8 characters: ignored, too short to find in ordinary text safely.
void net_log_secret(const char *s);
#define NLOG(...) do { if (net_log_on()) LOG(__VA_ARGS__); } while (0)

void net_config_reload(void);          // at an in-game reboot: the settings read again (main.c boot)

// Names. Returns true and the IPv4 address (host order) OCW's host resolves to, false = no such name (every name but
// that one). Literal dotted addresses resolve to themselves.
bool net_resolve(const char *name, uint32_t *ip);
// The last name resolved to ip (TLS: the game connects by address and names the address as the host); false = none
bool net_name_for_ip(uint32_t ip, char *out, size_t cap);
// May a game socket connect / send to ip (host order)? OCW's addresses and loopback only (TCP and UDP alike).
bool net_dest_allowed(uint32_t ip);
bool net_dest_allowed_udp(uint32_t ip);
bool net_ip_is_local(uint32_t ip);     // loopback, private, link-local, CGNAT
// The port a game connection to ip:port really goes to (always the port the game asks for)
uint16_t net_dest_port(uint32_t ip, uint16_t port);
const char *net_nsd_environment(void); // the nsd environment that replaces '%' in the game's host names ("lp1")

// The OCW backend's one host (games/smm2/mods/ocw.c, from ocw-config.json), set while the modules are loaded and
// before the game's first instruction. `host` is the name written into the game and the name it looks up; `ip`
// (NULL or empty: ask the host resolver) points that name at an address, which is how a self-hosted or local OCW
// instance is reached and how an IP-only setup works. Nothing else resolves and nothing else is reachable.
void net_ocw_set_target(const char *host, const char *ip);
bool net_ocw_host(char *out, size_t cap);  // the configured host ("" / false: none)
// Does the configured host resolve right now? 1 yes (or it is an address), 0 no, -1 not known yet (no host, or the
// first lookup is still out). Never blocks: the lookup runs on a thread and is repeated, so call it as often as needed.
int net_ocw_reachable(void);

// What the backend wants as the network service account (services/acc.c): the account ID to use, with no ID token
// at all. It matches the console mod exactly (_vendor/ocw-client-mod: it stubs GetNetworkServiceAccountId to one
// shared id and LoadNetworkServiceAccountIdTokenCache to nothing), because OCW identifies the player by the token in
// the WebSocket handshake, not by this id - the id is the same for every OCW player on a console.
bool net_account_shared(uint64_t *id);

// TLS
NetTrust net_tls_trust(void);         // NET_TRUST_ROOTS unless tls_trust=any
// May a connection to ip go ahead with "accept any certificate" (services/ssl.c)? Local addresses always; and OCW's
// own addresses too (the owner's choice: a self-hosted OCW's session is not certificate-checked).
bool net_tls_any_allowed(uint32_t ip);

void ip_to_str(uint32_t ip, char out[16]);
