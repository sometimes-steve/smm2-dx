// The Windows SDK's EventToken.h, which Microsoft's WebView2.h includes and zig's MinGW headers do not have: the one type it needs.
#pragma once
typedef struct EventRegistrationToken {
    __int64 value;
} EventRegistrationToken;
