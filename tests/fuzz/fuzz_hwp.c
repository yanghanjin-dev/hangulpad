/* libFuzzer: whole HWP 5.x reader (compound file, records, conversion, HWPX reader)
 * on an in-memory file image. Seed with small .hwp files. */
#include "hp/hwp.h"
#include "hp/ir.h"
#include <stdint.h>
#include <stdlib.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    hp_document *doc = NULL;
    if (hp_hwp_read_memory(data, size, &doc) == HP_OK) {
        char *t = hp_document_plain_text(doc);
        free(t);
        hp_document_free(doc);
    }
    return 0;
}
