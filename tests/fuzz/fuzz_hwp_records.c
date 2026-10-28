/* libFuzzer: HWP record parser and OWPML conversion without the compound file and
 * deflate layers (build with -DHP_FUZZING). Input: u32 DocInfo length, DocInfo
 * records, section records — see tests/fuzz/make_hwp_record_seeds.py. */
#include <stddef.h>
#include <stdint.h>
int hp_hwp_fuzz_records(const unsigned char *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    return hp_hwp_fuzz_records(data, size);
}
