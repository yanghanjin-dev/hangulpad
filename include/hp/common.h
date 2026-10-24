/* HangulPad common types — C11 */
#ifndef HP_COMMON_H
#define HP_COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hp_status {
    HP_OK = 0,
    HP_ERR_IO = 1,
    HP_ERR_NOMEM = 2,
    HP_ERR_INVALID = 3,
    HP_ERR_UNSUPPORTED = 4,
    HP_ERR_NOT_HWPX = 5,
    HP_ERR_NOT_HWP = 6,
    HP_ERR_XML = 7
} hp_status;

const char *hp_status_string(hp_status st);

#ifdef __cplusplus
}
#endif

#endif /* HP_COMMON_H */
