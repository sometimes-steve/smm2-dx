// UTF-8 / UTF-16 conversion and character-counted limits for the game's text prompts (runtime/hle/swkbd.c, runtime/ui/menu.c; issue #53).
// Kept apart from the HLE and the UI so recomp/tools/test_text_utf.py can build it on its own.
#pragma once
#include <stddef.h>
#include <stdint.h>

// UTF-16 (n units at most, stops at a NUL) -> UTF-8 in out (cap bytes, always NUL-terminated, never ends inside a character).
// A lone surrogate becomes U+FFFD.
void text_utf16_to_utf8(const uint16_t *s, uint32_t n, char *out, size_t cap);

// UTF-8 -> UTF-16 LE in out (cap units, room for the NUL is kept, always NUL-terminated, never ends inside a surrogate pair).
// Invalid UTF-8 becomes U+FFFD. Returns the units written, not counting the NUL.
size_t text_utf8_to_utf16(const char *s, uint16_t *out, size_t cap);

// Characters (code points) in a UTF-8 string
int text_utf8_chars(const char *s);

// Cut a UTF-8 string after max_chars characters, never inside one. Returns the characters kept.
int text_utf8_clip(char *s, int max_chars);
