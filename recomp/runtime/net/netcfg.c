// The game network stack's configuration (runtime/net/netcfg.h): the one online backend, OpenCourse World, which
// names resolve where, which addresses a game socket may reach, and how TLS servers are trusted. Read once, the first
// time anything asks. Deny by default: nothing here ever asks the host's DNS for a name the game gives, except the one
// host OpenCourse World's config names.
#include "netcfg.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../hle.h"
#include "hostsock.h"

// a game's own settings store (runtime/game_hooks.c, weak: none): SMM2 keeps these in smm2mods.ini
bool game_net_config(const char *key, char *out, size_t cap);
void game_net_config_set(const char *key, const char *val);

static pthread_once_t once = PTHREAD_ONCE_INIT;
static NetTrust trust = NET_TRUST_ROOTS;
static bool log_on, log_data_on, loaded;

static pthread_mutex_t rev_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint32_t ip; char name[128]; } rev[16];
static int nrev;

// ---- the ocw backend's one host (net_ocw_set_target, from games/smm2/mods/ocw.c)
#define OCW_MAX_IPS 8
#define OCW_TTL_S 300    // re-resolve the name this often: OCW's address is in AWS and will move
#define OCW_RETRY_S 15   // ... and after a lookup that found nothing, no sooner than this (the game retries hard)
static pthread_mutex_t ocw_lock = PTHREAD_MUTEX_INITIALIZER;
static char ocw_name[128];                 // lower case, as net_resolve compares
static char ocw_fixed[64];                 // the config's "ip": the name points here, the resolver is never asked
static uint32_t ocw_ips[OCW_MAX_IPS];      // the addresses the name has answered with lately: a session already
static int ocw_nips, ocw_oldest;           // connected to one must keep sending to it, so they stay allowed for a
                                           // while. A ring: full, the oldest goes, so a new one always gets in
static uint32_t ocw_cur;                   // the newest the name answered with: what net_resolve hands out
static time_t ocw_when;

static bool cfg(const char *env, const char *key, char *out, size_t cap) {
    const char *e = getenv(env);
    if (e) { snprintf(out, cap, "%s", e); return true; }
    return game_net_config(key, out, cap);
}

void ip_to_str(uint32_t ip, char out[16]) {
    snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
}

static bool parse_ip(const char *s, uint32_t *ip) {
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
    *ip = a << 24 | b << 16 | c << 8 | d;
    return true;
}

bool net_ip_is_local(uint32_t ip) {
    return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 20) == (172 << 4 | 1) || (ip >> 16) == (192 << 8 | 168) ||
           (ip >> 16) == (169 << 8 | 254) || (ip >> 22) == (100u << 2 | 1);  // (100.64.0.0/10: CGNAT, e.g. Tailscale)
}

static void remember(uint32_t ip, const char *name) {
    pthread_mutex_lock(&rev_lock);
    int i = 0;
    while (i < nrev && rev[i].ip != ip) i++;
    if (i == nrev) i = nrev < 16 ? nrev++ : 15;
    rev[i].ip = ip;
    snprintf(rev[i].name, sizeof rev[i].name, "%s", name);
    pthread_mutex_unlock(&rev_lock);
}

static void load(void) {
    char v[1024];
    if (cfg("RECOMP_NET_LOG", "net_log", v, sizeof v)) log_on = atoi(v) != 0;
    if (cfg("RECOMP_NET_LOG_DATA", "net_log_data", v, sizeof v)) log_data_on = atoi(v) != 0;
    // OpenCourse World serves an ordinary publicly-signed certificate, so it is checked like any other: the chain must
    // end in a public root and match the host name (tls_trust=roots, the default). A self-hosted OCW (a LAN instance,
    // or OCW itself under development) has no such certificate, so tls_trust=any stays available for it - explicitly,
    // never by default.
    if (cfg("RECOMP_NET_TLS_TRUST", "tls_trust", v, sizeof v)) trust = !strcmp(v, "any") ? NET_TRUST_ANY : NET_TRUST_ROOTS;
    loaded = true;
    LOG("[net] OpenCourse World: the game's own online client, certificate %s\n",
        trust == NET_TRUST_ANY ? "NOT checked (tls_trust=any: a server of your own)"
                               : "checked against the public roots and the host name");
}

static void init(void) { pthread_once(&once, load); }

// At an in-game reboot (main.c boot): the settings read again. Before the first load: nothing.
void net_config_reload(void) {
    if (!loaded) return;
    trust = NET_TRUST_ROOTS;
    log_on = log_data_on = false;
    pthread_mutex_lock(&ocw_lock);
    ocw_name[0] = ocw_fixed[0] = 0;
    ocw_nips = ocw_oldest = 0, ocw_cur = 0, ocw_when = 0;
    pthread_mutex_unlock(&ocw_lock);
    pthread_mutex_lock(&rev_lock);
    nrev = 0;
    pthread_mutex_unlock(&rev_lock);
    load();
}

