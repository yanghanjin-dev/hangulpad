/* Unit tests for src/xml.c */
#include "../src/xml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define EXPECT(cond, msg)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                        \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static hp_xml_doc *parse(const char *s, hp_status *st) {
    hp_xml_doc *d = NULL;
    char err[200];
    *st = hp_xml_parse(s, strlen(s), &d, err, sizeof(err));
    return d;
}

static int parses(const char *s) {
    hp_status st;
    hp_xml_doc *d = parse(s, &st);
    hp_xml_free(d);
    return st == HP_OK;
}

/* concatenated text of all text-node children */
static void child_text(const hp_xml_node *el, char *out, size_t n) {
    const hp_xml_node *c;
    out[0] = '\0';
    for (c = el->first_child; c; c = c->next) {
        if (c->type == HP_XML_TEXT && strlen(out) + c->text_len < n) {
            strcat(out, c->text);
        }
    }
}

static void test_basic(void) {
    const char *src = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<!-- lead --><a x='1' y=\"&lt;2&gt; &amp; &quot;q&quot; &apos;\">"
                      "<b/>t&amp;x<![CDATA[<raw>&amp;]]><!-- c --><c>&#xAC00;&#44032;&#x1F600;</c></a>\n";
    hp_status st;
    hp_xml_doc *d = parse(src, &st);
    hp_xml_node *a, *b, *c;
    char buf[128];
    EXPECT(st == HP_OK && d, "basic parse");
    if (!d) {
        return;
    }
    a = hp_xml_root(d);
    EXPECT(hp_xml_is(a, "a"), "root a");
    EXPECT(strcmp(hp_xml_attr(a, "x"), "1") == 0, "single-quoted attr");
    EXPECT(strcmp(hp_xml_attr(a, "y"), "<2> & \"q\" '") == 0, "entities in attr");
    EXPECT(hp_xml_attr(a, "z") == NULL, "missing attr");
    EXPECT(hp_xml_attr_long(a, "x", 9) == 1 && hp_xml_attr_long(a, "z", 9) == 9, "attr_long");
    b = hp_xml_child(a, "b");
    EXPECT(b && b->first_child == NULL && b->inner_start == b->end, "empty element");
    child_text(a, buf, sizeof(buf));
    EXPECT(strcmp(buf, "t&x<raw>&amp;") == 0, "text + CDATA (CDATA not decoded)");
    c = hp_xml_child(a, "c");
    child_text(c, buf, sizeof(buf));
    EXPECT(strcmp(buf, "\xEA\xB0\x80\xEA\xB0\x80\xF0\x9F\x98\x80") == 0, "numeric char refs -> UTF-8");
    /* spans */
    EXPECT(strncmp(src + a->start, "<a x=", 5) == 0, "span start");
    EXPECT(strncmp(src + a->end - 4, "</a>", 4) == 0, "span end");
    EXPECT(strncmp(src + c->inner_start, "&#xAC00;", 8) == 0 && strncmp(src + c->inner_end, "</c>", 4) == 0,
           "inner span");
    EXPECT(strncmp(src + b->start, "<b/>", 4) == 0 && b->end - b->start == 4, "empty element span");
    hp_xml_free(d);
}

