#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

/* Compute SHA256 of data. Returns 0 on success, -1 on failure. */
int sha256_compute(const uint8_t *data, size_t len, uint8_t out[32]);

#endif
