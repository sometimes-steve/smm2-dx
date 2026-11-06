// The exe players start: it runs smm2-dx_data\smm2-dx-runtime.exe, which sits beside its DLLs, Python, zig and
// everything it keeps (saves, settings, logs). Windows only loads a program's DLLs from its own folder, so the runtime
// lives in smm2-dx_data and this small program is all that is next to it. It has the same icon, passes its arguments
// on, waits for the runtime and returns its exit code.
#include <windows.h>
#include <wchar.h>

#define DIRNAME L"smm2-dx_data"
#define RUNTIME L"smm2-dx-runtime.exe"

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)inst; (void)prev; (void)cmd; (void)show;
    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (!n || n >= MAX_PATH) return 1;
    wchar_t *slash = wcsrchr(self, L'\\');
    if (slash) slash[1] = 0;
    wchar_t dir[MAX_PATH + 16], exe[MAX_PATH + 48];
    swprintf(dir, MAX_PATH + 16, L"%ls" DIRNAME, self);
    swprintf(exe, MAX_PATH + 48, L"%ls\\" RUNTIME, dir);
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"The \"" DIRNAME L"\" folder next to this program is missing.\n\n"
                          L"Keep this program and its \"" DIRNAME L"\" folder together.", L"smm2-dx", MB_ICONERROR);
        return 1;
    }
    // this program's own arguments, without its name (quoted or not), after the runtime's
    const wchar_t *a = GetCommandLineW();
    if (*a == L'"') { a++; while (*a && *a != L'"') a++; if (*a) a++; }
    else while (*a && *a != L' ' && *a != L'\t') a++;
    size_t len = wcslen(exe) + wcslen(a) + 8;
    wchar_t *line = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, len * sizeof(wchar_t));
    if (!line) return 1;
    swprintf(line, len, L"\"%ls\"%ls", exe, a);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, line, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        MessageBoxW(NULL, L"smm2-dx could not start.", L"smm2-dx", MB_ICONERROR);
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}