static void test_namespaces_and_find(void) {
    const char *src = "<hs:sec xmlns:hs=\"s\" xmlns:hp=\"p\">"
                      "<hp:p hp:id=\"1\"><hp:run><hp:t>A</hp:t><hp:tbl><hp:tr><hp:tc><hp:p id=\"2\"><hp:run>"
                      "<hp:t>B</hp:t></hp:run></hp:p></hp:tc></hp:tr></hp:tbl></hp:run></hp:p>"
                      "<hp:p id=\"3\"><hp:run><hp:t>C</hp:t></hp:run></hp:p></hs:sec>";
    hp_status st;
    hp_xml_doc *d = parse(src, &st);
    hp_xml_node *root, *p1, *t, *p3, *x;
    int count = 0;
    EXPECT(st == HP_OK, "ns parse");
    if (!d) {
        return;
    }
    root = hp_xml_root(d);
    EXPECT(strcmp(root->qname, "hs:sec") == 0 && strcmp(root->name, "sec") == 0, "qname/local");
    p1 = hp_xml_child(root, "p");
    EXPECT(p1 && strcmp(hp_xml_attr(p1, "id"), "1") == 0, "prefixed attr by local name");
    p3 = hp_xml_next(p1, "p");
    EXPECT(p3 && strcmp(hp_xml_attr(p3, "id"), "3") == 0, "next sibling p");
    for (t = hp_xml_find(root, "t"); t; t = hp_xml_find_next(root, t, "t")) {
        count++;
    }
    EXPECT(count == 3, "find/find_next visits all t in order");
    x = hp_xml_find(p1, "p");
    EXPECT(x && strcmp(hp_xml_attr(x, "id"), "2") == 0, "find descends (nested p)");
    x = hp_xml_find_after(root, hp_xml_find(p1, "tbl"), "t");
    EXPECT(x && x->first_child && strcmp(x->first_child->text, "C") == 0, "find_after skips subtree");
    x = hp_xml_find_after(p1, hp_xml_find(p1, "tbl"), "t");
    EXPECT(x == NULL, "find_after stays inside scope");
    hp_xml_free(d);
}

static void test_whitespace(void) {
    hp_status st;
    hp_xml_doc *d = parse("\xEF\xBB\xBF<a v=\"x\ty\nz\r\nw\">l1\r\nl2\rl3</a>", &st);
    char buf[64];
    EXPECT(st == HP_OK, "BOM + CRLF parse");
    if (!d) {
        return;
    }
    EXPECT(strcmp(hp_xml_attr(hp_xml_root(d), "v"), "x y z w") == 0, "attr whitespace normalized");
    child_text(hp_xml_root(d), buf, sizeof(buf));
    EXPECT(strcmp(buf, "l1\nl2\nl3") == 0, "line ends normalized");
    hp_xml_free(d);
}

static void test_lenient_entities(void) {
    hp_status st;
    hp_xml_doc *d = parse("<a>R&D &unknown; &#xD800; &#; &#99999999;</a>", &st);
    char buf[64];
    EXPECT(st == HP_OK, "lenient entity parse");
    if (!d) {
        return;
    }
    child_text(hp_xml_root(d), buf, sizeof(buf));
    EXPECT(strcmp(buf, "R&D &unknown; &#xD800; &#; &#99999999;") == 0, "unknown/invalid refs kept literally");
    hp_xml_free(d);
}

static void test_doctype_safe(void) {
    /* entity definitions are never expanded (no billion-laughs) */
    hp_status st;
    hp_xml_doc *d = parse("<!DOCTYPE a [<!ENTITY x \"<b>boom</b>\"> <!ENTITY y \"&x;&x;\">]><a>&y;</a>", &st);
    char buf[64];
    EXPECT(st == HP_OK, "DOCTYPE skipped");
    if (!d) {
        return;
    }
    child_text(hp_xml_root(d), buf, sizeof(buf));
    EXPECT(strcmp(buf, "&y;") == 0, "entity not expanded");
    EXPECT(hp_xml_root(d)->first_child && !hp_xml_child(hp_xml_root(d), "b"), "no injected elements");
    hp_xml_free(d);
}

