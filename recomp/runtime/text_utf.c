// UTF-8 / UTF-16 conversion and character-counted limits (text_utf.h).
#include "text_utf.h"

void text_utf16_to_utf8(const uint16_t *s, uint32_t n, char *out, size_t cap) {
    if (!cap) return;
    size_t il = 0;
    for (uint32_t i = 0; i < n && s[i] && il + 5 < cap; i++) {
        uint32_t ch = s[i];
        if (ch >= 0xd800 && ch < 0xdc00) {  // a high surrogate: needs its low one
            uint32_t lo = i + 1 < n ? s[i + 1] : 0;
            if (lo >= 0xdc00 && lo < 0xe000) { ch = 0x10000 + ((ch - 0xd800) << 10) + (lo - 0xdc00); i++; }
            else ch = 0xfffd;
        } else if (ch >= 0xdc00 && ch < 0xe000) ch = 0xfffd;  // a low one on its own
        if (ch < 0x80) out[il++] = (char)ch;
        else if (ch < 0x800) { out[il++] = (char)(0xc0 | ch >> 6); out[il++] = (char)(0x80 | (ch & 0x3f)); }
        else if (ch < 0x10000) {
            out[il++] = (char)(0xe0 | ch >> 12); out[il++] = (char)(0x80 | (ch >> 6 & 0x3f));
            out[il++] = (char)(0x80 | (ch & 0x3f));
        } else {
            out[il++] = (char)(0xf0 | ch >> 18); out[il++] = (char)(0x80 | (ch >> 12 & 0x3f));
            out[il++] = (char)(0x80 | (ch >> 6 & 0x3f)); out[il++] = (char)(0x80 | (ch & 0x3f));
        }
    }
    out[il] = 0;
}

size_t text_utf8_to_utf16(const char *s, uint16_t *out, size_t cap) {
    if (!cap) return 0;
    cap--;  // (the NUL)
    size_t k = 0;
    const unsigned char *t = (const unsigned char *)s;
    while (*t && k < cap) {
        uint32_t ch = *t++;
        int more = ch >= 0xf8 ? -1 : ch >= 0xf0 ? 3 : ch >= 0xe0 ? 2 : ch >= 0xc0 ? 1 : ch >= 0x80 ? -1 : 0;
        if (more < 0) ch = 0xfffd;  // (a stray continuation byte, or one no character starts with)
        else if (more) {
            ch &= 0x3f >> more;
            int got = 0;
            for (; got < more && (*t & 0xc0) == 0x80; got++) ch = ch << 6 | (*t++ & 0x3f);
            if (got < more || ch > 0x10ffff || (ch >= 0xd800 && ch < 0xe000)) ch = 0xfffd;  // (cut short, out of range, a surrogate)
        }
        if (ch >= 0x10000) {
            if (k + 1 >= cap) break;
            ch -= 0x10000;
            out[k++] = (uint16_t)(0xd800 | ch >> 10);
            out[k++] = (uint16_t)(0xdc00 | (ch & 0x3ff));
        } else out[k++] = (uint16_t)ch;
    }
    out[k] = 0;
    return k;
}

int text_utf8_chars(const char *s) {
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xc0) != 0x80;
    return n;
}

int text_utf8_clip(char *s, int max_chars) {
    int chars = 0;
    for (char *p = s; *p; p++) {
        if (((unsigned char)*p & 0xc0) == 0x80) continue;
        if (chars++ == max_chars) { *p = 0; return max_chars; }
    }
    return chars;
}
