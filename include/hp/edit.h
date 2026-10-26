#ifndef HP_EDIT_H
#define HP_EDIT_H
#include "hp/hwpx.h"
typedef struct hp_editor hp_editor;
hp_status hp_editor_open(const char *path, hp_editor **out);
void hp_editor_free(hp_editor *e);
size_t hp_editor_count(const hp_editor *e);
const char *hp_editor_text(const hp_editor *e, size_t i);
size_t hp_editor_section(const hp_editor *e, size_t i);
int hp_editor_is_deleted(const hp_editor *e, size_t i);
int hp_editor_can_delete(const hp_editor *e, size_t i);
hp_status hp_editor_set(hp_editor *e, size_t i, const char *text);
/* Insert after this paragraph and its nested contents; returns the new list index. */
hp_status hp_editor_insert(hp_editor *e, size_t i, size_t *inserted_index);
hp_status hp_editor_delete(hp_editor *e, size_t i);
hp_status hp_editor_save(hp_editor *e, const char *path);
#endif



