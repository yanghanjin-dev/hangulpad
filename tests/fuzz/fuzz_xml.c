/* libFuzzer: XML parser + full traversal + every query function.
 * Build: make fuzz   Run: build/fuzz_xml -max_total_time=120 <corpus-dir> */
#include "xml.h"
#include <stdint.h>
#include <string.h>
static size_t sink;
static void walk(const hp_xml_node *scope, const hp_xml_node *n, int d) {
    const hp_xml_node *c;
    if (n->type == HP_XML_TEXT) { sink += n->text_len + (n->text ? (unsigned char)n->text[0] : 0); return; }
    sink += strlen(n->name) + n->attr_count + (n->end - n->start) + (n->inner_end - n->inner_start);
    if (n->attr_count) sink += strlen(n->attrs[0].value) + (size_t)hp_xml_attr_long(n, n->attrs[0].name, 0);
    sink += hp_xml_child(n, "p") != NULL; sink += hp_xml_next(n, NULL) != NULL;
    sink += hp_xml_find_after(scope, n, "t") != NULL;
    for (c = n->first_child; c; c = c->next) walk(scope, c, d + 1);
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    hp_xml_doc *d = NULL; char err[128];
    if (hp_xml_parse((const char *)data, size, &d, err, sizeof err) == HP_OK) {
        const hp_xml_node *r = hp_xml_root(d), *n;
        walk(r, r, 0);
        for (n = hp_xml_find(r, NULL); n; n = hp_xml_find_next(r, n, NULL)) sink++;
        hp_xml_free(d);
    }
    return 0;
}
