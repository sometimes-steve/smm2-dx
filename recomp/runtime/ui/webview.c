// A web view over the game window (see webview.h). Edge WebView2 through its C interface (Microsoft's WebView2.h); the loader,
// WebView2Loader.dll, is loaded when it is first wanted, so a computer without it only loses this.
//
// One thread owns the view: WebView2 wants a single-threaded apartment with a message loop. It makes a borderless window owned by
// the game's (always above it, never in the taskbar), follows the game window's client area on a timer, and ends when the page goes
// to http://localhost/ (the page's own way to end the Switch's applet), on Esc, or on the controller's back button.
// The page is the game's own manual, not shown a Switch: the small script below stands in for the pieces of the Switch's browser it
// would use (the controller).
#define COBJMACROS
#include <windows.h>

#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "WebView2.h"
#include "webview.h"
#include "../paths.h"
#include "../rt.h"

typedef HRESULT(STDAPICALLTYPE *CreateEnvFn)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
                                              ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);

static atomic_bool g_active;
static HWND g_game, g_host;
static wchar_t g_url[1500];
static char g_url8[1500];  // (the same, for the browser when there is no web view)
static ICoreWebView2Controller *g_ctl;
static ICoreWebView2 *g_wv;
static ICoreWebView2Environment *g_env;
static bool g_failed;

#define WM_WV_PAD (WM_APP + 1)  // wParam: button, lParam: down
#define WM_WV_FAIL (WM_APP + 2)
#define WM_WV_END (WM_APP + 3)

// What the page needs of the controller and of the Switch's applet: D-pad as arrow keys (the pages' own handlers), A on the focused
// link, B / Esc the page's own back, and the page's end (its back button at the top) comes back as a navigation to http://localhost/.
static const wchar_t SHIM[] =
    L"(function(){"
    L"function back(){try{if(history.length>1&&typeof goBack==='function')goBack();else if(typeof endApplet==='function')endApplet();"
    L"else location.href='http://localhost/';}catch(e){location.href='http://localhost/';}}"
    L"function key(el,code){var e=new KeyboardEvent('keydown',{bubbles:true,cancelable:true});"
    L"Object.defineProperty(e,'keyCode',{get:function(){return code}});Object.defineProperty(e,'which',{get:function(){return code}});"
    L"el.dispatchEvent(e);}"
    L"window.__smm2pad=function(b,down){if(!down)return;"
    L"if(b==='back'){back();return;}"
    L"var el=document.activeElement;"
    L"if(!el||el===document.body){var f=document.querySelector('[tabindex=\"0\"]');if(f){f.focus();}return;}"
    L"if(b==='confirm'){el.click();return;}"
    L"key(el,{up:38,down:40,left:37,right:39}[b]);};"
    L"document.addEventListener('keydown',function(e){if(e.key==='Escape'){e.preventDefault();back();}});"
    // (the pages draw the buttons' icons as private-use characters of the console's NintendoExt font, which setup writes next to the
    // manual: the pages' fonts get it for those characters, four folders up from html/<language>/)
    L"(function(){var n=['SpecialFont','MessageFont'],l=['','-ja','-ko','-zh-cn','-zh-tw'],c='';"
    L"n.forEach(function(a){l.forEach(function(b){c+='@font-face{font-family:\"'+a+b+'\";"
    L"src:url(\"../../../../NintendoExt.ttf\");unicode-range:U+E000-F8FF}';});});"
    // (and the page's own default, for the elements that name no font: the back button's icon)
    L"c+='@font-face{font-family:\"NxGlyph\";src:url(\"../../../../NintendoExt.ttf\");unicode-range:U+E000-F8FF}body{font-family:\"NxGlyph\",serif}';"
    // (this script runs before the page has an <html>: what needs the page waits for it)
    L"document.addEventListener('DOMContentLoaded',function(){var s=document.createElement('style');s.textContent=c;document.head.appendChild(s);});})();"
    // (the Switch's cursor focuses what it is over, and the pages show a part's text when it is focused: a mouse hovers)
    L"document.addEventListener('mouseover',function(e){var a=e.target.closest&&e.target.closest('a.cursor');"
    L"if(a&&document.activeElement!==a)a.focus();});"
    L"})();";

static void to_wide(const char *s, wchar_t *out, int cap) {
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap)) out[0] = 0;
}

// ---- the COM handlers WebView2 calls back (static: one view at a time, so no counting)
static HRESULT STDMETHODCALLTYPE h_qi(void *This, REFIID riid, void **ppv) { (void)riid; *ppv = This; return S_OK; }
static ULONG STDMETHODCALLTYPE h_ref(void *This) { (void)This; return 1; }

