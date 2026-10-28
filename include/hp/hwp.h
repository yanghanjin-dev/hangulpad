/* HangulPad HWP 5.x codec (read) */
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
/* Read an HWP 5.x document. Password-protected and DRM documents return
 * HP_ERR_UNSUPPORTED. Distribution-only (배포용) documents are decrypted for
 * viewing and marked read-only (see hp_document_is_read_only). The document's package holds the
 * content converted to HWPX parts, so it can be saved with hp_hwpx_write(). */
hp_status hp_hwp_read(const char *path, hp_document **out_doc);
/* Same, from a memory image of the whole file (not copied, not modified). */
hp_status hp_hwp_read_memory(const unsigned char *data, size_t len, hp_document **out_doc);
/* 1 for a distribution-only document: view it, but do not edit or save it. */
int hp_document_is_read_only(const hp_document *doc);
/* Read .hwp or .hwpx, chosen by the file's content (not its name). */
hp_status hp_document_read(const char *path, hp_document **out_doc);
hp_status hp_hwp_write(const hp_document *doc, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* HP_HWP_H */
