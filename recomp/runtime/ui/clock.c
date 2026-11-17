// The menu's clock (clock.h).
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdio.h>

#include "clock.h"

bool clock_format(char *out, unsigned long cap) {
    if (cap) out[0] = 0;
    SDL_Time now;
    SDL_DateTime dt;
    if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &dt, true)) return false;  // (local time: the zone is the system's)
    SDL_DateFormat df;
    SDL_TimeFormat tf = SDL_TIME_FORMAT_24HR;
    SDL_GetDateTimeLocalePreferences(&df, &tf);  // (on a failure it leaves 24-hour)
    if (tf == SDL_TIME_FORMAT_12HR) snprintf(out, cap, "%d:%02d %s", dt.hour % 12 ? dt.hour % 12 : 12, dt.minute, dt.hour < 12 ? "AM" : "PM");
    else snprintf(out, cap, "%02d:%02d", dt.hour, dt.minute);
    return true;
}
