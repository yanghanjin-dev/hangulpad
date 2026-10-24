#include "hp/common.h"

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