static void fail(const char *why, HRESULT hr) {
    LOG("[webview] %s (%08lx): the page opens in the browser instead\n", why, (unsigned long)hr);
    g_failed = true;
    PostMessageW(g_host, WM_WV_FAIL, 0, 0);
}

static HRESULT STDMETHODCALLTYPE nav_invoke(ICoreWebView2NavigationStartingEventHandler *This, ICoreWebView2 *sender,
                                            ICoreWebView2NavigationStartingEventArgs *args) {
    (void)This, (void)sender;
    LPWSTR uri = NULL;
    if (SUCCEEDED(ICoreWebView2NavigationStartingEventArgs_get_Uri(args, &uri)) && uri) {
        bool ours = !_wcsnicmp(uri, L"file:///", 8);
        bool end = !_wcsnicmp(uri, L"http://localhost", 16);
        if (!ours) ICoreWebView2NavigationStartingEventArgs_put_Cancel(args, TRUE);  // (the game's pages only: no way out to the web)
        if (end) PostMessageW(g_host, WM_WV_END, 0, 0);
        CoTaskMemFree(uri);
    }
    return S_OK;
}
static ICoreWebView2NavigationStartingEventHandlerVtbl nav_vtbl = {(void *)h_qi, (void *)h_ref, (void *)h_ref, nav_invoke};
static ICoreWebView2NavigationStartingEventHandler nav_handler = {&nav_vtbl};

static void start_page(void) {
    ICoreWebView2_Navigate(g_wv, g_url);
    ICoreWebView2Controller_MoveFocus(g_ctl, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    LOG("[webview] showing the page\n");
}

static HRESULT STDMETHODCALLTYPE script_invoke(ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler *This, HRESULT hr,
                                               LPCWSTR id) {
    (void)This, (void)id;
    if (FAILED(hr)) LOG("[webview] the page script was not added (%08lx)\n", (unsigned long)hr);
    start_page();
    return S_OK;
}
static ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandlerVtbl script_vtbl = {(void *)h_qi, (void *)h_ref, (void *)h_ref,
                                                                                         script_invoke};
static ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler script_handler = {&script_vtbl};

static HRESULT STDMETHODCALLTYPE ctl_invoke(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, HRESULT hr,
                                            ICoreWebView2Controller *ctl) {
    (void)This;
    if (FAILED(hr) || !ctl) { fail("the web view could not be made", hr); return S_OK; }
    g_ctl = ctl;
    ICoreWebView2Controller_AddRef(ctl);
    ICoreWebView2Controller_get_CoreWebView2(ctl, &g_wv);
    RECT rc;
    GetClientRect(g_host, &rc);
    ICoreWebView2Controller_put_Bounds(ctl, rc);
    ICoreWebView2Controller_put_IsVisible(ctl, TRUE);
    ICoreWebView2Settings *st = NULL;
    if (SUCCEEDED(ICoreWebView2_get_Settings(g_wv, &st)) && st) {
        ICoreWebView2Settings_put_AreDefaultContextMenusEnabled(st, FALSE);
        ICoreWebView2Settings_put_AreDevToolsEnabled(st, FALSE);
        ICoreWebView2Settings_put_IsStatusBarEnabled(st, FALSE);
        ICoreWebView2Settings_put_IsZoomControlEnabled(st, FALSE);
        ICoreWebView2Settings_Release(st);
    }
    EventRegistrationToken tok;
    ICoreWebView2_add_NavigationStarting(g_wv, &nav_handler, &tok);
    // (the script is registered asynchronously: a page loaded before that is done would run without it, so the page waits for it)
    HRESULT sh = ICoreWebView2_AddScriptToExecuteOnDocumentCreated(g_wv, SHIM, &script_handler);
    if (FAILED(sh)) { LOG("[webview] the page script was not added (%08lx)\n", (unsigned long)sh); start_page(); }
    return S_OK;
}
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl ctl_vtbl = {(void *)h_qi, (void *)h_ref, (void *)h_ref, ctl_invoke};
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandler ctl_handler = {&ctl_vtbl};

static HRESULT STDMETHODCALLTYPE env_invoke(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This, HRESULT hr,
                                            ICoreWebView2Environment *env) {
    (void)This;
    if (FAILED(hr) || !env) { fail("the WebView2 runtime did not start", hr); return S_OK; }
    g_env = env;
    ICoreWebView2Environment_AddRef(env);
    hr = ICoreWebView2Environment_CreateCoreWebView2Controller(env, g_host, &ctl_handler);
    if (FAILED(hr)) fail("the web view could not be made", hr);
    return S_OK;
}
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl env_vtbl = {(void *)h_qi, (void *)h_ref, (void *)h_ref, env_invoke};
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler env_handler = {&env_vtbl};

// ---- the window: over the game window's client area
static bool game_area(RECT *r) {
    if (!g_game || !IsWindow(g_game) || IsIconic(g_game)) return false;
    RECT c;
    GetClientRect(g_game, &c);
    POINT p = {0, 0};
    ClientToScreen(g_game, &p);
    r->left = p.x, r->top = p.y, r->right = p.x + c.right, r->bottom = p.y + c.bottom;
    return c.right > 0 && c.bottom > 0;
}

static void follow_game(void) {
    RECT r;
    if (!game_area(&r)) { ShowWindow(g_host, SW_HIDE); return; }
    RECT now;
    GetWindowRect(g_host, &now);
    if (!IsWindowVisible(g_host)) ShowWindow(g_host, SW_SHOWNA);
    if (memcmp(&now, &r, sizeof r)) {
        SetWindowPos(g_host, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_ctl) {
            RECT b = {0, 0, r.right - r.left, r.bottom - r.top};
            ICoreWebView2Controller_put_Bounds(g_ctl, b);
        }
    }
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_TIMER: follow_game(); return 0;
    case WM_WV_PAD: {
        static const char *const names[] = {"up", "down", "left", "right", "confirm", "back"};
        if (g_wv && wp < 6) {
            wchar_t js[96];
            swprintf(js, sizeof js / sizeof *js, L"window.__smm2pad&&window.__smm2pad('%hs',%d)", names[wp], lp ? 1 : 0);
            ICoreWebView2_ExecuteScript(g_wv, js, NULL);
        }
        return 0;
    }
    case WM_WV_END: case WM_WV_FAIL: DestroyWindow(w); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    }
    return DefWindowProcW(w, m, wp, lp);
}

