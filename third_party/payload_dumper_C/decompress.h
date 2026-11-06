#ifndef DECOMPRESS_H
#define DECOMPRESS_H

#include <stdint.h>
#include <stddef.h>

/* Decompress data. Returns a malloc'd buffer on success, NULL on failure.
 * *out_len is set to the decompressed length. */
uint8_t* decompress_zstd(const uint8_t *in, size_t in_len, size_t *out_len);
uint8_t* decompress_brotli(const uint8_t *in, size_t in_len, size_t *out_len);
uint8_t* decompress_bz2(const uint8_t *in, size_t in_len, size_t *out_len);
uint8_t* decompress_xz(const uint8_t *in, size_t in_len, size_t *out_len);
uint8_t* decompress_lz4_legacy(const uint8_t *in, size_t in_len, size_t *out_len);

#endif
