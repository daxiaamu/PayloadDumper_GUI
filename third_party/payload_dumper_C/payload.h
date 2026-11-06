#ifndef PAYLOAD_H
#define PAYLOAD_H

#include <stdint.h>
#include <stddef.h>

#define PAYLOAD_MAGIC "CrAU"

/* Payload header (binary) */
typedef struct {
    char     magic[4];           /* "CrAU" */
    uint64_t file_format_version;
    uint64_t manifest_size;
    uint32_t metadata_signature_size; /* only if version >= 2 */
} __attribute__((packed)) PayloadHeader;

/* Install operation types (from proto) */
typedef enum {
    OP_REPLACE      = 0,
    OP_REPLACE_BZ   = 1,
    OP_MOVE         = 2,  /* deprecated */
    OP_BSDIFF       = 3,  /* deprecated */
    OP_SOURCE_COPY  = 4,
    OP_SOURCE_BSDIFF = 5,
    OP_ZERO         = 6,
    OP_DISCARD      = 7,
    OP_REPLACE_XZ   = 8,
    OP_PUFFDIFF     = 9,
    OP_BROTLI_BSDIFF = 10,
    OP_ZUCCHINI     = 11,
    OP_LZ4DIFF_BSDIFF = 12,
    OP_LZ4DIFF_PUFFDIFF = 13,
    OP_ZSTD         = 14,
} InstallOpType;

/* Extent (from proto) */
typedef struct {
    uint64_t start_block;
    uint64_t num_blocks;
} Extent;

/* Single install operation */
typedef struct {
    InstallOpType type;
    uint64_t data_offset;   /* offset in payload blob section */
    uint64_t data_length;
    Extent   *src_extents;
    int       num_src_extents;
    uint64_t  src_length;
    Extent   *dst_extents;
    int       num_dst_extents;
    uint64_t  dst_length;
    uint8_t  *data_sha256;  /* 32 bytes or NULL */
} InstallOperation;

/* Partition info */
typedef struct {
    uint64_t size;
    uint8_t *hash;          /* SHA256 or NULL */
    int      hash_len;
} PartitionInfo;

/* One partition update */
typedef struct {
    char          *partition_name;
    PartitionInfo  old_info;
    PartitionInfo  new_info;
    InstallOperation *operations;
    int            num_operations;
} PartitionUpdate;

/* Parsed manifest */
typedef struct {
    uint32_t block_size;          /* usually 4096 */
    uint32_t minor_version;
    int      num_partitions;
    PartitionUpdate *partitions;
    uint64_t data_offset;         /* offset of blob data in file */
} DeltaArchiveManifest;

/* Parse payload header + manifest from buffer.
 * Returns 0 on success, -1 on error. */
int parse_payload_metadata(const uint8_t *buf, size_t bufsize,
                           PayloadHeader *header, DeltaArchiveManifest *manifest,
                           uint64_t base_offset);

/* Free manifest internals */
void free_manifest(DeltaArchiveManifest *manifest);

#endif