bool net_real_sockets(void) { return true; }
bool net_log_on(void) { init(); return log_on; }
bool net_log_data(void) { init(); return log_data_on; }

// ---- what must never be read out of a log. The PID is the player's account id, which the TLS plaintext dump would
// show in every NEX station URL; a registered secret is an account token (net_log_secret, e.g. OCW's). A log file is
// what a player attaches to a bug report.
#define MAX_SECRETS 4
#define MIN_SECRET 8
static pthread_mutex_t secret_lock = PTHREAD_MUTEX_INITIALIZER;
static char secrets[MAX_SECRETS][160];
static int nsecrets;

void net_log_secret(const char *s) {
    if (!s || strlen(s) < MIN_SECRET) return;  // (too short to find without hitting ordinary text)
    pthread_mutex_lock(&secret_lock);
    bool have = false;
    for (int i = 0; i < nsecrets; i++) have |= !strcmp(secrets[i], s);
    if (!have && nsecrets < MAX_SECRETS) snprintf(secrets[nsecrets++], sizeof secrets[0], "%s", s);
    pthread_mutex_unlock(&secret_lock);
}

// Every registered secret in s, overwritten in place (s is the redacted copy, never the caller's own text).
static void hide_secrets(char *s) {
    if (!nsecrets) return;
    pthread_mutex_lock(&secret_lock);
    for (int i = 0; i < nsecrets; i++) {
        size_t n = strlen(secrets[i]);
        for (char *p = strstr(s, secrets[i]); p; p = strstr(p + n, secrets[i])) memset(p, 'x', n);
    }
    pthread_mutex_unlock(&secret_lock);
}

void net_log_redact(const char *in, char *out, size_t cap) {
    static const char mask[] = "<hidden>";
    size_t o = 0;
    if (!cap) return;
    while (*in && o + 1 < cap) {
        if (tolower((unsigned char)in[0]) == 'p' && tolower((unsigned char)in[1]) == 'i' &&
            tolower((unsigned char)in[2]) == 'd' && in[3] == '=' && isdigit((unsigned char)in[4])) {
            for (int i = 0; i < 4 && o + 1 < cap; i++) out[o++] = *in++;
            while (isdigit((unsigned char)*in)) in++;
            for (const char *m = mask; *m && o + 1 < cap; m++) out[o++] = *m;
            continue;
        }
        out[o++] = *in++;
    }
    out[o] = 0;
    hide_secrets(out);
}
NetTrust net_tls_trust(void) { init(); return trust; }
const char *net_nsd_environment(void) { return "lp1"; }

void net_ocw_set_target(const char *host, const char *ip) {
    pthread_mutex_lock(&ocw_lock);
    size_t n = 0;
    for (; host && host[n] && n < sizeof ocw_name - 1; n++) ocw_name[n] = (char)tolower((unsigned char)host[n]);
    ocw_name[n] = 0;
    while (n && ocw_name[n - 1] == '.') ocw_name[--n] = 0;
    snprintf(ocw_fixed, sizeof ocw_fixed, "%s", ip ? ip : "");
    ocw_nips = 0;
    ocw_oldest = 0;
    ocw_cur = 0;
    ocw_when = 0;
    pthread_mutex_unlock(&ocw_lock);
    LOG("[net] ocw host \"%s\"%s%s\n", ocw_name, ocw_fixed[0] ? " at " : "", ocw_fixed);
}

bool net_ocw_host(char *out, size_t cap) {
    pthread_mutex_lock(&ocw_lock);
    snprintf(out, cap, "%s", ocw_name);
    bool any = ocw_name[0] != 0;
    pthread_mutex_unlock(&ocw_lock);
    return any;
}

// Note an address of the ocw host. Addresses are merged, never replaced: a session already connected to one has to
// go on sending to it even after the name has moved (bsd_host.c checks every send).
static void ocw_add(uint32_t ip) {
    for (int i = 0; i < ocw_nips; i++)
        if (ocw_ips[i] == ip) return;
    char a[16];
    ip_to_str(ip, a);
    if (ocw_nips < OCW_MAX_IPS) {
        ocw_ips[ocw_nips++] = ip;
    } else {  // (the oldest goes: a new address must always get in, or the host becomes unreachable for good)
        char old[16];
        ip_to_str(ocw_ips[ocw_oldest], old);
        LOG("[net] ocw %s: %s replaces %s (%d kept)\n", ocw_name, a, old, OCW_MAX_IPS);
        ocw_ips[ocw_oldest] = ip;
        ocw_oldest = (ocw_oldest + 1) % OCW_MAX_IPS;
        return;
    }
    LOG("[net] ocw %s -> %s\n", ocw_name, a);
}

