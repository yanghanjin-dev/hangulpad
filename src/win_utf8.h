/* HangulPad — UTF-8 command-line arguments on Windows (internal, header-only).
 *
 * The C runtime hands main() its arguments in the ANSI code page (CP949 on Korean
 * Windows), but the engine and libzip take UTF-8 paths. Call hp_utf8_args() first
 * thing in main(); it is a no-op on other platforms. Needs shell32 on Windows. */
#ifndef HP_WIN_UTF8_H
#define HP_WIN_UTF8_H

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>

static void hp_utf8_args(int *argc, char ***argv) {
    int n = 0, i;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    char **out;
    if (!wargv) {
        return;
    }
    out = (char **)calloc((size_t)n + 1, sizeof(char *));
    for (i = 0; out && i < n; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        out[i] = len > 0 ? (char *)malloc((size_t)len) : NULL;
        if (!out[i] || WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, out[i], len, NULL, NULL) <= 0) {
            out = NULL; /* keep the CRT arguments (process-lifetime allocation, not freed) */
            break;
        }
    }
    LocalFree(wargv);
    if (out) {
        *argc = n;
        *argv = out;
    }
    SetConsoleOutputCP(CP_UTF8); /* Korean text printed to the console */
}
#else
static void hp_utf8_args(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
}
#endif

#endif /* HP_WIN_UTF8_H */
