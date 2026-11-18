// The support report (issue #55): one action that gathers what is needed to look into a crash or a graphics bug - the program's version,
// the PC, the settings that matter, the mods that were on, the logs of this and the last session and a failed session's crash log -
// with the player's name, folders and accounts taken out (support_redact.h). Local only: it is copied or written to a file by the
// player, and sent by the player, if at all.
#pragma once
#include <stdbool.h>
#include <stddef.h>

// The report's text (malloc'd, never NULL). `version`: the program's version (the menu's).
char *support_report_build(const char *version);
// On the clipboard (SDL). msg: what to tell the player either way.
bool support_report_copy(const char *version, char *msg, size_t cap);
// In the log folder as support-report-<date>.txt; path: where, or msg: why not.
bool support_report_export(const char *version, char *path, size_t path_cap, char *msg, size_t cap);

// Did the session before this one end on a fatal error (its log, now smm2-dx.previous.log, says so)? Known once the logs were moved.
bool support_last_run_failed(void);
// At the start, before the last session's log is moved aside (runtime/platform/windows.c): if it ended on a fatal error, a copy is
// kept as crash-<date>.log in the log folder (the newest five), so it outlives the three sessions the log rotation keeps.
void support_crash_keep(const char *log, const char *dir);
