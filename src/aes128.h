/* AES-128 block decryption (FIPS-197), for HWP distribution-only documents.
 * Internal header. */
#ifndef HP_AES128_H
#define HP_AES128_H

#include <stddef.h>
#include <stdint.h>

/* Decrypt `len` bytes in place in ECB mode; a trailing partial block is left as is. */
void hp_aes128_ecb_decrypt(const uint8_t key[16], uint8_t *data, size_t len);

#endif /* HP_AES128_H */
