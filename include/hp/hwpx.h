/* HangulPad HWPX (OWPML) codec */
#ifndef HP_HWPX_H
#define HP_HWPX_H

#include "hp/common.h"
#include "hp/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HP_HWPX_MIMETYPE "application/hwp+zip"

int hp_looks_like_hwpx(const char *path);
hp_status hp_hwpx_read(const char *path, hp_document **out_doc);
hp_status hp_hwpx_write(const hp_document *doc, const char *path);
hp_status hp_hwpx_roundtrip(const char *input, const char *output, hp_document **out_doc);

#ifdef __cplusplus
}
#endif

#endif /* HP_HWPX_H */
