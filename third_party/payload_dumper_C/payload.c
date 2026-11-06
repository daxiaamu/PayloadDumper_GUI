/**
 * payload.c -- Protobuf-based payload.bin manifest parser
 *
 * Parses the DeltaArchiveManifest protobuf from a payload.bin file.
 * Uses a minimal wire-format parser - only reads fields we need.
 */
#include "payload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Simple protobuf wire reader.
 * Wire types: 0=varint, 1=64-bit, 2=length-delimited, 5=32-bit */

static uint64_t read_varint(const uint8_t **data, const uint8_t *end) {
    uint64_t result = 0;
    int shift = 0;
    int count = 0;
    while (*data < end && count < 10) {
        uint8_t byte = *(*data)++;
        if (count == 9 && (byte & 0xFE) != 0) {
            *data = end;
            return 0;
        }
        result |= (uint64_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) return result;
        shift += 7;
        count++;
    }
    *data = end;
    return 0;
}

static int read_key(const uint8_t **data, const uint8_t *end, int *field, int *wire) {
    const uint8_t *start = *data;
    uint64_t key = read_varint(data, end);
    if (*data <= start || key == 0 || key > UINT32_MAX) return -1;
    *field = (int)(key >> 3);
    *wire = (int)(key & 7);
    return *field > 0 ? 0 : -1;
}

static int skip_field(const uint8_t **data, const uint8_t *end, int wire_type) {
    switch (wire_type) {
    case 0: /* varint */
        while (*data < end && (*(*data) & 0x80)) (*data)++;
        if (*data < end) (*data)++;
        return 0;
    case 1: /* 64-bit */
        if (*data + 8 > end) return -1;
        *data += 8;
        return 0;
    case 2: /* length-delimited */
        if (*data >= end) return -1;
        {
            const uint8_t *saved = *data;
            uint64_t len = read_varint(data, end);
            if (len > (uint64_t)(end - *data)) { *data = saved; return -1; }
            *data += (size_t)len;
        }
        return 0;
    case 5: /* 32-bit */
        if (*data + 4 > end) return -1;
        *data += 4;
        return 0;
    default:
        return -1;
    }
}

/* Read a length-delimited string/bytes field value.
 * Returns pointer to data and length. */
static const uint8_t* read_length_delimited(const uint8_t **data, const uint8_t *end, size_t *len) {
    if (*data >= end) return NULL;
    uint64_t l = read_varint(data, end);
    if (l > (uint64_t)(end - *data) || l > SIZE_MAX) return NULL;
    *len = (size_t)l;
    const uint8_t *result = *data;
    *data += (size_t)l;
    return result;
}

/* Read a varint field value */
/* Parse a single Extent from protobuf data */
static int parse_extent(const uint8_t *data, size_t len, Extent *ext) {
    const uint8_t *end = data + len;
    memset(ext, 0, sizeof(*ext));
    while (data < end) {
        int field, wire;
        if (read_key(&data, end, &field, &wire) != 0) return -1;
        switch (field) {
        case 1: /* start_block: uint64 */
            if (wire != 0) { skip_field(&data, end, wire); break; }
            ext->start_block = read_varint(&data, end);
            break;
        case 2: /* num_blocks: uint64 */
            if (wire != 0) { skip_field(&data, end, wire); break; }
            ext->num_blocks = read_varint(&data, end);
            break;
        default:
            skip_field(&data, end, wire);
            break;
        }
    }
    return 0;
}

/* Parse a single InstallOperation */
static int parse_install_op(const uint8_t *data, size_t len, InstallOperation *op) {
    const uint8_t *end = data + len;
    memset(op, 0, sizeof(*op));
    while (data < end) {
        int field, wire;
        if (read_key(&data, end, &field, &wire) != 0) return -1;
        switch (field) {
        case 1: /* type: enum (varint) */
            if (wire != 0) { skip_field(&data, end, wire); break; }
            op->type = (InstallOpType)read_varint(&data, end);
            break;
        case 2: /* data_offset: uint64 */
            if (wire != 0) { skip_field(&data, end, wire); break; }
            op->data_offset = read_varint(&data, end);
            break;
        case 3: /* data_length: uint64 */
            if (wire != 0) { skip_field(&data, end, wire); break; }
            op->data_length = read_varint(&data, end);
            break;
        case 4: /* src_extents: repeated Extent */
            if (wire != 2) { skip_field(&data, end, wire); break; }
            {
                size_t sub_len;
                const uint8_t *sub = read_length_delimited(&data, end, &sub_len);
                if (sub) {
                    int old_n = op->num_src_extents;
                    if (old_n == INT_MAX) return -1;
                    int new_n = old_n + 1;
                    Extent *r = realloc(op->src_extents, new_n * sizeof(Extent));
                    if (!r) return -1;
                    op->src_extents = r;
                    if (parse_extent(sub, sub_len, &op->src_extents[old_n]) != 0) return -1;
                    op->num_src_extents = new_n;
                }
            }
            break;
        case 5: /* src_length: uint64 */
            if (wire != 0) { skip_field(&data, end, wire); break; }
            op->src_length = read_varint(&data, end);
            break;
        case 6: /* dst_extents: repeated Extent */
            if (wire != 2) { skip_field(&data, end, wire); break; }
            {
                size_t sub_len;
                const uint8_t *sub = read_length_delimited(&data, end, &sub_len);
                if (sub) {
                    int old_n = op->num_dst_extents;
                    if (old_n == INT_MAX) return -1;
                    int new_n = old_n + 1;
                    Extent *r = realloc(op->dst_extents, new_n * sizeof(Extent));
                    if (!r) return -1;
                    op->dst_extents = r;
                    if (parse_extent(sub, sub_len, &op->dst_extents[old_n]) != 0) return -1;
                    op->num_dst_extents = new_n;
                }
            }
            break;
        case 7: /* dst_length: uint64 */
            if (wire != 0) { skip_field(&data, end, wire); break; }
            op->dst_length = read_varint(&data, end);
            break;
        case 8: /* data_sha256_hash: bytes */
            if (wire != 2) { skip_field(&data, end, wire); break; }
            {
                size_t hlen;
                const uint8_t *h = read_length_delimited(&data, end, &hlen);
                if (h && hlen == 32) {
                    op->data_sha256 = malloc(32);
                    if (!op->data_sha256) return -1;
                    memcpy(op->data_sha256, h, 32);
                }
            }
            break;
        default:
            skip_field(&data, end, wire);
            break;
        }
    }
    return 0;
}