static void test_errors(void) {
    char deep[HP_XML_MAX_DEPTH * 8 + 64];
    size_t i, n = 0;
    EXPECT(!parses(""), "empty input");
    EXPECT(!parses("   "), "whitespace only");
    EXPECT(!parses("<a><b></a></b>"), "mismatched tags");
    EXPECT(!parses("<a><b>"), "unclosed");
    EXPECT(!parses("<a/><b/>"), "two roots");
    EXPECT(!parses("<a/>junk"), "text after root");
    EXPECT(!parses("junk<a/>"), "text before root");
    EXPECT(!parses("<a x=\"1></a>"), "unterminated attr");
    EXPECT(!parses("<a x=1></a>"), "unquoted attr");
    EXPECT(!parses("<a x></a>"), "attr without value");
    EXPECT(!parses("<a x=\"1\"y=\"2\"></a>"), "attrs without space");
    EXPECT(!parses("<></>"), "missing name");
    EXPECT(!parses("<a><!-- x </a>"), "unterminated comment");
    EXPECT(!parses("<a><![CDATA[x</a>"), "unterminated CDATA");
    EXPECT(!parses("<a></a "), "malformed end tag");
    EXPECT(!parses("</a>"), "end tag first");
    EXPECT(!parses("<a"), "truncated start tag");
    EXPECT(!parses("<a x=\"<\"/>"), "'<' in attribute value");
    EXPECT(parses("<a></a >"), "end tag with trailing space ok");
    for (i = 0; i < HP_XML_MAX_DEPTH + 1; i++) {
        n += (size_t)sprintf(deep + n, "<d>");
    }
    EXPECT(!parses(deep), "too deep");
    n = 0;
    for (i = 0; i < HP_XML_MAX_DEPTH; i++) {
        n += (size_t)sprintf(deep + n, "<d>");
    }
    for (i = 0; i < HP_XML_MAX_DEPTH; i++) {
        n += (size_t)sprintf(deep + n, "</d>");
    }
    EXPECT(parses(deep), "max depth ok");
}

static void test_not_nul_terminated(void) {
    /* parser must respect len, not stop at/rely on NUL */
    char buf[16];
    hp_xml_doc *d = NULL;
    memcpy(buf, "<a>xy</a>JUNK", 13);
    EXPECT(hp_xml_parse(buf, 9, &d, NULL, 0) == HP_OK, "len-bounded parse");
    hp_xml_free(d);
    d = NULL;
    EXPECT(hp_xml_parse(buf, 6, &d, NULL, 0) != HP_OK && d == NULL, "truncated by len fails");
}

/* C long is 32-bit on Windows: big HWPUNIT values must not be clamped there. */
static void test_64bit_attrs(void) {
    hp_status st;
    hp_xml_doc *d = parse("<a u=\"4294967295\" n=\"-1\" big=\"4294966796\"/>", &st);
    EXPECT(st == HP_OK && d, "64-bit attrs parse");
    if (!d) {
        return;
    }
    EXPECT(hp_xml_attr_long(hp_xml_root(d), "u", 0) == 4294967295LL, "attr_long beyond 32-bit");
    EXPECT((unsigned)hp_xml_attr_long(hp_xml_root(d), "u", 0) == 4294967295u, "uint32 HWPUNIT round trip");
    EXPECT(hp_xml_attr_long(hp_xml_root(d), "n", 0) == -1, "negative");
    EXPECT(hp_xml_attr_ulong(hp_xml_root(d), "big", 0) == 4294966796ULL, "attr_ulong");
    hp_xml_free(d);
}

static void test_many_attrs(void) {
    char src[4096];
    size_t n = 0;
    int i;
    hp_status st;
    hp_xml_doc *d;
    n += (size_t)sprintf(src + n, "<a");
    for (i = 0; i < 100; i++) {
        n += (size_t)sprintf(src + n, " k%d=\"%d\"", i, i);
    }
    sprintf(src + n, "/>");
    d = parse(src, &st);
    EXPECT(st == HP_OK && d && hp_xml_root(d)->attr_count == 100, "100 attributes");
    EXPECT(d && hp_xml_attr_long(hp_xml_root(d), "k99", -1) == 99, "attr 99");
    hp_xml_free(d);
}

int main(void) {
    test_basic();
    test_namespaces_and_find();
    test_whitespace();
    test_lenient_entities();
    test_doctype_safe();
    test_errors();
    test_not_nul_terminated();
    test_many_attrs();
    test_64bit_attrs();
    if (failures) {
        fprintf(stderr, "%d xml test(s) failed\n", failures);
        return 1;
    }
    puts("xml tests passed");
    return 0;
}
