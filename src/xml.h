/* HangulPad — small, dependency-free XML DOM parser (C11).
 *
 * Built for OWPML/HWPX parts:
 *  - UTF-8 input (a leading BOM is skipped); no DTD processing (DOCTYPE is skipped,
 *    so no entity expansion attacks), no external resources.
 *  - Decodes the 5 predefined entities and numeric references (&#10; &#xAC00;),
 *    CDATA sections; comments and processing instructions are skipped.
 *  - Namespaces: names keep their prefix (qname "hp:p"); lookups use the local
 *    name ("p"), matching how HWPX files are read in practice.
 *  - Every node remembers its byte span in the source, so writers can patch the
 *    original XML in place (lossless save).
 *  - Structure errors (mismatched/unclosed tags, bad syntax, too deep) fail the
 *    parse. Unknown named entities and stray '&' are kept literally (lenient).
 * Internal header. */
#ifndef HP_XML_H
#define HP_XML_H

#include "hp/common.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HP_XML_MAX_DEPTH 512

typedef enum hp_xml_type { HP_XML_ELEMENT = 0, HP_XML_TEXT = 1 } hp_xml_type;

typedef struct hp_xml_attribute {
    const char *qname; /* "hp:id" */
    const char *name;  /* local part ("id"), points into qname */
    const char *value; /* decoded */
} hp_xml_attribute;

typedef struct hp_xml_node {
    hp_xml_type type;
    const char *qname; /* elements: "hp:p" */
    const char *name;  /* elements: local name "p" */
    const char *text;  /* text nodes: decoded text (CDATA included) */
    size_t text_len;
    hp_xml_attribute *attrs;
    size_t attr_count;
    struct hp_xml_node *parent;
    struct hp_xml_node *first_child;
    struct hp_xml_node *last_child;
    struct hp_xml_node *next; /* next sibling */
    /* Byte offsets into the parsed buffer.
     * element: start = '<' of start tag, end = just past the end tag (or "/>"),
     *          inner_start = just past the start tag's '>', inner_end = '<' of end tag
     *          (inner_start == inner_end == end for empty-element tags).
     * text:    start..end = raw source range of this text run. */
    size_t start, end, inner_start, inner_end;
} hp_xml_node;

typedef struct hp_xml_doc hp_xml_doc;

/* Parse len bytes (need not be NUL-terminated). On failure *out = NULL and, if err
 * is given, a message with the byte offset is written there. */
hp_status hp_xml_parse(const char *buf, size_t len, hp_xml_doc **out, char *err, size_t err_size);
void hp_xml_free(hp_xml_doc *doc);
hp_xml_node *hp_xml_root(const hp_xml_doc *doc);

/* Attribute value by local name, or NULL. */
const char *hp_xml_attr(const hp_xml_node *el, const char *name);
/* Integer attributes are parsed as 64-bit on every platform (C `long` is only 32-bit
 * on Windows, which would clamp values such as 4294967295). */
long long hp_xml_attr_long(const hp_xml_node *el, const char *name, long long def);
unsigned long long hp_xml_attr_ulong(const hp_xml_node *el, const char *name, unsigned long long def);

/* Element navigation by local name (name == NULL matches any element). */
hp_xml_node *hp_xml_child(const hp_xml_node *el, const char *name);          /* first child */
hp_xml_node *hp_xml_next(const hp_xml_node *el, const char *name);           /* next sibling */
hp_xml_node *hp_xml_find(const hp_xml_node *el, const char *name);           /* first descendant */
/* Next element after `cur` in document order that is still inside `scope`
 * (cur's own subtree is entered first). Use with hp_xml_find to walk all matches. */
hp_xml_node *hp_xml_find_next(const hp_xml_node *scope, const hp_xml_node *cur, const char *name);
/* Like hp_xml_find_next but does not descend into cur's subtree. */
hp_xml_node *hp_xml_find_after(const hp_xml_node *scope, const hp_xml_node *cur, const char *name);

int hp_xml_is(const hp_xml_node *n, const char *name); /* element with this local name */

#ifdef __cplusplus
}
#endif

#endif /* HP_XML_H */