/* Parse repeated InstallOperation field */
/* Parse PartitionInfo */
static int parse_partition_info(const uint8_t *data, size_t len, PartitionInfo *info) {
    const uint8_t *end = data + len;
    memset(info, 0, sizeof(*info));
    while (data < end) {
        int field, wire;
        if (read_key(&data, end, &field, &wire) != 0) return -1;
        switch (field) {
        case 1: /* size: uint64 */
            if (wire == 0) info->size = read_varint(&data, end);
            else skip_field(&data, end, wire);
            break;
        case 2: /* hash: bytes */
            if (wire == 2) {
                size_t hlen;
                const uint8_t *h = read_length_delimited(&data, end, &hlen);
                if (h && hlen > 0) {
                    info->hash = malloc(hlen);
                    if (!info->hash || hlen > INT_MAX) return -1;
                    memcpy(info->hash, h, hlen);
                    info->hash_len = (int)hlen;
                }
            } else skip_field(&data, end, wire);
            break;
        default:
            skip_field(&data, end, wire);
            break;
        }
    }
    return 0;
}

/* Parse a single PartitionUpdate */
static int parse_partition_update(const uint8_t *data, size_t len, PartitionUpdate *pu) {
    const uint8_t *end = data + len;
    memset(pu, 0, sizeof(*pu));
    while (data < end) {
        int field, wire;
        if (read_key(&data, end, &field, &wire) != 0) return -1;
        switch (field) {
        case 1: /* partition_name: string */
            if (wire == 2) {
                size_t slen;
                const uint8_t *s = read_length_delimited(&data, end, &slen);
                if (s && slen > 0) {
                    if (slen == SIZE_MAX) return -1;
                    pu->partition_name = malloc(slen + 1);
                    if (!pu->partition_name) return -1;
                    memcpy(pu->partition_name, s, slen);
                    pu->partition_name[slen] = 0;
                }
            } else skip_field(&data, end, wire);
            break;
        case 6: /* old_partition_info: PartitionInfo */
            if (wire == 2) {
                size_t slen;
                const uint8_t *s = read_length_delimited(&data, end, &slen);
                if (s && parse_partition_info(s, slen, &pu->old_info) != 0) return -1;
            } else skip_field(&data, end, wire);
            break;
        case 7: /* new_partition_info: PartitionInfo */
            if (wire == 2) {
                size_t slen;
                const uint8_t *s = read_length_delimited(&data, end, &slen);
                if (s && parse_partition_info(s, slen, &pu->new_info) != 0) return -1;
            } else skip_field(&data, end, wire);
            break;
        case 8: { /* operations: repeated InstallOperation */
            if (wire == 2) {
                size_t slen;
                const uint8_t *sub = read_length_delimited(&data, end, &slen);
                if (sub) {
                    /* Each field 8 is one InstallOperation message */
                    int old_n = pu->num_operations;
                    if (old_n == INT_MAX) return -1;
                    int new_n = old_n + 1;
                    InstallOperation *tmp = realloc(pu->operations, new_n * sizeof(InstallOperation));
                    if (!tmp) return -1;
                    pu->operations = tmp;
                    memset(&pu->operations[old_n], 0, sizeof(InstallOperation));
                    pu->num_operations = new_n;
                    if (parse_install_op(sub, slen, &pu->operations[old_n]) != 0) return -1;
                }
            } else skip_field(&data, end, wire);
            break;
        }
        default:
            skip_field(&data, end, wire);
            break;
        }
    }
    return 0;
}

