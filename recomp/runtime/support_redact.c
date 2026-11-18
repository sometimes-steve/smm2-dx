// Redaction of text for a support report (support_redact.h).
#include "support_redact.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int is_alnum(int c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int is_tokenish(int c) { return is_alnum(c) || c == '-' || c == '_' || c == '+' || c == '/' || c == '=' || c == '.'; }

typedef struct {
    char *out;
    size_t cap, n;
    int full;
} Out;

static void put(Out *o, const char *s, size_t len) {
    if (o->full) return;
    if (o->n + len >= o->cap) { o->full = 1; return; }
    memcpy(o->out + o->n, s, len);
    o->n += len;
}
static void puts_(Out *o, const char *s) { put(o, s, strlen(s)); }

// does `in` at i start with the literal (ignoring case)?
static int starts_ci(const char *in, size_t len, size_t i, const char *lit, size_t ll) {
    if (!ll || i + ll > len) return 0;
    for (size_t k = 0; k < ll; k++)
        if (lower((unsigned char)in[i + k]) != lower((unsigned char)lit[k])) return 0;
    return 1;
}

// words whose value (after ':' or '=' or a space) is taken out
static const char *const SECRET_WORDS[] = {"token", "authorization", "bearer", "password", "passwd", "secret", "api_key", "apikey",
                                           "api-key", "cookie", "access_key", "private_key"};

// a secret word at in[i]: the length of the word, or 0 (it must not be the middle of a longer word on the left)
static size_t secret_word_at(const char *in, size_t len, size_t i) {
    if (i > 0 && is_alnum((unsigned char)in[i - 1])) return 0;
    for (size_t w = 0; w < sizeof SECRET_WORDS / sizeof *SECRET_WORDS; w++) {
        size_t wl = strlen(SECRET_WORDS[w]);
        if (starts_ci(in, len, i, SECRET_WORDS[w], wl)) return wl;
    }
    return 0;
}

// an IPv4 address at in[i] (not part of a longer number run): its length, or 0
static size_t ipv4_at(const char *in, size_t len, size_t i) {
    if (i > 0 && (is_alnum((unsigned char)in[i - 1]) || in[i - 1] == '.')) return 0;
    size_t p = i;
    for (int part = 0; part < 4; part++) {
        size_t d = 0;
        int v = 0;
        while (p < len && in[p] >= '0' && in[p] <= '9' && d < 4) v = v * 10 + (in[p] - '0'), p++, d++;
        if (d == 0 || d > 3 || v > 255) return 0;
        if (part < 3) {
            if (p >= len || in[p] != '.') return 0;
            p++;
        }
    }
    if (p < len && (is_alnum((unsigned char)in[p]) || (in[p] == '.' && p + 1 < len && in[p + 1] >= '0' && in[p + 1] <= '9'))) return 0;
    return p - i;
}

// a Course ID / Maker ID (XXX-XXX-XXX of capitals and digits) at in[i]: 11, or 0
static size_t id_at(const char *in, size_t len, size_t i) {
    if (i + 11 > len) return 0;
    if (i > 0 && (is_alnum((unsigned char)in[i - 1]) || in[i - 1] == '-')) return 0;
    for (int k = 0; k < 11; k++) {
        unsigned char c = (unsigned char)in[i + k];
        if (k == 3 || k == 7) { if (c != '-') return 0; }
        else if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return 0;
    }
    if (i + 11 < len && (is_alnum((unsigned char)in[i + 11]) || in[i + 11] == '-')) return 0;
    return 11;
}

// an e-mail address around the '@' at in[at]: [start, end), or empty
static int email_around(const char *in, size_t len, size_t at, size_t *start, size_t *end) {
    size_t s = at, e = at + 1;
    while (s > 0 && (is_alnum((unsigned char)in[s - 1]) || strchr("._%+-", in[s - 1]))) s--;
    while (e < len && (is_alnum((unsigned char)in[e]) || in[e] == '.' || in[e] == '-')) e++;
    if (s == at || e == at + 1) return 0;
    const char *dot = memchr(in + at + 1, '.', e - at - 1);
    if (!dot) return 0;
    *start = s, *end = e;
    return 1;
}

