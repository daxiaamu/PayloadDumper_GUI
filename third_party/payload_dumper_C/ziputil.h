#ifndef ZIPUTIL_H
#define ZIPUTIL_H

#include <stdint.h>
#include <stddef.h>

/* Find a stored (uncompressed) entry in a ZIP file.
 * Returns the offset and size of the entry data.
 * Handles ZIP64 if needed.
 * Returns 0 on success, -1 if not found. */
int zip_find_stored_entry(const uint8_t *data, size_t datasize,
                          const char *entry_name,
                          uint64_t *entry_offset, uint64_t *entry_size);


/* Find stored entry via HTTP range reader callback */
int zip_find_stored_entry_http(uint64_t file_size,
                                int (*http_read_fn)(void *ctx, uint64_t offset, uint8_t *buf, size_t size),
                                void *http_ctx,
                                const char *entry_name,
                                uint64_t *out_offset, uint64_t *out_size);

#endif
