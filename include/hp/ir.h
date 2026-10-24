/* HangulPad internal document model (IR) */
#ifndef HP_IR_H
#define HP_IR_H

#include "hp/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hp_run {
    char *text; /* UTF-8; ownership by run */
} hp_run;

typedef struct hp_paragraph {
    hp_run *runs;
    size_t run_count;
} hp_paragraph;

typedef struct hp_page {
    uint32_t width;
    uint32_t height;
    uint32_t margin_left;
    uint32_t margin_right;
    uint32_t margin_top;
    uint32_t margin_bottom;
} hp_page;

typedef struct hp_section {
    hp_page page;
    hp_paragraph *paragraphs;
    size_t paragraph_count;
} hp_section;

typedef struct hp_document {
    char *title;
    char *language;
    hp_section *sections;
    size_t section_count;
} hp_document;

void hp_page_default(hp_page *page);
hp_document *hp_document_new(void);
hp_document *hp_document_from_text(const char *text);
void hp_document_free(hp_document *doc);

hp_status hp_document_set_title(hp_document *doc, const char *title);
hp_status hp_section_add_paragraph_text(hp_section *sec, const char *text);

/* Caller frees returned string with free(). */
char *hp_document_plain_text(const hp_document *doc);

#ifdef __cplusplus
}
#endif

#endif /* HP_IR_H */