size_t support_redact(const char *in, size_t in_len, char *out, size_t cap, const RedactLiteral *lits, int nlits) {
    if (!cap) return 0;
    Out o = {out, cap, 0, 0};
    size_t i = 0;
    while (i < in_len && !o.full) {
        // 1. literals (longest first would be better: the caller lists the longer ones first)
        int hit = 0;
        for (int k = 0; k < nlits && !hit; k++) {
            size_t ll = lits[k].find ? strlen(lits[k].find) : 0;
            if (starts_ci(in, in_len, i, lits[k].find, ll)) {
                puts_(&o, lits[k].with);
                i += ll;
                hit = 1;
            }
        }
        if (hit) continue;
        // 2. a secret word and its value
        size_t sw = secret_word_at(in, in_len, i);
        if (sw) {
            size_t p = i + sw;
            // an optional closing quote, then separators (':', '=', space, quotes), then the value
            size_t q = p;
            while (q < in_len && (in[q] == '"' || in[q] == '\'' || in[q] == ':' || in[q] == '=' || in[q] == ' ')) q++;
            size_t v = q;
            while (v < in_len && in[v] != '\n' && in[v] != '\r' && in[v] != ' ' && in[v] != '"' && in[v] != '\'' && in[v] != ',' && in[v] != ';' &&
                   in[v] != '&' && in[v] != '}')
                v++;
            if ((v - q == 6 && starts_ci(in, in_len, q, "bearer", 6)) || (v - q == 5 && starts_ci(in, in_len, q, "basic", 5))) {
                while (v < in_len && in[v] == ' ') v++;  // ("Authorization: Bearer <token>": the token is the value)
                while (v < in_len && in[v] != '\n' && in[v] != '\r' && in[v] != ' ' && in[v] != '"' && in[v] != '\'' && in[v] != ',') v++;
            }
            if (q > p && v > q && q - p <= 6) {  // (a separator between: "token = abc", not "tokens" or "token_count")
                put(&o, in + i, q - i);
                puts_(&o, "<redacted>");
                i = v;
                continue;
            }
        }
        unsigned char c = (unsigned char)in[i];
        // 3. an e-mail address (found at its '@', then taken from its start: the start is already written, so it is checked here, at the start)
        if (c == '@') {
            // (the local part before it was copied already: cut it back from the output)
            size_t s, e;
            if (email_around(in, in_len, i, &s, &e)) {
                size_t back = i - s;
                if (o.n >= back && !memcmp(out + o.n - back, in + s, back)) o.n -= back;  // (only if it was copied as it is)
                puts_(&o, "<email>");
                i = e;
                continue;
            }
        }
        // 4. an IPv4 address
        if (c >= '0' && c <= '9') {
            size_t l = ipv4_at(in, in_len, i);
            if (l && !(l == 9 && !memcmp(in + i, "127.0.0.1", 9)) && !(l == 7 && !memcmp(in + i, "0.0.0.0", 7))) {
                puts_(&o, "<ip>");
                i += l;
                continue;
            }
        }
        // 5. a Course ID / Maker ID
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            if (id_at(in, in_len, i)) {
                puts_(&o, "<id>");
                i += 11;
                continue;
            }
        }
        // 6. a long token-like string
        if (is_tokenish(c) && (i == 0 || !is_tokenish((unsigned char)in[i - 1]))) {
            size_t e = i;
            int letters = 0, digits = 0, nonhex = 0, upper = 0, lowerc = 0;
            while (e < in_len && is_tokenish((unsigned char)in[e])) {
                unsigned char ch = (unsigned char)in[e];
                if (is_alnum(ch)) {
                    if (ch >= '0' && ch <= '9') digits++;
                    else letters++;
                    if (ch >= 'A' && ch <= 'Z') upper++;
                    if (ch >= 'a' && ch <= 'z') lowerc++;
                    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) nonhex++;
                }
                e++;
            }
            // (a path or a version is long but has no mix: a token has letters of both cases and digits. A lower-case hex string is a hash
            // or an id the log is read by - a build id; an all-capitals name is a cipher suite; "_Z..." is a C++ symbol the log names. A
            // token the app was given is taken out as a literal, by its value, whatever it looks like)
            bool symbol = in[i] == '_' && i + 1 < in_len && in[i + 1] == 'Z';
            if (e - i >= 32 && letters >= 4 && digits >= 4 && nonhex > 0 && upper > 0 && lowerc > 0 && !symbol && !memchr(in + i, '/', e - i)) {
                puts_(&o, "<token>");
                i = e;
                continue;
            }
        }
        put(&o, in + i, 1);
        i++;
    }
    if (o.full) {  // (cut at the last whole line)
        while (o.n > 0 && out[o.n - 1] != '\n') o.n--;
    }
    out[o.n] = 0;
    return o.n;
}
