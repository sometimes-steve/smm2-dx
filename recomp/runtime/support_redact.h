// Redaction of text for a support report (issue #55): what a player may send to someone else must not carry their name, their
// folders, their accounts' tokens or addresses. Pure C, no dependencies (tools/test_support_report.py builds and runs it on its own).
#pragma once
#include <stddef.h>

typedef struct {
    const char *find;  // a literal to take out wherever it appears (case does not matter: Windows paths); "" is ignored
    const char *with;  // what replaces it
} RedactLiteral;

// `in` (in_len bytes) -> `out` (NUL-terminated, at most cap - 1 bytes; the text is cut at a whole line when it would not fit), with
//   - each literal replaced by its label (the player's name, home folder, computer name, OpenCourse World's domain and token ...);
//   - a value after token / authorization / bearer / password / secret / api key / cookie replaced by <redacted>;
//   - an e-mail address, a long token-like string (32+ of letters, digits and -_+/=.), a Course ID / Maker ID (XXX-XXX-XXX) and an IPv4
//     address (but 127.0.0.1 and 0.0.0.0) replaced by a label.
// Returns the length written.
size_t support_redact(const char *in, size_t in_len, char *out, size_t cap, const RedactLiteral *lits, int nlits);