// The ocw host's addresses, resolved on first use and re-resolved every OCW_TTL_S. Call with ocw_lock held.
static void ocw_lookup_locked(void) {
    if (!ocw_name[0]) return;
    time_t now = time(NULL);
    // (ocw_when is the last attempt: a lookup that found nothing must not be retried on every call)
    if (ocw_when && now - ocw_when < (ocw_nips ? OCW_TTL_S : OCW_RETRY_S)) return;
    ocw_when = now;
    uint32_t ip;
    if (ocw_fixed[0]) {  // the config points the name at an address: the resolver is never asked
        if (parse_ip(ocw_fixed, &ip)) ocw_add(ip), ocw_cur = ip;
        else LOG("[net] ocw ip=\"%s\": not an IPv4 address\n", ocw_fixed);
        return;
    }
    if (parse_ip(ocw_name, &ip)) { ocw_add(ip), ocw_cur = ip; return; }  // the config's domain is itself an address
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hsock_startup();
    int e = getaddrinfo(ocw_name, NULL, &hints, &res);
    if (e || !res) {
        LOG("[net] ocw: cannot resolve %s (%d)\n", ocw_name, e);
        return;
    }
    uint32_t first = 0;
    for (struct addrinfo *a = res; a; a = a->ai_next)
        if (a->ai_family == AF_INET && a->ai_addr) {
            uint32_t v = ntohl(((struct sockaddr_in *)(void *)a->ai_addr)->sin_addr.s_addr);
            ocw_add(v);
            if (!first) first = v;
        }
    freeaddrinfo(res);
    // What the game is told from now on. Without this the first address ever seen was handed out for the rest of the
    // session, so once OCW moved the game dialled a dead address and never recovered.
    if (first && first != ocw_cur) {
        char a[16], b[16];
        ip_to_str(ocw_cur, a), ip_to_str(first, b);
        if (ocw_cur) LOG("[net] ocw %s now at %s (was %s)\n", ocw_name, b, a);
        ocw_cur = first;
    }
}

// Is `name` the ocw host? Then its address (the first known one).
static bool ocw_resolve(const char *name, uint32_t *ip) {
    bool hit = false;
    pthread_mutex_lock(&ocw_lock);
    if (ocw_name[0] && !strcmp(ocw_name, name)) {
        ocw_lookup_locked();
        if (ocw_cur) *ip = ocw_cur, hit = true;
    }
    pthread_mutex_unlock(&ocw_lock);
    if (hit) remember(*ip, name);  // (ssl.c rebuilds the host name from the address for SNI)
    return hit;
}

static bool ocw_owns(uint32_t ip) {
    bool mine = false;
    pthread_mutex_lock(&ocw_lock);
    for (int i = 0; i < ocw_nips && !mine; i++) mine = ocw_ips[i] == ip;
    pthread_mutex_unlock(&ocw_lock);
    return mine;
}

bool net_resolve(const char *name, uint32_t *ip) {
    init();
    char n[256];
    size_t len = 0;
    for (const char *p = name; *p && len < sizeof n - 1; p++) n[len++] = (char)tolower((unsigned char)*p);
    n[len] = 0;
    while (len && n[len - 1] == '.') n[--len] = 0;  // (an absolute name)
    if (parse_ip(n, ip)) return true;
    if (!strcmp(n, "localhost")) { *ip = 0x7f000001; return true; }
    return ocw_resolve(n, ip);
}

bool net_name_for_ip(uint32_t ip, char *out, size_t cap) {
    pthread_mutex_lock(&rev_lock);
    bool found = false;
    for (int i = 0; i < nrev && !found; i++)
        if (rev[i].ip == ip) snprintf(out, cap, "%s", rev[i].name), found = true;
    pthread_mutex_unlock(&rev_lock);
    return found;
}

// The account ID the console mod writes (_vendor/ocw-client-mod/source/program/main.cpp: movz x8, #0xCAFE), shared
// by every OCW player there. Matched exactly so OCW sees what it sees from a console.
#define OCW_NSA_ID 0xCAFEull

bool net_account_shared(uint64_t *id) {
    *id = OCW_NSA_ID;
    return true;
}

bool net_tls_any_allowed(uint32_t ip) {
    init();
    return net_ip_is_local(ip) || ocw_owns(ip);
}

// TCP: the one host OCW's config names, and loopback.
bool net_dest_allowed(uint32_t ip) {
    init();
    if ((ip >> 24) == 127 || ocw_owns(ip)) return true;
    // Not one of ours: the name has moved under us (or this is something else entirely). Let the next lookup ask
    // again instead of waiting out the TTL, so a dropped connection can be remade at the new address.
    pthread_mutex_lock(&ocw_lock);
    ocw_when = 0;
    pthread_mutex_unlock(&ocw_lock);
    return false;
}

bool net_dest_allowed_udp(uint32_t ip) { return net_dest_allowed(ip); }
uint16_t net_dest_port(uint32_t ip, uint16_t port) { (void)ip; return port; }