static DWORD WINAPI view_thread(void *unused) {
    (void)unused;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"smm2dx_webview";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);
    RECT r = {0, 0, 640, 360};
    game_area(&r);
    g_host = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top, g_game,
                             NULL, inst, NULL);
    g_failed = false;
    g_ctl = NULL, g_wv = NULL, g_env = NULL;
    HMODULE loader = LoadLibraryW(L"WebView2Loader.dll");
    CreateEnvFn create = loader ? (CreateEnvFn)GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions") : NULL;
    bool loop = true;
    if (!g_host || !create) {
        LOG("[webview] %s: the page opens in the browser instead\n", !g_host ? "no window" : "WebView2Loader.dll is missing");
        g_failed = true;
        loop = false;  // (nothing will end a message loop: the window goes now)
        if (g_host) DestroyWindow(g_host);
    } else {
        ShowWindow(g_host, SW_SHOW);
        SetTimer(g_host, 1, 50, NULL);
        wchar_t udf[1100], dir[1000];
        to_wide(rt_data_root(), dir, 1000);
        swprintf(udf, sizeof udf / sizeof *udf, L"%ls\\webview2", dir);
        HRESULT hr = create(NULL, udf, NULL, &env_handler);
        if (FAILED(hr)) fail("the WebView2 runtime was not found", hr);
    }
    if (loop) {
        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (g_ctl) { ICoreWebView2Controller_Close(g_ctl); ICoreWebView2Controller_Release(g_ctl); }
    if (g_env) ICoreWebView2Environment_Release(g_env);
    g_ctl = NULL, g_wv = NULL, g_env = NULL, g_host = NULL;
    UnregisterClassW(wc.lpszClassName, inst);
    if (g_failed) SDL_OpenURL(g_url8);
    if (g_game && IsWindow(g_game)) SetForegroundWindow(g_game);
    CoUninitialize();
    atomic_store(&g_active, false);
    return 0;
}

bool webview_active(void) { return atomic_load(&g_active); }

bool webview_open(const char *url) {
    if (atomic_exchange(&g_active, true)) return false;
    int n = 0;
    SDL_Window **ws = SDL_GetWindows(&n);
    g_game = NULL;
    if (ws && n > 0) g_game = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(ws[0]), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    SDL_free(ws);
    snprintf(g_url8, sizeof g_url8, "%s", url);
    to_wide(url, g_url, (int)(sizeof g_url / sizeof *g_url));
    HANDLE t = CreateThread(NULL, 0, view_thread, NULL, 0, NULL);
    if (!t) { atomic_store(&g_active, false); SDL_OpenURL(url); return true; }
    CloseHandle(t);
    return true;
}

void webview_gamepad(int button, bool down) {
    if (atomic_load(&g_active) && g_host) PostMessageW(g_host, WM_WV_PAD, (WPARAM)button, down ? 1 : 0);
}
