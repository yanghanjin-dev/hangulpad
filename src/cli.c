#include "hp/hwp.h"
#include "hp/hwpx.h"
#include "hp/ir.h"
#include "hp/version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win_utf8.h"

static void usage(const char *argv0) {
    fprintf(stderr,
            "HangulPad " HP_VERSION_STRING " (" HP_VERSION_DATE ") engine CLI — HWPX first; HWP read stub\n"
            "Usage:\n"
            "  %s open <input>\n"
            "  %s new <output.hwpx> [--text TEXT] [--title TITLE]\n"
            "  %s save <input.hwpx|.hwp> <output.hwpx>  (lossless for .hwpx; .hwp is converted)\n"
            "  %s append <input.hwpx> <output.hwpx> --text TEXT [--text TEXT ...]\n"
            "  %s roundtrip <input.hwpx> <output.hwpx>\n"
            "  %s probe <input>\n"
            "  %s --version\n",
            argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

static const char *file_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : "";
}

static int streq_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static hp_status open_any(const char *path, hp_document **out) {
    const char *ext = file_ext(path);
    /* content first: HWPX packages are also published with a .hwp name */
    if (hp_looks_like_hwpx(path)) {
        return hp_hwpx_read(path, out);
    }
    if (hp_looks_like_hwp(path)) {
        return hp_hwp_read(path, out);
    }
    if (streq_ci(ext, "hwpx")) {
        return hp_hwpx_read(path, out);
    }
    if (streq_ci(ext, "hwp")) {
        return hp_hwp_read(path, out);
    }
    return HP_ERR_INVALID;
}

int main(int argc, char **argv) {
    hp_utf8_args(&argc, &argv); /* Windows: UTF-8 paths (Korean file names) */
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "version") == 0 || strcmp(argv[1], "-v") == 0) {
        printf("HangulPad %s (%s)\n", HP_VERSION_STRING, HP_VERSION_DATE);
        return 0;
    }

    if (strcmp(argv[1], "open") == 0) {
        hp_document *doc = NULL;
        hp_status st;
        char *text;
        if (argc != 3) {
            usage(argv[0]);
            return 1;
        }
        st = open_any(argv[2], &doc);
        if (st != HP_OK) {
            fprintf(stderr, "error: %s\n", hp_status_string(st));
            return 1;
        }
        text = hp_document_plain_text(doc);
        if (text) {
            puts(text);
            free(text);
        }
        hp_document_free(doc);
        return 0;
    }

    if (strcmp(argv[1], "new") == 0) {
        const char *output;
        const char *text = NULL;
        const char *title = NULL;
        hp_document *doc;
        int i;
        hp_status st;
        if (argc < 3) {
            usage(argv[0]);
            return 1;
        }
        output = argv[2];
        for (i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) {
                text = argv[++i];
            } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
                title = argv[++i];
            } else {
                usage(argv[0]);
                return 1;
            }
        }
        doc = text ? hp_document_from_text(text) : hp_document_new();
        if (!doc) {
            fprintf(stderr, "error: out of memory\n");
            return 1;
        }
        if (title) {
            hp_document_set_title(doc, title);
        }
        st = hp_hwpx_write(doc, output);
        hp_document_free(doc);
        if (st != HP_OK) {
            fprintf(stderr, "error: %s\n", hp_status_string(st));
            return 1;
        }
        fprintf(stderr, "wrote %s\n", output);
        return 0;
    }

    if (strcmp(argv[1], "save") == 0) {
        hp_document *doc = NULL;
        hp_status st;
        if (argc != 4) {
            usage(argv[0]);
            return 1;
        }
        st = open_any(argv[2], &doc);
        if (st != HP_OK) {
            fprintf(stderr, "error: %s\n", hp_status_string(st));
            return 1;
        }
        st = hp_hwpx_write(doc, argv[3]);
        hp_document_free(doc);
        if (st != HP_OK) {
            fprintf(stderr, "error: %s\n", hp_status_string(st));
            return 1;
        }
        fprintf(stderr, "saved %s\n", argv[3]);
        return 0;
    }

    if (strcmp(argv[1], "append") == 0) {
        hp_document *doc = NULL;
        hp_status st;
        hp_section *last;
        int i, added = 0;
        if (argc < 6) {
            usage(argv[0]);
            return 1;
        }
        st = hp_hwpx_read(argv[2], &doc);
        if (st != HP_OK) {
            fprintf(stderr, "error: %s\n", hp_status_string(st));
            return 1;
        }
        last = &doc->sections[doc->section_count - 1];
        for (i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) {
                st = hp_section_add_paragraph_text(last, argv[++i]);
                if (st != HP_OK) {
                    break;
                }
                added++;
            } else {
                hp_document_free(doc);
                usage(argv[0]);
                return 1;
            }
        }
        if (st == HP_OK) {
            st = hp_hwpx_write(doc, argv[3]);
        }
        hp_document_free(doc);
        if (st != HP_OK) {
            fprintf(stderr, "error: %s\n", hp_status_string(st));
            return 1;
        }
        fprintf(stderr, "appended %d paragraph(s) → %s\n", added, argv[3]);
        return 0;
    }

    if (strcmp(argv[1], "roundtrip") == 0) {
        hp_document *doc = NULL;
        hp_status st;
        char *text;
        if (argc != 4) {
            usage(argv[0]);
            return 1;
        }
        st = hp_hwpx_roundtrip(argv[2], argv[3], &doc);
        if (st != HP_OK) {
            fprintf(stderr, "error: %s\n", hp_status_string(st));
            return 1;
        }
        text = hp_document_plain_text(doc);
        if (text) {
            puts(text);
            free(text);
        }
        hp_document_free(doc);
        fprintf(stderr, "roundtrip ok → %s\n", argv[3]);
        return 0;
    }

    if (strcmp(argv[1], "probe") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            return 1;
        }
        if (hp_looks_like_hwpx(argv[2])) {
            puts("hwpx");
        } else if (hp_looks_like_hwp(argv[2])) {
            puts("hwp-cfb");
        } else {
            puts("unknown");
        }
        return 0;
    }

    usage(argv[0]);
    return 1;
}
