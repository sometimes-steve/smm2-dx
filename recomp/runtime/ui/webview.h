// A web view over the game window (Edge WebView2, which comes with Windows): the game's Play Guide, an offline HTML manual from
// the player's own game files, is shown in it (issue #19). Closes by itself when the page ends (its back button) or on Esc / B.
#pragma once
#include <stdbool.h>

// Show the page (a file:// URL of the extracted manual). Any thread. Where the web view can't start (no WebView2 runtime,
// no loader DLL) the page is opened in the default browser instead. false: one is already up.
bool webview_open(const char *url);
bool webview_active(void);
// The game controller while it is up, as the menus read it (menu.c's A / B, swap included): the main loop's thread.
enum { WV_PAD_UP, WV_PAD_DOWN, WV_PAD_LEFT, WV_PAD_RIGHT, WV_PAD_CONFIRM, WV_PAD_BACK };
void webview_gamepad(int button, bool down);
