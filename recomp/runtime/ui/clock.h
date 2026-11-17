// The real-world clock in the menu's footer (issue #56): the computer's local time, in the format Windows is set to.
#pragma once

// Writes the time of day, e.g. "3:07 PM" or "15:07" (the 12 / 24-hour choice and the time zone are the system's, read
// each call so a change in either shows at once). Returns false, and an empty string, when the system gives no time.
bool clock_format(char *out, unsigned long cap);
