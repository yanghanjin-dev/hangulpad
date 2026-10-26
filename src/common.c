#include "hp/common.h"
#include "hp/version.h"

#ifdef _WIN32
#include <windows.h>
#endif

FILE *hp_fopen(const char *utf8_path, const char *mode) {
#ifdef _WIN32
    wchar_t wpath[1024];
    wchar_t wmode[16];
    if (!utf8_path || !mode ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path, -1, wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) == 0 ||
        MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, (int)(sizeof(wmode) / sizeof(wmode[0]))) == 0) {
        return utf8_path && mode ? fopen(utf8_path, mode) : NULL; /* not UTF-8: let the CRT try */
    }
    return _wfopen(wpath, wmode);
#else
    return utf8_path && mode ? fopen(utf8_path, mode) : NULL;
#endif
}

const char *hp_version(void) {
    return HP_VERSION_STRING;
}

const char *hp_status_string(hp_status st) {
    switch (st) {
    case HP_OK:
        return "ok";
    case HP_ERR_IO:
        return "I/O error";
    case HP_ERR_NOMEM:
        return "out of memory";
    case HP_ERR_INVALID:
        return "invalid package";
    case HP_ERR_UNSUPPORTED:
        return "unsupported";
    case HP_ERR_NOT_HWPX:
        return "not HWPX";
    case HP_ERR_NOT_HWP:
        return "not HWP CFB";
    case HP_ERR_XML:
        return "XML error";
    default:
        return "unknown error";
    }
}