int parse_payload_metadata(const uint8_t *buf, size_t bufsize,
                           PayloadHeader *header, DeltaArchiveManifest *manifest,
                           uint64_t base_offset)
{
    size_t offset = 0;
    memset(header, 0, sizeof(*header));
    memset(manifest, 0, sizeof(*manifest));

    /* Read fixed header (big-endian fields) */
    if (bufsize < 20) return -1;
    memcpy(header->magic, buf, 4);
    if (memcmp(header->magic, PAYLOAD_MAGIC, 4) != 0) {
        fprintf(stderr, "Error: invalid payload magic\n");
        return -1;
    }
    /* Payload header uses big-endian byte order */
    {
        uint64_t ver_be, msz_be;
        memcpy(&ver_be, buf + 4, 8);
        memcpy(&msz_be, buf + 12, 8);
        /* Convert big-endian to host: byteswap on little-endian */
        header->file_format_version = __builtin_bswap64(ver_be);
        header->manifest_size = __builtin_bswap64(msz_be);
    }
    offset = 20;

    if (header->file_format_version > 1) {
        if (bufsize < 24) return -1;
        uint32_t msig_be;
        memcpy(&msig_be, buf + 20, 4);
        header->metadata_signature_size = __builtin_bswap32(msig_be);
        offset = 24;
    }

    if (header->manifest_size == 0 ||
        header->manifest_size > (uint64_t)(bufsize - offset)) {
        fprintf(stderr, "Error: manifest size out of bounds\n");
        return -1;
    }

    /* Parse manifest protobuf */
    const uint8_t *mdata = buf + offset;
    size_t msize = (size_t)header->manifest_size;
    const uint8_t *mend = mdata + msize;

    manifest->block_size = 4096; /* default */
    if (base_offset > UINT64_MAX - offset ||
        base_offset + offset > UINT64_MAX - msize ||
        base_offset + offset + msize > UINT64_MAX - header->metadata_signature_size) {
        fprintf(stderr, "Error: payload data offset overflow\n");
        return -1;
    }
    manifest->data_offset = base_offset + offset + msize + header->metadata_signature_size;

    int block_size_parsed = 0;

    /* We need to count PartitionUpdate messages first */
    int part_cap = 0, part_n = 0;
    PartitionUpdate *parts = NULL;

    while (mdata < mend) {
        int field_num, wire_type;
        if (read_key(&mdata, mend, &field_num, &wire_type) != 0) {
            manifest->partitions = parts;
            manifest->num_partitions = part_n;
            free_manifest(manifest);
            return -1;
        }

        switch (field_num) {
        case 3: /* block_size: uint32 */
            if (!block_size_parsed && wire_type == 0) {
                manifest->block_size = (uint32_t)read_varint(&mdata, mend);
                block_size_parsed = 1;
            } else {
                skip_field(&mdata, mend, wire_type);
            }
            break;
        case 12: /* minor_version: uint32 */
            if (wire_type == 0) manifest->minor_version = (uint32_t)read_varint(&mdata, mend);
            else skip_field(&mdata, mend, wire_type);
            break;
        case 13: /* partitions: repeated PartitionUpdate */
            if (wire_type == 2) {
                size_t slen;
                const uint8_t *sub = read_length_delimited(&mdata, mend, &slen);
                if (!sub) break;
                if (part_n >= part_cap) {
                    if (part_cap > INT_MAX / 2) {
                        manifest->partitions = parts;
                        manifest->num_partitions = part_n;
                        free_manifest(manifest);
                        return -1;
                    }
                    part_cap = part_cap ? part_cap * 2 : 16;
                    PartitionUpdate *tmp = realloc(parts, part_cap * sizeof(PartitionUpdate));
                    if (!tmp) {
                        manifest->partitions = parts;
                        manifest->num_partitions = part_n;
                        free_manifest(manifest);
                        return -1;
                    }
                    parts = tmp;
                }
                if (parse_partition_update(sub, slen, &parts[part_n]) != 0) {
                    manifest->partitions = parts;
                    manifest->num_partitions = part_n + 1;
                    free_manifest(manifest);
                    return -1;
                }
                part_n++;
            } else skip_field(&mdata, mend, wire_type);
            break;
        default:
            skip_field(&mdata, mend, wire_type);
            break;
        }
    }

    manifest->partitions = parts;
    manifest->num_partitions = part_n;
    return 0;
}

void free_manifest(DeltaArchiveManifest *manifest) {
    for (int i = 0; i < manifest->num_partitions; i++) {
        free(manifest->partitions[i].partition_name);
        free(manifest->partitions[i].old_info.hash);
        free(manifest->partitions[i].new_info.hash);
        for (int j = 0; j < manifest->partitions[i].num_operations; j++) {
            free(manifest->partitions[i].operations[j].src_extents);
            free(manifest->partitions[i].operations[j].dst_extents);
            free(manifest->partitions[i].operations[j].data_sha256);
        }
        free(manifest->partitions[i].operations);
    }
    free(manifest->partitions);
}
