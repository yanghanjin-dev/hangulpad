/* HangulPad HWP 5.x codec — Phase 1 read stub */
#ifndef HP_HWP_H
#define HP_HWP_H

#include "hp/common.h"
#include "hp/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

/* OLE2 / CFB magic used by HWP 5.x */
extern const unsigned char HP_CFB_MAGIC[8];

int hp_looks_like_hwp(const char *path);
hp_status hp_hwp_read(const char *path, hp_document **out_doc);
hp_status hp_hwp_write(const hp_document *doc, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* HP_HWP_H */
