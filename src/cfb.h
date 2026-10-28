/* HangulPad — read-only Compound File Binary (OLE2 / CFB) container.
 *
 * HWP 5.x documents are CFB files: a small FAT file system whose streams hold
 * FileHeader, DocInfo, BodyText/Section<n>, BinData/BIN<hex>.<ext> and so on.
 * Works on a memory image of the whole file; every sector index, chain and size is
 * checked so a damaged or hostile file fails cleanly instead of looping or reading
 * out of bounds. Internal header. */
#ifndef HP_CFB_H
#define HP_CFB_H

#include "hp/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hp_cfb hp_cfb;

/* data must stay valid while the hp_cfb is used (it is not copied). */
hp_status hp_cfb_open(const unsigned char *data, size_t len, hp_cfb **out);
void hp_cfb_close(hp_cfb *cfb);

/* Entry index for a '/'-separated path below the root ("BodyText/Section0"),
 * compared case-insensitively like CFB itself; -1 when missing. */
long hp_cfb_find(const hp_cfb *cfb, const char *path);
/* 1 for a stream, 0 otherwise (storage, root, bad index). */
int hp_cfb_is_stream(const hp_cfb *cfb, long entry);
/* Whole stream content in a malloc'd buffer (free with free()). */
hp_status hp_cfb_read(const hp_cfb *cfb, long entry, unsigned char **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* HP_CFB_H */
