/**
 * dumper.c -- Core extraction logic
 *
 * Flow:
 * 1. Open file (or URL via WinHTTP)
 * 2. If ZIP, find payload.bin offset
 * 3. Parse payload header + manifest protobuf
 * 4. Submit all partition operations to thread pool (full parallelism)
 */
#include "dumper.h"
#include "payload.h"
#include "ziputil.h"
#include "decompress.h"
#include "threadpool.h"
#include "http_reader.h"
#include "sha256.h"


/* Forward declarations for file mapping */
static uint8_t* map_file_read(const char *path, size_t *out_size, HANDLE *out_map);
static void unmap_file(uint8_t *buf, HANDLE hMap);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* Convert UTF-8 path to wide string for Windows API */
static wchar_t* utf8_to_wide(const char *utf8) {
    if (!utf8) return NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (len <= 0) {
        fprintf(stderr, "Error: MultiByteToWideChar failed for UTF-8 input\n");
        return NULL;
    }
    wchar_t *out = malloc(len * sizeof(wchar_t));
    if (out) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, len);
    return out;
}

static int is_path_sep(char ch) {
    return ch == '\\' || ch == '/';
}

static int build_output_path(char *buf, size_t buf_size,
                             const char *out_dir, const char *name) {
    size_t dir_len = out_dir ? strlen(out_dir) : 0;
    const char *sep = (dir_len > 0 && is_path_sep(out_dir[dir_len - 1])) ? "" : "\\";
    int len = snprintf(buf, buf_size, "%s%s%s", out_dir ? out_dir : "", sep, name);
    return (len < 0 || (size_t)len >= buf_size) ? -1 : 0;
}

static int ensure_output_dir(const char *out_dir) {
    wchar_t *path = utf8_to_wide(out_dir);
    DWORD attrs;
    int unc_path;
    int sep_count = 0;

    if (!path)
        return -1;

    unc_path = path[0] == L'\\' && path[1] == L'\\';
    for (wchar_t *p = path; *p; p++) {
        if (*p != L'\\' && *p != L'/')
            continue;
        sep_count++;

        /* Do not try to create roots like \, C:\, or \\server\share\. */
        if (p == path || (p == path + 2 && path[1] == L':') ||
            (unc_path && sep_count <= 3)) {
            continue;
        }

        wchar_t saved = *p;
        *p = L'\0';
        if (path[0] != L'\0' && !CreateDirectoryW(path, NULL)) {
            DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS) {
                free(path);
                return -1;
            }
        }
        *p = saved;
    }

    if (!CreateDirectoryW(path, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            free(path);
            return -1;
        }
    }

    attrs = GetFileAttributesW(path);
    free(path);
    return (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) ? 0 : -1;
}

static double now_seconds(void) {
    static LARGE_INTEGER freq;
    static int have_freq = 0;
    LARGE_INTEGER counter;
    if (!have_freq) {
        QueryPerformanceFrequency(&freq);
        have_freq = 1;
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
}

static void print_ops_timing(int ops, double seconds) {
    double rate;
    if (seconds < 0.000001)
        seconds = 0.000001;
    rate = ops / seconds;
    if (seconds < 10.0)
        printf("Processed %d ops in %.3fs (%.1f ops/s)\n", ops, seconds, rate);
    else
        printf("Processed %d ops in %.2fs (%.1f ops/s)\n", ops, seconds, rate);
}

/* Context for one partition: file handle + lock + name */
typedef struct {
    HANDLE      hOut;
    HANDLE      hMapOut;
    uint8_t    *out_data;
    size_t      map_size;
    CRITICAL_SECTION lock;
    char        name[256];
    uint64_t    total_size;
} PartCtx;

/* Flat job list entry: which partition + which operation */
typedef struct {
    int part_idx;
    int op_idx;
} JobEntry;

/* Shared context passed to all threadpool tasks */
typedef struct {
    const uint8_t     *file_data;
    size_t             file_size;
    uint64_t           data_off;      /* manifest->data_offset (absolute offset in file) */
    uint64_t           payload_end;
    HttpReader        *http;
    uint32_t           block_size;
    int                num_parts;
    PartitionUpdate   *parts;         /* partition array from manifest */
    PartCtx           *part_ctx;      /* per-partition context */
    int                num_jobs;
    JobEntry          *jobs;
    volatile LONG      ops_done;
    volatile LONG      failed;
    int                num_partitions_selected;
} DumpCtx;

static void mark_failed(DumpCtx *ctx) {
    InterlockedExchange(&ctx->failed, 1);
}

static int map_output_file(PartCtx *pc) {
    if (!pc || !pc->hOut || pc->total_size == 0 || pc->total_size > (uint64_t)SIZE_MAX)
        return 0;

    DWORD size_high = (DWORD)(pc->total_size >> 32);
    DWORD size_low = (DWORD)(pc->total_size & 0xFFFFFFFFu);
    HANDLE hMap = CreateFileMappingW(pc->hOut, NULL, PAGE_READWRITE,
                                     size_high, size_low, NULL);
    if (!hMap)
        return 0;

    uint8_t *mapped = (uint8_t*)MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE,
                                              0, 0, 0);
    if (!mapped) {
        CloseHandle(hMap);
        return 0;
    }

    pc->hMapOut = hMap;
    pc->out_data = mapped;
    pc->map_size = (size_t)pc->total_size;
    return 0;
}

static void close_part_ctx(PartCtx *pc) {
    if (!pc || !pc->hOut)
        return;
    if (pc->out_data) {
        UnmapViewOfFile(pc->out_data);
        pc->out_data = NULL;
    }
    if (pc->hMapOut) {
        CloseHandle(pc->hMapOut);
        pc->hMapOut = NULL;
    }
    CloseHandle(pc->hOut);
    pc->hOut = NULL;
    DeleteCriticalSection(&pc->lock);
}

static int add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) return -1;
    *out = a + b;
    return 0;
}

static int mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0 && b > UINT64_MAX / a) return -1;
    *out = a * b;
    return 0;
}

static int valid_partition_name(const char *name) {
    if (!name || !name[0]) return 0;
    size_t len = strlen(name);
    if (len > 127 || name[0] == '.' || name[len - 1] == '.') return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

static int partition_selected(const char *filter, const char *name) {
    if (!filter || !filter[0]) return 1;
    size_t name_len = strlen(name);
    const char *p = filter;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && end[-1] == ' ') end--;
        size_t len = (size_t)(end - start);
        if (len == name_len && _strnicmp(start, name, len) == 0) return 1;
        if (*p == ',') p++;
    }
    return 0;
}

static int validate_partition_layout(const PartitionUpdate *pu, uint32_t block_size,
                                     uint64_t *total_size) {
    uint64_t max_end = 0;
    if (block_size == 0) return -1;
    for (int j = 0; j < pu->num_operations; j++) {
        const InstallOperation *op = &pu->operations[j];
        for (int k = 0; k < op->num_dst_extents; k++) {
            uint64_t end_block, end_byte;
            if (add_u64(op->dst_extents[k].start_block,
                        op->dst_extents[k].num_blocks, &end_block) != 0 ||
                mul_u64(end_block, block_size, &end_byte) != 0 ||
                end_byte > (uint64_t)INT64_MAX)
                return -1;
            if (pu->new_info.size && end_byte > pu->new_info.size)
                return -1;
            if (end_byte > max_end) max_end = end_byte;
        }
    }
    *total_size = pu->new_info.size ? pu->new_info.size : max_end;
    if (*total_size > (uint64_t)INT64_MAX) return -1;
    for (int j = 0; j < pu->num_operations; j++) {
        const InstallOperation *op = &pu->operations[j];
        if (op->type != OP_MOVE) continue;
        for (int k = 0; k < op->num_src_extents; k++) {
            uint64_t end_block, end_byte;
            if (add_u64(op->src_extents[k].start_block,
                        op->src_extents[k].num_blocks, &end_block) != 0 ||
                mul_u64(end_block, block_size, &end_byte) != 0 ||
                end_byte > *total_size)
                return -1;
        }
    }
    return 0;
}

static uint64_t dst_extents_total_bytes(const InstallOperation *op, uint32_t block_size) {
    uint64_t total = 0;
    for (int i = 0; i < op->num_dst_extents; i++) {
        uint64_t bytes;
        if (mul_u64(op->dst_extents[i].num_blocks, block_size, &bytes) != 0 ||
            add_u64(total, bytes, &total) != 0)
            return UINT64_MAX;
    }
    return total;
}

static uint64_t src_extents_total_bytes(const InstallOperation *op, uint32_t block_size) {
    uint64_t total = 0;
    for (int i = 0; i < op->num_src_extents; i++) {
        uint64_t bytes;
        if (mul_u64(op->src_extents[i].num_blocks, block_size, &bytes) != 0 ||
            add_u64(total, bytes, &total) != 0)
            return UINT64_MAX;
    }
    return total;
}

static int read_buffer_from_src_extents(PartCtx *pc, const InstallOperation *op,
                                        uint32_t block_size, uint8_t *data,
                                        size_t data_len) {
    size_t pos = 0;

    if (pc->out_data) {
        for (int i = 0; i < op->num_src_extents && pos < data_len; i++) {
            uint64_t extent_off64, extent_len64;
            if (mul_u64(op->src_extents[i].start_block, block_size, &extent_off64) != 0 ||
                mul_u64(op->src_extents[i].num_blocks, block_size, &extent_len64) != 0 ||
                extent_off64 > (uint64_t)pc->map_size ||
                extent_len64 > (uint64_t)pc->map_size - extent_off64)
                return -1;
            size_t extent_off = (size_t)extent_off64;
            size_t extent_len = (size_t)extent_len64;
            size_t remaining = data_len - pos;
            size_t to_read = extent_len < remaining ? extent_len : remaining;
            memcpy(data + pos, pc->out_data + extent_off, to_read);
            pos += to_read;
        }
        return pos == data_len ? 0 : -1;
    }

    EnterCriticalSection(&pc->lock);
    for (int i = 0; i < op->num_src_extents && pos < data_len; i++) {
        uint64_t extent_off = op->src_extents[i].start_block * (uint64_t)block_size;
        uint64_t extent_len = op->src_extents[i].num_blocks * (uint64_t)block_size;
        uint64_t remaining = (uint64_t)(data_len - pos);
        uint64_t to_read = extent_len < remaining ? extent_len : remaining;
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)extent_off;
        if (!SetFilePointerEx(pc->hOut, li, NULL, FILE_BEGIN)) {
            LeaveCriticalSection(&pc->lock);
            return -1;
        }
        while (to_read > 0) {
            DWORD chunk = (DWORD)(to_read > 0x40000000ULL ? 0x40000000ULL : to_read);
            DWORD got = 0;
            if (!ReadFile(pc->hOut, data + pos, chunk, &got, NULL) || got == 0) {
                LeaveCriticalSection(&pc->lock);
                return -1;
            }
            pos += got;
            to_read -= got;
        }
    }
    LeaveCriticalSection(&pc->lock);
    return pos == data_len ? 0 : -1;
}

static int write_buffer_to_dst_extents(PartCtx *pc, const InstallOperation *op,
                                       uint32_t block_size, const uint8_t *data,
                                       size_t data_len) {
    size_t pos = 0;

    if (op->num_dst_extents <= 0)
        return data_len == 0 ? 0 : -1;

    if (pc->out_data) {
        for (int i = 0; i < op->num_dst_extents && pos < data_len; i++) {
            uint64_t extent_off64, extent_len64;
            if (mul_u64(op->dst_extents[i].start_block, block_size, &extent_off64) != 0 ||
                mul_u64(op->dst_extents[i].num_blocks, block_size, &extent_len64) != 0 ||
                extent_off64 > (uint64_t)pc->map_size ||
                extent_len64 > (uint64_t)pc->map_size - extent_off64)
                return -1;
            size_t extent_off = (size_t)extent_off64;
            size_t extent_len = (size_t)extent_len64;
            size_t remaining = data_len - pos;
            size_t to_write = extent_len < remaining ? extent_len : remaining;
            memcpy(pc->out_data + extent_off, data + pos, to_write);
            pos += to_write;
        }
        return pos == data_len ? 0 : -1;
    }

    EnterCriticalSection(&pc->lock);
    for (int i = 0; i < op->num_dst_extents && pos < data_len; i++) {
        uint64_t extent_off = op->dst_extents[i].start_block * (uint64_t)block_size;
        uint64_t extent_len = op->dst_extents[i].num_blocks * (uint64_t)block_size;
        uint64_t remaining = (uint64_t)(data_len - pos);
        uint64_t to_write = extent_len < remaining ? extent_len : remaining;
        LARGE_INTEGER li;
        li.QuadPart = extent_off;
        if (!SetFilePointerEx(pc->hOut, li, NULL, FILE_BEGIN)) {
            LeaveCriticalSection(&pc->lock);
            return -1;
        }
        while (to_write > 0) {
            DWORD chunk = (DWORD)(to_write > 0x40000000ULL ? 0x40000000ULL : to_write);
            DWORD written = 0;
            if (!WriteFile(pc->hOut, data + pos, chunk, &written, NULL) || written == 0) {
                LeaveCriticalSection(&pc->lock);
                return -1;
            }
            pos += written;
            to_write -= written;
        }
    }
    LeaveCriticalSection(&pc->lock);

    return pos == data_len ? 0 : -1;
}

static int write_all_handle(HANDLE file, const uint8_t *data, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        size_t remaining = len - pos;
        DWORD chunk = (DWORD)(remaining > 0x40000000ULL ? 0x40000000ULL : remaining);
        DWORD written = 0;
        if (!WriteFile(file, data + pos, chunk, &written, NULL) || written == 0)
            return -1;
        pos += written;
    }
    return 0;
}

static int write_buffer_to_dst_memory(uint8_t *image, size_t image_size,
                                      const InstallOperation *op,
                                      uint32_t block_size,
                                      const uint8_t *data, size_t data_len) {
    size_t pos = 0;

    if (op->num_dst_extents <= 0)
        return data_len == 0 ? 0 : -1;

    for (int i = 0; i < op->num_dst_extents && pos < data_len; i++) {
        uint64_t extent_off64, extent_len64;
        if (mul_u64(op->dst_extents[i].start_block, block_size, &extent_off64) != 0 ||
            mul_u64(op->dst_extents[i].num_blocks, block_size, &extent_len64) != 0 ||
            extent_off64 > image_size || extent_len64 > image_size - extent_off64)
            return -1;

        size_t extent_off = (size_t)extent_off64;
        size_t extent_len = (size_t)extent_len64;
        size_t remaining = data_len - pos;
        size_t to_copy = extent_len < remaining ? extent_len : remaining;
        memcpy(image + extent_off, data + pos, to_copy);
        pos += to_copy;
    }

    return pos == data_len ? 0 : -1;
}

static int zero_dst_extents(PartCtx *pc, const InstallOperation *op,
                            uint32_t block_size) {
    if (pc->out_data) {
        for (int k = 0; k < op->num_dst_extents; k++) {
            uint64_t zero_off64, zero_len64;
            if (mul_u64(op->dst_extents[k].start_block, block_size, &zero_off64) != 0 ||
                mul_u64(op->dst_extents[k].num_blocks, block_size, &zero_len64) != 0 ||
                zero_off64 > (uint64_t)pc->map_size ||
                zero_len64 > (uint64_t)pc->map_size - zero_off64)
                return -1;
            memset(pc->out_data + (size_t)zero_off64, 0, (size_t)zero_len64);
        }
        return 0;
    }

    EnterCriticalSection(&pc->lock);
    for (int k = 0; k < op->num_dst_extents; k++) {
        uint64_t zero_off = op->dst_extents[k].start_block * (uint64_t)block_size;
        uint64_t zero_len = op->dst_extents[k].num_blocks * (uint64_t)block_size;
        LARGE_INTEGER li;
        li.QuadPart = zero_off;
        if (!SetFilePointerEx(pc->hOut, li, NULL, FILE_BEGIN)) {
            LeaveCriticalSection(&pc->lock);
            return -1;
        }
        uint8_t zero_buf[65536];
        memset(zero_buf, 0, sizeof(zero_buf));
        while (zero_len > 0) {
            DWORD chunk = (DWORD)(zero_len > sizeof(zero_buf) ? sizeof(zero_buf) : zero_len);
            DWORD written = 0;
            if (!WriteFile(pc->hOut, zero_buf, chunk, &written, NULL) ||
                written != chunk) {
                LeaveCriticalSection(&pc->lock);
                return -1;
            }
            zero_len -= written;
        }
    }
    LeaveCriticalSection(&pc->lock);
    return 0;
}

static uint8_t *decode_operation_blob(const InstallOperation *op,
                                      const uint8_t *blob, size_t blob_len,
                                      size_t *out_len, int *borrowed_blob) {
    *out_len = 0;
    *borrowed_blob = 0;

    switch (op->type) {
    case OP_REPLACE:
        *out_len = blob_len;
        *borrowed_blob = 1;
        return (uint8_t*)blob;
    case OP_REPLACE_BZ:
        return decompress_bz2(blob, blob_len, out_len);
    case OP_REPLACE_XZ:
        return decompress_xz(blob, blob_len, out_len);
    case OP_ZSTD:
        return decompress_zstd(blob, blob_len, out_len);
    default:
        return NULL;
    }
}

static int apply_operation_to_memory(uint8_t *image, size_t image_size,
                                     const InstallOperation *op,
                                     uint32_t block_size,
                                     const uint8_t *blob, size_t blob_len) {
    if (op->type == OP_ZERO || op->type == OP_DISCARD)
        return 0;

    if (op->type == OP_MOVE || op->type == OP_BSDIFF ||
        op->type == OP_SOURCE_COPY || op->type == OP_SOURCE_BSDIFF ||
        op->type == OP_BROTLI_BSDIFF || op->type == OP_PUFFDIFF ||
        op->type == OP_ZUCCHINI || op->type == OP_LZ4DIFF_BSDIFF ||
        op->type == OP_LZ4DIFF_PUFFDIFF)
        return -1;

    if (op->data_sha256) {
        uint8_t hash[32];
        if (sha256_compute(blob, blob_len, hash) != 0 ||
            memcmp(hash, op->data_sha256, 32) != 0)
            return -1;
    }

    size_t data_len = 0;
    int borrowed_blob = 0;
    uint8_t *data = decode_operation_blob(op, blob, blob_len, &data_len, &borrowed_blob);
    if (!data)
        return -1;

    uint64_t expected = dst_extents_total_bytes(op, block_size);
    int rc = -1;
    if (expected != UINT64_MAX && expected <= SIZE_MAX && data_len == (size_t)expected)
        rc = write_buffer_to_dst_memory(image, image_size, op, block_size, data, data_len);

    if (!borrowed_blob)
        free(data);
    return rc;
}

static int find_kernel_release(const uint8_t *image, size_t image_size,
                               char *out, size_t out_size) {
    static const char marker[] = "Linux version ";
    static const char short_marker[] = "inux version ";
    static const char release_marker[] = "KERNEL_RELEASE";
    size_t marker_len = sizeof(marker) - 1;
    size_t short_marker_len = sizeof(short_marker) - 1;
    size_t release_marker_len = sizeof(release_marker) - 1;

    if (!image || !out || out_size == 0 || image_size < marker_len)
        return -1;

    for (size_t i = 0; i <= image_size - short_marker_len; i++) {
        size_t matched_len = 0;
        if (i <= image_size - marker_len && memcmp(image + i, marker, marker_len) == 0)
            matched_len = marker_len;
        else if (memcmp(image + i, short_marker, short_marker_len) == 0)
            matched_len = short_marker_len;
        else
            continue;

        size_t pos = i + matched_len;
        size_t out_pos = 0;
        if (pos >= image_size || image[pos] < '0' || image[pos] > '9')
            continue;
        while (pos < image_size && out_pos + 1 < out_size) {
            unsigned char ch = image[pos++];
            if (ch <= ' ' || ch == '(' || ch == ')' || ch == '\0')
                break;
            if (ch < 0x21 || ch > 0x7e)
                return -1;
            out[out_pos++] = (char)ch;
        }
        if (out_pos > 0) {
            out[out_pos] = '\0';
            return 0;
        }
    }

    if (image_size >= release_marker_len) {
        for (size_t i = 0; i <= image_size - release_marker_len; i++) {
            if (memcmp(image + i, release_marker, release_marker_len) != 0)
                continue;

            size_t pos = i + release_marker_len;
            while (pos < image_size && (image[pos] < '0' || image[pos] > '9'))
                pos++;
            if (pos >= image_size)
                continue;

            size_t out_pos = 0;
            while (pos < image_size && out_pos + 1 < out_size) {
                unsigned char ch = image[pos++];
                if (ch <= ' ' || ch == '\0')
                    break;
                if (ch < 0x21 || ch > 0x7e)
                    return -1;
                out[out_pos++] = (char)ch;
            }
            if (out_pos > 0) {
                out[out_pos] = '\0';
                return 0;
            }
        }
    }

    return -1;
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint64_t read_u64_le(const uint8_t *p) {
    uint64_t lo = read_u32_le(p);
    uint64_t hi = read_u32_le(p + 4);
    return lo | (hi << 32);
}

static int is_reasonable_page_size(uint32_t page_size) {
    return page_size >= 2048 && page_size <= 65536 &&
           (page_size & (page_size - 1)) == 0;
}

static uint64_t align_u64(uint64_t value, uint64_t align) {
    return (value + align - 1) / align * align;
}

static int find_kernel_release_in_kernel_data(const uint8_t *kernel, size_t kernel_size,
                                              char *out, size_t out_size) {
    if (find_kernel_release(kernel, kernel_size, out, out_size) == 0)
        return 0;

    uint8_t *dec = NULL;
    size_t dec_len = 0;
    if (kernel_size >= 4 &&
        kernel[0] == 0x02 && kernel[1] == 0x21 &&
        kernel[2] == 0x4C && kernel[3] == 0x18) {
        dec = decompress_lz4_legacy(kernel, kernel_size, &dec_len);
    } else if (kernel_size >= 6 && memcmp(kernel, "\xFD""7zXZ", 6) == 0) {
        dec = decompress_xz(kernel, kernel_size, &dec_len);
    } else if (kernel_size >= 4 &&
               kernel[0] == 0x28 && kernel[1] == 0xB5 &&
               kernel[2] == 0x2F && kernel[3] == 0xFD) {
        dec = decompress_zstd(kernel, kernel_size, &dec_len);
    } else if (kernel_size >= 3 && memcmp(kernel, "BZh", 3) == 0) {
        dec = decompress_bz2(kernel, kernel_size, &dec_len);
    }

    if (!dec)
        return -1;
    int rc = find_kernel_release(dec, dec_len, out, out_size);
    free(dec);
    return rc;
}

static int find_kernel_release_in_boot_image(const uint8_t *image, size_t image_size,
                                             char *out, size_t out_size) {
    if (image_size >= 8 && memcmp(image, "ANDROID!", 8) == 0) {
        uint32_t kernel_size = read_u32_le(image + 8);
        uint32_t header_size = image_size >= 24 ? read_u32_le(image + 20) : 0;
        uint32_t page_size = image_size >= 40 ? read_u32_le(image + 36) : 0;
        uint32_t header_version = image_size >= 44 ? read_u32_le(image + 40) : 0;
        uint64_t kernel_off = 0;

        if (header_version >= 3 && header_size >= 512 && header_size <= 4096) {
            kernel_off = align_u64(header_size, 4096);
        } else if (is_reasonable_page_size(page_size)) {
            kernel_off = page_size;
        } else {
            kernel_off = 4096;
        }

        if (kernel_size > 0 && kernel_off <= image_size &&
            kernel_size <= image_size - kernel_off &&
            find_kernel_release_in_kernel_data(image + kernel_off, kernel_size,
                                               out, out_size) == 0)
            return 0;
    }

    return find_kernel_release(image, image_size, out, out_size);
}

static int extract_arb_from_xbl_config(const uint8_t *image, size_t image_size,
                                       uint32_t *out_major,
                                       uint32_t *out_minor,
                                       uint32_t *out_arb) {
    if (!image || !out_major || !out_minor || !out_arb || image_size < 64 ||
        memcmp(image, "\x7F""ELF", 4) != 0 || image[4] != 2 || image[5] != 1)
        return -1;

    uint64_t phoff = read_u64_le(image + 0x20);
    uint16_t phentsz = read_u16_le(image + 0x36);
    uint16_t phnum = read_u16_le(image + 0x38);
    if (phentsz < 56 || phnum == 0 || phoff > image_size)
        return -1;

    uint64_t hash_off = 0;
    uint64_t hash_size = 0;
    for (int i = (int)phnum - 1; i >= 0; i--) {
        uint64_t off = phoff + (uint64_t)i * phentsz;
        if (off > image_size || 56 > image_size - off)
            return -1;

        uint32_t p_type = read_u32_le(image + off);
        uint64_t p_offset = read_u64_le(image + off + 8);
        uint64_t p_filesz = read_u64_le(image + off + 32);
        if (p_type == 0 && p_filesz > 0) {
            if (p_offset > image_size || p_filesz > image_size - p_offset)
                return -1;
            hash_off = p_offset;
            hash_size = p_filesz;
            break;
        }
    }
    if (hash_size == 0 || hash_size > SIZE_MAX)
        return -1;

    const uint8_t *seg = image + hash_off;
    size_t seg_size = (size_t)hash_size;
    size_t scan_limit = seg_size < 0x1000 ? seg_size : 0x1000;
    for (size_t off = 0; off + 36 <= seg_size && off + 36 <= scan_limit; off += 4) {
        uint32_t version = read_u32_le(seg + off);
        uint32_t common_sz = read_u32_le(seg + off + 4);
        uint32_t qti_sz = read_u32_le(seg + off + 8);
        uint32_t oem_sz = read_u32_le(seg + off + 12);
        uint32_t hash_tbl_sz = read_u32_le(seg + off + 16);

        if (version < 1 || version > 10)
            continue;
        if (common_sz > 0x1000 || oem_sz > 0x4000 || hash_tbl_sz > 0x4000)
            continue;

        uint64_t oem_md_off = (uint64_t)off + 36 + common_sz + qti_sz;
        uint64_t end = oem_md_off + oem_sz;
        if (end > seg_size || oem_md_off + 12 > seg_size)
            continue;

        *out_major = read_u32_le(seg + oem_md_off + 0);
        *out_minor = read_u32_le(seg + oem_md_off + 4);
        *out_arb = read_u32_le(seg + oem_md_off + 8);
        return 0;
    }

    return -1;
}

static int partition_name_equals(const char *a, const char *b) {
    return a && b && _stricmp(a, b) == 0;
}

static int find_named_partition(const DeltaArchiveManifest *manifest, const char *name) {
    for (int i = 0; i < manifest->num_partitions; i++) {
        if (partition_name_equals(manifest->partitions[i].partition_name, name))
            return i;
    }
    return -1;
}

static int extract_partition_image_local(const uint8_t *file_data, size_t file_size,
                                         uint64_t data_off, uint32_t block_size,
                                         PartitionUpdate *pu,
                                         uint8_t **out_image, size_t *out_size) {
    uint64_t total_size = 0;
    if (validate_partition_layout(pu, block_size, &total_size) != 0 ||
        total_size == 0 || total_size > SIZE_MAX)
        return -1;

    uint8_t *image = calloc(1, (size_t)total_size);
    if (!image)
        return -1;

    for (int i = 0; i < pu->num_operations; i++) {
        InstallOperation *op = &pu->operations[i];
        uint64_t blob_off = 0;
        if (add_u64(data_off, op->data_offset, &blob_off) != 0 ||
            blob_off > file_size || op->data_length > file_size - blob_off ||
            op->data_length > SIZE_MAX) {
            free(image);
            return -1;
        }

        const uint8_t *blob = file_data + blob_off;
        if (apply_operation_to_memory(image, (size_t)total_size, op, block_size,
                                      blob, (size_t)op->data_length) != 0) {
            free(image);
            return -1;
        }
    }

    *out_image = image;
    *out_size = (size_t)total_size;
    return 0;
}

static int extract_partition_image_http(HttpReader *hr, uint64_t file_size,
                                        uint64_t data_off, uint32_t block_size,
                                        PartitionUpdate *pu,
                                        uint8_t **out_image, size_t *out_size) {
    uint64_t total_size = 0;
    if (validate_partition_layout(pu, block_size, &total_size) != 0 ||
        total_size == 0 || total_size > SIZE_MAX)
        return -1;

    uint8_t *image = calloc(1, (size_t)total_size);
    if (!image)
        return -1;

    for (int i = 0; i < pu->num_operations; i++) {
        InstallOperation *op = &pu->operations[i];
        uint64_t blob_off = 0;
        if (add_u64(data_off, op->data_offset, &blob_off) != 0 ||
            blob_off > file_size || op->data_length > file_size - blob_off ||
            op->data_length > SIZE_MAX) {
            free(image);
            return -1;
        }

        uint8_t *blob = NULL;
        if (op->data_length > 0) {
            blob = malloc((size_t)op->data_length);
            if (!blob || http_read(hr, blob_off, blob, (size_t)op->data_length) != 0) {
                free(blob);
                free(image);
                return -1;
            }
        }

        int rc = apply_operation_to_memory(image, (size_t)total_size, op, block_size,
                                           blob, (size_t)op->data_length);
        free(blob);
        if (rc != 0) {
            free(image);
            return -1;
        }
    }

    *out_image = image;
    *out_size = (size_t)total_size;
    return 0;
}

static int print_kernel_version_from_manifest_local(const uint8_t *file_data, size_t file_size,
                                                    const DeltaArchiveManifest *manifest) {
    static const char *candidates[] = { "boot", "vendor_boot", "init_boot" };
    char version[256];

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        int p = find_named_partition(manifest, candidates[i]);
        if (p < 0)
            continue;

        uint8_t *image = NULL;
        size_t image_size = 0;
        if (extract_partition_image_local(file_data, file_size, manifest->data_offset,
                                          manifest->block_size, &manifest->partitions[p],
                                          &image, &image_size) == 0) {
            int found = find_kernel_release_in_boot_image(image, image_size,
                                                          version, sizeof(version));
            free(image);
            if (found == 0) {
                printf("%s\n", version);
                return 0;
            }
        }
    }

    fprintf(stderr, "Error: kernel version not found\n");
    return 1;
}

static int print_kernel_version_from_manifest_http(HttpReader *hr, uint64_t file_size,
                                                   const DeltaArchiveManifest *manifest) {
    static const char *candidates[] = { "boot", "vendor_boot", "init_boot" };
    char version[256];

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        int p = find_named_partition(manifest, candidates[i]);
        if (p < 0)
            continue;

        uint8_t *image = NULL;
        size_t image_size = 0;
        if (extract_partition_image_http(hr, file_size, manifest->data_offset,
                                         manifest->block_size, &manifest->partitions[p],
                                         &image, &image_size) == 0) {
            int found = find_kernel_release_in_boot_image(image, image_size,
                                                          version, sizeof(version));
            free(image);
            if (found == 0) {
                printf("%s\n", version);
                return 0;
            }
        }
    }

    fprintf(stderr, "Error: kernel version not found\n");
    return 1;
}

static int print_arb_from_manifest_local(const uint8_t *file_data, size_t file_size,
                                         const DeltaArchiveManifest *manifest) {
    static const char *candidates[] = { "xbl_config", "xbl_config_a", "xbl_config_b" };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        int p = find_named_partition(manifest, candidates[i]);
        if (p < 0)
            continue;

        uint8_t *image = NULL;
        size_t image_size = 0;
        if (extract_partition_image_local(file_data, file_size, manifest->data_offset,
                                          manifest->block_size, &manifest->partitions[p],
                                          &image, &image_size) == 0) {
            uint32_t major = 0, minor = 0, arb = 0;
            int found = extract_arb_from_xbl_config(image, image_size,
                                                    &major, &minor, &arb);
            free(image);
            if (found == 0) {
                printf("OEM Metadata Major Version : %u\n", major);
                printf("OEM Metadata Minor Version : %u\n", minor);
                printf("ARB (Anti-Rollback) : %u\n", arb);
                return 0;
            }
        }
    }

    fprintf(stderr, "Error: ARB not found\n");
    return 1;
}

static int print_arb_from_manifest_http(HttpReader *hr, uint64_t file_size,
                                        const DeltaArchiveManifest *manifest) {
    static const char *candidates[] = { "xbl_config", "xbl_config_a", "xbl_config_b" };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        int p = find_named_partition(manifest, candidates[i]);
        if (p < 0)
            continue;

        uint8_t *image = NULL;
        size_t image_size = 0;
        if (extract_partition_image_http(hr, file_size, manifest->data_offset,
                                         manifest->block_size, &manifest->partitions[p],
                                         &image, &image_size) == 0) {
            uint32_t major = 0, minor = 0, arb = 0;
            int found = extract_arb_from_xbl_config(image, image_size,
                                                    &major, &minor, &arb);
            free(image);
            if (found == 0) {
                printf("OEM Metadata Major Version : %u\n", major);
                printf("OEM Metadata Minor Version : %u\n", minor);
                printf("ARB (Anti-Rollback) : %u\n", arb);
                return 0;
            }
        }
    }

    fprintf(stderr, "Error: ARB not found\n");
    return 1;
}

/* Per-operation extraction function (called by threadpool) */
static void extract_op(void *arg, int task_idx) {
    DumpCtx *ctx = (DumpCtx*)arg;
    if (task_idx >= ctx->num_jobs) return;

    int p = ctx->jobs[task_idx].part_idx;
    int op_idx = ctx->jobs[task_idx].op_idx;
    PartitionUpdate *pu = &ctx->parts[p];
    InstallOperation *op = &pu->operations[op_idx];
    PartCtx *pc = &ctx->part_ctx[p];

    /* Read compressed blob */
    uint64_t blob_off = 0;
    uint64_t blob_len = op->data_length;

    if (add_u64(ctx->data_off, op->data_offset, &blob_off) != 0 ||
        blob_off > ctx->file_size || blob_len > ctx->file_size - blob_off) {
        fprintf(stderr, "\nError: op %d blob out of bounds (off=%llu len=%llu file=%llu)\n",
                op_idx, (unsigned long long)blob_off, (unsigned long long)blob_len,
                (unsigned long long)ctx->file_size);
        mark_failed(ctx);
        return;
    }

    const uint8_t *blob = ctx->file_data + blob_off;

    uint8_t *dec = NULL;
    size_t dec_len = 0;

    switch (op->type) {
    case OP_REPLACE:
        if (op->num_dst_extents > 0) {
            uint64_t write_len = dst_extents_total_bytes(op, ctx->block_size);
            if (write_len == UINT64_MAX || write_len != blob_len) {
                fprintf(stderr, "\nError: %s op %d size mismatch\n", pc->name, op_idx);
                mark_failed(ctx);
                return;
            }
            if (op->data_sha256) {
                uint8_t hash[32];
                if (sha256_compute(blob, (size_t)blob_len, hash) != 0 ||
                    memcmp(hash, op->data_sha256, 32) != 0) {
                    fprintf(stderr, "\nError: SHA256 mismatch %s op %d\n", pc->name, op_idx);
                    mark_failed(ctx);
                    return;
                }
            }
            if (write_buffer_to_dst_extents(pc, op, ctx->block_size, blob, (size_t)write_len) != 0) {
                fprintf(stderr, "\nError: %s op %d write failed\n", pc->name, op_idx);
                mark_failed(ctx);
                return;
            }
        }
        break;

    case OP_MOVE:
        if (op->num_src_extents > 0 && op->num_dst_extents > 0) {
            uint64_t src_len = src_extents_total_bytes(op, ctx->block_size);
            uint64_t dst_len = dst_extents_total_bytes(op, ctx->block_size);
            if (src_len == UINT64_MAX || src_len != dst_len || src_len > SIZE_MAX) {
                fprintf(stderr, "\nError: %s op %d MOVE extent size mismatch\n", pc->name, op_idx);
                mark_failed(ctx);
                return;
            }
            uint8_t *tmp = malloc((size_t)src_len);
            if (!tmp || read_buffer_from_src_extents(pc, op, ctx->block_size,
                                                     tmp, (size_t)src_len) != 0 ||
                write_buffer_to_dst_extents(pc, op, ctx->block_size,
                                            tmp, (size_t)src_len) != 0) {
                fprintf(stderr, "\nError: %s op %d MOVE failed\n", pc->name, op_idx);
                free(tmp);
                mark_failed(ctx);
                return;
            }
            free(tmp);
        }
        break;

    case OP_REPLACE_BZ:
        dec = decompress_bz2(blob, (size_t)blob_len, &dec_len);
        if (!dec) {
            fprintf(stderr, "\nError: BZ2 decompress failed %s op %d\n", pc->name, op_idx);
            mark_failed(ctx);
            return;
        }
        /* SHA256 verification (hash of compressed blob) */
        if (op->data_sha256) {
            uint8_t hash[32];
            if (sha256_compute(blob, (size_t)blob_len, hash) != 0) {
                fprintf(stderr, "\nError: SHA256 compute failed %s op %d\n", pc->name, op_idx);
                free(dec); mark_failed(ctx); return;
            }
            if (memcmp(hash, op->data_sha256, 32) != 0) {
                fprintf(stderr, "\nError: SHA256 mismatch %s op %d -- data may be corrupt\n", pc->name, op_idx);
                free(dec); mark_failed(ctx); return;
            }
        }
        if (op->num_dst_extents > 0) {
            uint64_t expected = dst_extents_total_bytes(op, ctx->block_size);
            if (expected == UINT64_MAX || dec_len != (size_t)expected) {
                fprintf(stderr, "\nError: %s op %d size mismatch: expected %llu, got %zu\n",
                        pc->name, op_idx, (unsigned long long)expected, dec_len);
                free(dec); mark_failed(ctx); return;
            }
            if (write_buffer_to_dst_extents(pc, op, ctx->block_size, dec, dec_len) != 0) {
                fprintf(stderr, "\nError: %s op %d write failed\n", pc->name, op_idx);
                free(dec); mark_failed(ctx); return;
            }
        }
        break;

    case OP_REPLACE_XZ:
        dec = decompress_xz(blob, (size_t)blob_len, &dec_len);
        if (!dec) {
            fprintf(stderr, "\nError: XZ decompress failed %s op %d\n", pc->name, op_idx);
            mark_failed(ctx);
            return;
        }
        /* SHA256 verification (hash of compressed blob) */
        if (op->data_sha256) {
            uint8_t hash[32];
            if (sha256_compute(blob, (size_t)blob_len, hash) != 0) {
                fprintf(stderr, "\nError: SHA256 compute failed %s op %d\n", pc->name, op_idx);
                free(dec); mark_failed(ctx); return;
            }
            if (memcmp(hash, op->data_sha256, 32) != 0) {
                fprintf(stderr, "\nError: SHA256 mismatch %s op %d -- data may be corrupt\n", pc->name, op_idx);
                free(dec); mark_failed(ctx); return;
            }
        }
        if (op->num_dst_extents > 0) {
            uint64_t expected = dst_extents_total_bytes(op, ctx->block_size);
            if (expected == UINT64_MAX || dec_len != (size_t)expected) {
                fprintf(stderr, "\nError: %s op %d size mismatch: expected %llu, got %zu\n",
                        pc->name, op_idx, (unsigned long long)expected, dec_len);
                free(dec); mark_failed(ctx); return;
            }
            if (write_buffer_to_dst_extents(pc, op, ctx->block_size, dec, dec_len) != 0) {
                fprintf(stderr, "\nError: %s op %d write failed\n", pc->name, op_idx);
                free(dec); mark_failed(ctx); return;
            }
        }
        break;

    case OP_BROTLI_BSDIFF:
        fprintf(stderr, "\nError: %s op %d BROTLI_BSDIFF not yet supported (need bspatch)\n", pc->name, op_idx);
        mark_failed(ctx);
        return;

    case OP_ZSTD:
        dec = decompress_zstd(blob, (size_t)blob_len, &dec_len);
        if (!dec) {
            fprintf(stderr, "\nError: ZSTD decompress failed %s op %d\n", pc->name, op_idx);
            mark_failed(ctx);
            return;
        }
        /* SHA256 verification (hash of compressed blob) */
        if (op->data_sha256) {
            uint8_t hash[32];
            if (sha256_compute(blob, (size_t)blob_len, hash) != 0) {
                fprintf(stderr, "\nError: SHA256 compute failed %s op %d\n", pc->name, op_idx);
                free(dec); mark_failed(ctx); return;
            }
            if (memcmp(hash, op->data_sha256, 32) != 0) {
                fprintf(stderr, "\nError: SHA256 mismatch %s op %d -- data may be corrupt\n", pc->name, op_idx);
                free(dec); mark_failed(ctx); return;
            }
        }
        if (op->num_dst_extents > 0) {
            uint64_t expected = dst_extents_total_bytes(op, ctx->block_size);
            if (expected == UINT64_MAX || dec_len != (size_t)expected) {
                fprintf(stderr, "\nError: %s op %d size mismatch: expected %llu, got %zu\n",
                        pc->name, op_idx, (unsigned long long)expected, dec_len);
                free(dec); mark_failed(ctx); return;
            }
            if (write_buffer_to_dst_extents(pc, op, ctx->block_size, dec, dec_len) != 0) {
                fprintf(stderr, "\nError: %s op %d write failed\n", pc->name, op_idx);
                free(dec); mark_failed(ctx); return;
            }
        }
        break;

    case OP_ZERO:
        if (zero_dst_extents(pc, op, ctx->block_size) != 0) {
            fprintf(stderr, "\nError: %s op %d zero write failed\n", pc->name, op_idx);
            mark_failed(ctx);
            return;
        }
        break;

    default:
        fprintf(stderr, "\nError: unsupported op type %d in %s\n", op->type, pc->name);
        mark_failed(ctx);
        return;
    }

    free(dec);
    {
        LONG done = InterlockedIncrement(&ctx->ops_done);
        int total = ctx->num_jobs;
        int pct = (int)(done * 100LL / total);
        fprintf(stderr, "\r  %d/%d ops (%d%%)    ", (int)done, total, pct);
        fflush(stderr);
        if (done == total) fprintf(stderr, "\n");
    }
}

static void extract_http_op(void *arg, int task_idx) {
    DumpCtx *ctx = (DumpCtx*)arg;
    if (task_idx >= ctx->num_jobs) return;

    int p = ctx->jobs[task_idx].part_idx;
    int op_idx = ctx->jobs[task_idx].op_idx;
    PartitionUpdate *pu = &ctx->parts[p];
    InstallOperation *op = &pu->operations[op_idx];
    PartCtx *pc = &ctx->part_ctx[p];

    uint64_t blob_off = 0;
    uint64_t blob_len = op->data_length;
    if (add_u64(ctx->data_off, op->data_offset, &blob_off) != 0 ||
        blob_off > ctx->payload_end || blob_len > ctx->payload_end - blob_off ||
        blob_len > SIZE_MAX) {
        fprintf(stderr, "\nError: HTTP blob is out of payload bounds for %s op %d\n",
                pc->name, op_idx);
        mark_failed(ctx);
        return;
    }

    uint8_t *blob = NULL;
    if (blob_len > 0) {
        blob = malloc((size_t)blob_len);
        if (!blob) {
            mark_failed(ctx);
            return;
        }
        HttpReader local_hr = *ctx->http;
        if (http_read(&local_hr, blob_off, blob, (size_t)blob_len) != 0) {
            fprintf(stderr, "\nError: HTTP read failed for %s op %d\n", pc->name, op_idx);
            free(blob);
            mark_failed(ctx);
            return;
        }
    }

    uint8_t *dec = NULL;
    size_t dec_len = 0;

    switch (op->type) {
    case OP_REPLACE:
        if (op->num_dst_extents > 0) {
            uint64_t write_len = dst_extents_total_bytes(op, ctx->block_size);
            if (write_len == UINT64_MAX || write_len != blob_len) {
                fprintf(stderr, "\nError: %s op %d size mismatch\n", pc->name, op_idx);
                free(blob); mark_failed(ctx); return;
            }
            if (op->data_sha256) {
                uint8_t hash[32];
                if (sha256_compute(blob, (size_t)blob_len, hash) != 0 ||
                    memcmp(hash, op->data_sha256, 32) != 0) {
                    fprintf(stderr, "\nError: SHA256 mismatch %s op %d\n", pc->name, op_idx);
                    free(blob); mark_failed(ctx); return;
                }
            }
            if (write_buffer_to_dst_extents(pc, op, ctx->block_size, blob, (size_t)write_len) != 0) {
                fprintf(stderr, "\nError: %s op %d write failed\n", pc->name, op_idx);
                free(blob); mark_failed(ctx); return;
            }
        }
        break;
    case OP_REPLACE_BZ:
        dec = decompress_bz2(blob, (size_t)blob_len, &dec_len);
        if (!dec) {
            fprintf(stderr, "\nError: BZ2 decompress failed %s op %d\n", pc->name, op_idx);
            free(blob); mark_failed(ctx); return;
        }
        if (op->data_sha256) {
            uint8_t hash[32];
            if (sha256_compute(blob, (size_t)blob_len, hash) != 0 ||
                memcmp(hash, op->data_sha256, 32) != 0) {
                fprintf(stderr, "\nError: SHA256 mismatch %s op %d -- data may be corrupt\n", pc->name, op_idx);
                free(dec); free(blob); mark_failed(ctx); return;
            }
        }
        if (op->num_dst_extents > 0) {
            uint64_t expected = dst_extents_total_bytes(op, ctx->block_size);
            if (expected == UINT64_MAX || dec_len != (size_t)expected ||
                write_buffer_to_dst_extents(pc, op, ctx->block_size, dec, dec_len) != 0) {
                fprintf(stderr, "\nError: %s op %d write failed\n", pc->name, op_idx);
                free(dec); free(blob); mark_failed(ctx); return;
            }
        }
        break;
    case OP_REPLACE_XZ:
        dec = decompress_xz(blob, (size_t)blob_len, &dec_len);
        if (!dec) {
            fprintf(stderr, "\nError: XZ decompress failed %s op %d\n", pc->name, op_idx);
            free(blob); mark_failed(ctx); return;
        }
        if (op->data_sha256) {
            uint8_t hash[32];
            if (sha256_compute(blob, (size_t)blob_len, hash) != 0 ||
                memcmp(hash, op->data_sha256, 32) != 0) {
                fprintf(stderr, "\nError: SHA256 mismatch %s op %d -- data may be corrupt\n", pc->name, op_idx);
                free(dec); free(blob); mark_failed(ctx); return;
            }
        }
        if (op->num_dst_extents > 0) {
            uint64_t expected = dst_extents_total_bytes(op, ctx->block_size);
            if (expected == UINT64_MAX || dec_len != (size_t)expected ||
                write_buffer_to_dst_extents(pc, op, ctx->block_size, dec, dec_len) != 0) {
                fprintf(stderr, "\nError: %s op %d write failed\n", pc->name, op_idx);
                free(dec); free(blob); mark_failed(ctx); return;
            }
        }
        break;
    case OP_ZSTD:
        dec = decompress_zstd(blob, (size_t)blob_len, &dec_len);
        if (!dec) {
            fprintf(stderr, "\nError: ZSTD decompress failed %s op %d\n", pc->name, op_idx);
            free(blob); mark_failed(ctx); return;
        }
        if (op->data_sha256) {
            uint8_t hash[32];
            if (sha256_compute(blob, (size_t)blob_len, hash) != 0 ||
                memcmp(hash, op->data_sha256, 32) != 0) {
                fprintf(stderr, "\nError: SHA256 mismatch %s op %d -- data may be corrupt\n", pc->name, op_idx);
                free(dec); free(blob); mark_failed(ctx); return;
            }
        }
        if (op->num_dst_extents > 0) {
            uint64_t expected = dst_extents_total_bytes(op, ctx->block_size);
            if (expected == UINT64_MAX || dec_len != (size_t)expected ||
                write_buffer_to_dst_extents(pc, op, ctx->block_size, dec, dec_len) != 0) {
                fprintf(stderr, "\nError: %s op %d write failed\n", pc->name, op_idx);
                free(dec); free(blob); mark_failed(ctx); return;
            }
        }
        break;
    case OP_ZERO:
        if (zero_dst_extents(pc, op, ctx->block_size) != 0) {
            fprintf(stderr, "\nError: %s op %d zero write failed\n", pc->name, op_idx);
            free(blob); mark_failed(ctx); return;
        }
        break;
    default:
        fprintf(stderr, "\nError: unsupported op type %d in %s\n", op->type, pc->name);
        free(blob); mark_failed(ctx); return;
    }

    free(dec);
    free(blob);
    {
        LONG done = InterlockedIncrement(&ctx->ops_done);
        int total = ctx->num_jobs;
        int pct = (int)(done * 100LL / total);
        fprintf(stderr, "\r  %d/%d ops (%d%%)    ", (int)done, total, pct);
        fflush(stderr);
        if (done == total) fprintf(stderr, "\n");
    }
}

static void extract_partition(void *arg, int part_idx) {
    DumpCtx *ctx = (DumpCtx*)arg;
    for (int i = 0; i < ctx->num_jobs; i++) {
        if (ctx->jobs[i].part_idx == part_idx)
            extract_op(arg, i);
    }
}

static int op_type_parallel_safe(InstallOpType type) {
    return type == OP_REPLACE ||
           type == OP_REPLACE_BZ ||
           type == OP_REPLACE_XZ ||
           type == OP_ZSTD ||
           type == OP_ZERO;
}

static int jobs_parallel_safe(const DumpCtx *ctx) {
    for (int i = 0; i < ctx->num_jobs; i++) {
        int p = ctx->jobs[i].part_idx;
        int op_idx = ctx->jobs[i].op_idx;
        if (!op_type_parallel_safe(ctx->parts[p].operations[op_idx].type))
            return 0;
    }
    return 1;
}

/* Forward declaration for HTTP path */
static int dumper_run_http(const char *url, const char *out_dir,
                            const char *partitions, int num_threads,
                            int list_only, int metadata_only,
                            int kernel_version_only, int arb_only);

static int is_valid_utf8_text(const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t c = data[i];
        if (c == 0)
            return 0;
        if (c < 0x80) {
            if (c < 0x20 && c != '\r' && c != '\n' && c != '\t')
                return 0;
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= len || (data[i + 1] & 0xC0) != 0x80 || c < 0xC2)
                return 0;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= len || (data[i + 1] & 0xC0) != 0x80 || (data[i + 2] & 0xC0) != 0x80)
                return 0;
            if (c == 0xE0 && data[i + 1] < 0xA0)
                return 0;
            if (c == 0xED && data[i + 1] >= 0xA0)
                return 0;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= len || (data[i + 1] & 0xC0) != 0x80 ||
                (data[i + 2] & 0xC0) != 0x80 || (data[i + 3] & 0xC0) != 0x80)
                return 0;
            if (c == 0xF0 && data[i + 1] < 0x90)
                return 0;
            if (c > 0xF4 || (c == 0xF4 && data[i + 1] >= 0x90))
                return 0;
            i += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

int dumper_run(const char *payload_path, const char *out_dir,
               const char *partitions, int num_threads,
               int list_only, int metadata_only,
               int kernel_version_only, int arb_only)
{
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Detect URL input */
    if (strncmp(payload_path, "http://", 7) == 0 ||
        strncmp(payload_path, "https://", 8) == 0) {
        return dumper_run_http(payload_path, out_dir, partitions,
                                num_threads, list_only, metadata_only,
                                kernel_version_only, arb_only);
    }

    size_t file_size = 0;
    HANDLE file_map = NULL;
    uint8_t *file_data = map_file_read(payload_path, &file_size, &file_map);
    if (!file_data) {
        return 1;
    }
    
    /* Handle --metadata FIRST */
    if (metadata_only) {
        int metadata_rc = 1;
        if (file_size >= 4 && file_data[0] == 0x50 && file_data[1] == 0x4B &&
            file_data[2] == 0x03 && file_data[3] == 0x04) {
            uint64_t meta_off, meta_size;
            if (zip_find_stored_entry(file_data, file_size,
                                      "META-INF/com/android/metadata",
                                      &meta_off, &meta_size) == 0) {
                if (meta_off > file_size || meta_size > file_size - meta_off ||
                    meta_size > SIZE_MAX) {
                    fprintf(stderr, "Error: metadata entry is out of bounds\n");
                    unmap_file(file_data, file_map);
                    return 1;
                }
                int display_text = is_valid_utf8_text(file_data + meta_off, (size_t)meta_size);
                if (display_text) {
                    fwrite(file_data + meta_off, 1, (size_t)meta_size, stdout);
                    printf("\n");
                }
                if (ensure_output_dir(out_dir) != 0) {
                    fprintf(stderr, "Error: cannot create output directory %s\n", out_dir);
                    unmap_file(file_data, file_map);
                    return 1;
                }
                char out_path[1024];
                if (build_output_path(out_path, sizeof(out_path), out_dir, "metadata") != 0) {
                    fprintf(stderr, "Error: metadata output path is too long\n");
                    unmap_file(file_data, file_map);
                    return 1;
                }
                wchar_t *wout_path_m = utf8_to_wide(out_path);
                HANDLE hFile = CreateFileW(wout_path_m, GENERIC_WRITE, 0, NULL,
                                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                free(wout_path_m);
                if (hFile != INVALID_HANDLE_VALUE) {
                    int write_rc = write_all_handle(hFile, file_data + meta_off, (size_t)meta_size);
                    CloseHandle(hFile);
                    if (write_rc == 0) {
                        if (!display_text)
                            printf("Metadata is not UTF-8 text; saved raw data to %s\n", out_path);
                        printf("Metadata saved to %s\n", out_path);
                        metadata_rc = 0;
                    } else {
                        fprintf(stderr, "Error: failed to write metadata\n");
                    }
                } else {
                    fprintf(stderr, "Error: cannot create metadata output file\n");
                }
            } else {
                printf("Error: META-INF/com/android/metadata not found in ZIP\n");
            }
        } else {
            printf("Error: not a valid ZIP file\n");
        }
        unmap_file(file_data, file_map);
        return metadata_rc;
    }

    /* Create output directory */
    {
        if (ensure_output_dir(out_dir) != 0) {
            fprintf(stderr, "Error: cannot create output directory %s\n", out_dir);
            unmap_file(file_data, file_map);
            return 1;
        }
    }

    uint64_t payload_offset = 0;
    uint64_t payload_size = file_size;

    /* Check if it's a ZIP file */
    if (file_size >= 4 && file_data[0] == 0x50 && file_data[1] == 0x4B &&
        file_data[2] == 0x03 && file_data[3] == 0x04) {
        uint64_t entry_off, entry_size;
        if (zip_find_stored_entry(file_data, file_size, "payload.bin",
                                  &entry_off, &entry_size) == 0) {
            payload_offset = entry_off;
            payload_size = entry_size;
            // printf("Found payload.bin in ZIP at offset %llu, size %llu\n",
                   // (unsigned long long)payload_offset, (unsigned long long)entry_size);
        }
    }

    if (payload_offset > file_size || payload_size > file_size - payload_offset) {
        fprintf(stderr, "Error: payload entry is out of file bounds\n");
        unmap_file(file_data, file_map);
        return 1;
    }

    /* Parse payload header + manifest */
    PayloadHeader header;
    DeltaArchiveManifest manifest;
    memset(&header, 0, sizeof(header));
    memset(&manifest, 0, sizeof(manifest));

    if (parse_payload_metadata(file_data + payload_offset, payload_size,
                                &header, &manifest, payload_offset) != 0) {
        fprintf(stderr, "Error: failed to parse payload metadata\n");
        unmap_file(file_data, file_map);
        return 1;
    }

    for (int p = 0; p < manifest.num_partitions; p++) {
        uint64_t checked_size = 0;
        PartitionUpdate *pu = &manifest.partitions[p];
        if (!valid_partition_name(pu->partition_name) ||
            validate_partition_layout(pu, manifest.block_size, &checked_size) != 0) {
            fprintf(stderr, "Error: invalid partition name or extent layout at index %d\n", p);
            free_manifest(&manifest);
            unmap_file(file_data, file_map);
            return 1;
        }
    }

    if (kernel_version_only) {
        int rc = print_kernel_version_from_manifest_local(file_data, file_size, &manifest);
        free_manifest(&manifest);
        unmap_file(file_data, file_map);
        return rc;
    }

    if (arb_only) {
        int rc = print_arb_from_manifest_local(file_data, file_size, &manifest);
        free_manifest(&manifest);
        unmap_file(file_data, file_map);
        return rc;
    }

    // printf("Payload: format v%llu, block_size=%u, minor=%u\n",
           // (unsigned long long)header.file_format_version,
           // manifest.block_size, manifest.minor_version);
    // printf("Partitions: %d\n", manifest.num_partitions);
    /* Handle --list */
    if (list_only) {
        printf("Partitions: ");
        for (int i = 0; i < manifest.num_partitions; i++) {
            PartitionUpdate *pu = &manifest.partitions[i];
            if (i > 0) printf(", ");
            printf("%s", pu->partition_name ? pu->partition_name : "(unnamed)");
        }
        printf("\n");
        free_manifest(&manifest);
        unmap_file(file_data, file_map);
        return 0;
    }

    /* ===== Full parallelism extraction ===== */

    printf("\nExtracting partitions (workers: %d)...\n", num_threads);

    /* Step 1: count matching partitions, open output files */
    PartCtx *part_ctx = calloc(manifest.num_partitions, sizeof(PartCtx));
    if (!part_ctx) {
        fprintf(stderr, "Error: out of memory allocating partition contexts\n");
        free_manifest(&manifest);
        unmap_file(file_data, file_map);
        return 1;
    }
    int num_sel = 0;
    int total_ops = 0;
    int setup_failed = 0;

    for (int p = 0; p < manifest.num_partitions; p++) {
        PartitionUpdate *pu = &manifest.partitions[p];
        if (!pu->partition_name || pu->num_operations <= 0) continue;

        /* Apply partition filter */
        if (!partition_selected(partitions, pu->partition_name)) continue;

        PartCtx *pc = &part_ctx[p];
        strncpy(pc->name, pu->partition_name, sizeof(pc->name) - 1);

        /* Open output file */
        char out_path[1024];
        char img_name[320];
        int name_len = snprintf(img_name, sizeof(img_name), "%s.img", pu->partition_name);
        if (name_len < 0 || (size_t)name_len >= sizeof(img_name) ||
            build_output_path(out_path, sizeof(out_path), out_dir, img_name) != 0) {
            fprintf(stderr, "Error: output path is too long for partition %s\n", pu->partition_name);
            setup_failed = 1;
            continue;
        }
        wchar_t *wout = utf8_to_wide(out_path);
        pc->hOut = CreateFileW(wout, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        free(wout);
        if (pc->hOut == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "Error: cannot create %s\n", out_path);
            pc->hOut = NULL;
            setup_failed = 1;
            continue;
        }

        InitializeCriticalSection(&pc->lock);

        /* Calculate total file size */
        uint64_t total_size = 0;
        validate_partition_layout(pu, manifest.block_size, &total_size);
        pc->total_size = total_size;

        /* Pre-allocate sparse file */
        if (total_size > 0) {
            LARGE_INTEGER li;
            li.QuadPart = total_size;
            if (!SetFilePointerEx(pc->hOut, li, NULL, FILE_BEGIN) ||
                !SetEndOfFile(pc->hOut)) {
                fprintf(stderr, "Error: cannot size %s\n", out_path);
                close_part_ctx(pc);
                setup_failed = 1;
                continue;
            }
            map_output_file(pc);
        }

        if (pu->num_operations > INT_MAX - total_ops) {
            fprintf(stderr, "Error: too many payload operations\n");
            close_part_ctx(pc);
            setup_failed = 1;
            continue;
        }
        total_ops += pu->num_operations;
        num_sel++;
    }

    if (num_sel == 0) {
        printf("No partitions to extract.\n");
        free(part_ctx);
        free_manifest(&manifest);
        unmap_file(file_data, file_map);
        return 1;
    }

    /* Step 2: build flat job list */
    JobEntry *jobs = malloc(total_ops * sizeof(JobEntry));
    if (!jobs) {
        fprintf(stderr, "Error: out of memory allocating operation list\n");
        for (int p = 0; p < manifest.num_partitions; p++) {
            close_part_ctx(&part_ctx[p]);
        }
        free(part_ctx);
        free_manifest(&manifest);
        unmap_file(file_data, file_map);
        return 1;
    }
    int job_idx = 0;
    for (int p = 0; p < manifest.num_partitions; p++) {
        PartitionUpdate *pu = &manifest.partitions[p];
        if (!pu->partition_name || pu->num_operations <= 0) continue;
        if (!partition_selected(partitions, pu->partition_name)) continue;
        if (!part_ctx[p].hOut) continue;

        for (int j = 0; j < pu->num_operations; j++) {
            jobs[job_idx].part_idx = p;
            jobs[job_idx].op_idx = j;
            job_idx++;
        }
    }

    /* Step 3: submit ALL jobs to thread pool */
    DumpCtx ctx;
    ctx.file_data = file_data;
    ctx.file_size = (size_t)(payload_offset + payload_size);
    ctx.data_off = manifest.data_offset;
    ctx.block_size = manifest.block_size;
    ctx.num_parts = num_sel;
    ctx.parts = manifest.partitions;
    ctx.part_ctx = part_ctx;
    ctx.num_jobs = total_ops;
    ctx.jobs = jobs;
    ctx.ops_done = 0;
    ctx.failed = setup_failed;

    printf("  %d operations across %d partitions\n", total_ops, num_sel);

    double extract_start = now_seconds();
    ThreadPool *pool = threadpool_create(num_threads);
    if (pool) {
        if (jobs_parallel_safe(&ctx)) {
            if (threadpool_run(pool, extract_op, &ctx, total_ops) != 0)
                mark_failed(&ctx);
        } else {
            if (threadpool_run(pool, extract_partition, &ctx, manifest.num_partitions) != 0)
                mark_failed(&ctx);
        }
        threadpool_destroy(pool);
    } else {
        if (jobs_parallel_safe(&ctx)) {
            for (int i = 0; i < total_ops; i++)
                extract_op(&ctx, i);
        } else {
            for (int p = 0; p < manifest.num_partitions; p++)
                extract_partition(&ctx, p);
        }
    }

    double extract_seconds = now_seconds() - extract_start;

    /* Step 4: close all files, report sizes */
    printf("\nResults:\n");
    for (int i = 0; i < manifest.num_partitions; i++) {
        PartCtx *pc = &part_ctx[i];
        if (pc->hOut) {
            uint64_t sz = pc->total_size;
            close_part_ctx(pc);
            const char *unit = "B";
            double val = (double)sz;
            if (sz >= 1024LL*1024*1024) { val = sz / (1024.0*1024*1024); unit = "GB"; }
            else if (sz >= 1024*1024) { val = sz / (1024.0*1024); unit = "MB"; }
            else if (sz >= 1024) { val = sz / 1024.0; unit = "KB"; }
            printf("  %-20s %.1f %s\n", pc->name, val, unit);
        }
    }

    if (ctx.failed)
        fprintf(stderr, "\nExtraction failed; one or more output images are incomplete.\n");
    else {
        print_ops_timing(total_ops, extract_seconds);
        printf("\nDone.\n");
    }

    free(jobs);
    free(part_ctx);
    free_manifest(&manifest);
    unmap_file(file_data, file_map);
    return ctx.failed ? 1 : 0;
}


/* ===== HTTP URL extraction path ===== */

static int http_read_wrapper(void *ctx, uint64_t offset, uint8_t *buf, size_t size) {
    return http_read((HttpReader*)ctx, offset, buf, size);
}

static int dumper_run_http(const char *url, const char *out_dir,
                            const char *partitions, int num_threads,
                            int list_only, int metadata_only,
                            int kernel_version_only, int arb_only)
{
    HttpReader hr;
    if (http_open(&hr, url) != 0) {
        return 1;
    }
    uint64_t file_size = http_size(&hr);
    // fprintf(stderr, "HTTP file size: %llu bytes\n"rintf(stderr, "HTTP file size: %llu bytes\n", (unsigned long long)file_size);

    /* Handle --metadata FIRST */
    if (metadata_only) {
        int metadata_rc = 1;
        uint64_t meta_off, meta_size;
        if (zip_find_stored_entry_http(file_size, http_read_wrapper, &hr,
                                       "META-INF/com/android/metadata",
                                       &meta_off, &meta_size) == 0) {
            if (meta_off > file_size || meta_size > file_size - meta_off ||
                meta_size > SIZE_MAX) {
                fprintf(stderr, "Error: metadata entry is out of bounds\n");
                http_close(&hr);
                return 1;
            }
            uint8_t *meta = malloc((size_t)meta_size);
            if (meta) {
                if (http_read(&hr, meta_off, meta, (size_t)meta_size) != 0) {
                    fprintf(stderr, "Error: failed to read metadata\n");
                    free(meta);
                    http_close(&hr);
                    return 1;
                }
                int display_text = is_valid_utf8_text(meta, (size_t)meta_size);
                if (display_text) {
                    fwrite(meta, 1, (size_t)meta_size, stdout);
                    printf("\n");
                }
                /* Save to file */
                if (ensure_output_dir(out_dir) != 0) {
                    fprintf(stderr, "Error: cannot create output directory %s\n", out_dir);
                    free(meta);
                    http_close(&hr);
                    return 1;
                }
                char out_path[1024];
                if (build_output_path(out_path, sizeof(out_path), out_dir, "metadata") != 0) {
                    fprintf(stderr, "Error: metadata output path is too long\n");
                    free(meta);
                    http_close(&hr);
                    return 1;
                }
                wchar_t *wp_meta = utf8_to_wide(out_path);
                HANDLE hFile = wp_meta ? CreateFileW(wp_meta, GENERIC_WRITE, 0, NULL,
                                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
                free(wp_meta);
                if (hFile != INVALID_HANDLE_VALUE) {
                    int write_rc = write_all_handle(hFile, meta, (size_t)meta_size);
                    CloseHandle(hFile);
                    if (write_rc == 0) {
                        if (!display_text)
                            printf("Metadata is not UTF-8 text; saved raw data to %s\n", out_path);
                        printf("Metadata saved to %s\n", out_path);
                        metadata_rc = 0;
                    } else {
                        fprintf(stderr, "Error: failed to write metadata\n");
                    }
                } else {
                    fprintf(stderr, "Error: cannot create metadata output file\n");
                }
                free(meta);
            } else {
                fprintf(stderr, "Error: out of memory reading metadata\n");
            }
        } else {
            printf("Error: META-INF/com/android/metadata not found\n");
        }
        http_close(&hr);
        return metadata_rc;
    }

    /* Find payload.bin in ZIP */
    uint64_t payload_off = 0, payload_size = 0;
    if (zip_find_stored_entry_http(file_size, http_read_wrapper, &hr,
                                    "payload.bin", &payload_off, &payload_size) != 0) {
        fprintf(stderr, "Error: payload.bin not found in ZIP\n");
        http_close(&hr);
        return 1;
    }
    // fprintf(stderr, "Found payload.bin at offset %llu, size %llu\n",
            // (unsigned long long)payload_off, (unsigned long long)payload_size);

    /* Read the common 20-byte header first; v2+ adds four signature-size bytes. */
    uint8_t pheader[24] = {0};
    if (payload_size < 20 || http_read(&hr, payload_off, pheader, 20) != 0) {
        fprintf(stderr, "Error: failed to read payload header\n");
        http_close(&hr);
        return 1;
    }

    if (memcmp(pheader, "CrAU", 4) != 0) {
        fprintf(stderr, "Error: invalid payload magic\n");
        http_close(&hr);
        return 1;
    }

    /* Parse header (big-endian, without unaligned pointer casts). */
    uint64_t ver_be, msz_be;
    memcpy(&ver_be, pheader + 4, 8);
    memcpy(&msz_be, pheader + 12, 8);
    uint64_t ver = __builtin_bswap64(ver_be);
    uint64_t manifest_size = __builtin_bswap64(msz_be);
    uint32_t meta_sig = 0;
    if (ver > 1) {
        if (payload_size < 24 || http_read(&hr, payload_off + 20, pheader + 20, 4) != 0) {
            fprintf(stderr, "Error: failed to read payload v2 header\n");
            http_close(&hr);
            return 1;
        }
        uint32_t msig_be;
        memcpy(&msig_be, pheader + 20, 4);
        meta_sig = __builtin_bswap32(msig_be);
    }
    uint64_t header_size = ver > 1 ? 24 : 20;
    if (manifest_size == 0 || header_size > payload_size ||
        manifest_size > payload_size - header_size ||
        meta_sig > payload_size - header_size - manifest_size ||
        manifest_size > SIZE_MAX - header_size) {
        fprintf(stderr, "Error: HTTP payload manifest is out of bounds\n");
        http_close(&hr);
        return 1;
    }

    /* Read manifest */
    uint8_t *manifest_buf = malloc((size_t)manifest_size);
    if (!manifest_buf) {
        http_close(&hr);
        return 1;
    }
    uint64_t manifest_off = payload_off + header_size;
    if (http_read(&hr, manifest_off, manifest_buf, (size_t)manifest_size) != 0) {
        free(manifest_buf);
        http_close(&hr);
        return 1;
    }

    /* Parse manifest */
    PayloadHeader header;
    DeltaArchiveManifest manifest;
    memset(&header, 0, sizeof(header));
    memset(&manifest, 0, sizeof(manifest));

    /* Build a synthetic buffer mimicking the real payload header + manifest */
    // fprintf(stderr, "  Allocating synth buffer (%llu bytes)\n"rintf(stderr, "  Allocating synth buffer (%llu bytes)\n", (unsigned long long)(24 + manifest_size));
    size_t synth_size = (size_t)header_size + (size_t)manifest_size;
    uint8_t *synth = malloc(synth_size);
    if (!synth) { fprintf(stderr, "  malloc failed!\n"); free(manifest_buf); http_close(&hr); return 1; }
    // fprintf(stderr, "  Building synth header\n"rintf(stderr, "  Building synth header\n");
    if (!synth) { free(manifest_buf); http_close(&hr); return 1; }
    memcpy(synth, "CrAU", 4);
    /* version (big-endian) */
    uint64_t synth_ver_be = __builtin_bswap64(ver);
    memcpy(synth + 4, &synth_ver_be, 8);
    /* manifest_size (big-endian) */
    uint64_t synth_msz_be = __builtin_bswap64(manifest_size);
    memcpy(synth + 12, &synth_msz_be, 8);
    /* metadata_signature_size (big-endian) */
    if (ver > 1) {
        uint32_t msig_be = __builtin_bswap32(meta_sig);
        memcpy(synth + 20, &msig_be, 4);
    }
    /* Append manifest data */
    memcpy(synth + header_size, manifest_buf, (size_t)manifest_size);
    free(manifest_buf);

    uint64_t data_off = payload_off + header_size + manifest_size + meta_sig;

    // fprintf(stderr, "  Calling parse_payload_metadata (bufsize=%llu)\n"rintf(stderr, "  Calling parse_payload_metadata (bufsize=%llu)\n", (unsigned long long)(24 + manifest_size));
    if (parse_payload_metadata(synth, synth_size,
                                &header, &manifest, payload_off) != 0) {
        fprintf(stderr, "Error: failed to parse manifest\n");
        // fprintf(stderr, "  synth magic=%02x%02x%02x%02x\n"rintf(stderr, "  synth magic=%02x%02x%02x%02x\n", synth[0], synth[1], synth[2], synth[3]);
        free(synth);
        http_close(&hr);
        return 1;
    }
    free(synth);

    /* Fix data_offset */
    manifest.data_offset = data_off;

    for (int p = 0; p < manifest.num_partitions; p++) {
        uint64_t checked_size = 0;
        PartitionUpdate *pu = &manifest.partitions[p];
        if (!valid_partition_name(pu->partition_name) ||
            validate_partition_layout(pu, manifest.block_size, &checked_size) != 0) {
            fprintf(stderr, "Error: invalid partition name or extent layout at index %d\n", p);
            free_manifest(&manifest);
            http_close(&hr);
            return 1;
        }
    }

    if (kernel_version_only) {
        int rc = print_kernel_version_from_manifest_http(&hr, file_size, &manifest);
        free_manifest(&manifest);
        http_close(&hr);
        return rc;
    }

    if (arb_only) {
        int rc = print_arb_from_manifest_http(&hr, file_size, &manifest);
        free_manifest(&manifest);
        http_close(&hr);
        return rc;
    }

    // printf("Payload: format v%llu, block_size=%u, minor=%u\n",
           // (unsigned long long)header.file_format_version,
           // manifest.block_size, manifest.minor_version);
    // printf("Partitions: %d\n", manifest.num_partitions);

     /* Handle --list */
     if (list_only) {
         printf("Partitions: ");
        for (int i = 0; i < manifest.num_partitions; i++) {
            PartitionUpdate *pu = &manifest.partitions[i];
            if (i > 0) printf(", ");
            printf("%s", pu->partition_name ? pu->partition_name : "(unnamed)");
        }
        printf("\n");
        free_manifest(&manifest);
        http_close(&hr);
        return 0;
    }

    /* ===== Extraction with HTTP range reads ===== */

    if (ensure_output_dir(out_dir) != 0) {
        fprintf(stderr, "Error: cannot create output directory %s\n", out_dir);
        free_manifest(&manifest);
        http_close(&hr);
        return 1;
    }

    /* Select and open output files */
    int num_sel = 0;
    int total_ops = 0;
    int setup_failed = 0;
    PartCtx *part_ctx = calloc(manifest.num_partitions, sizeof(PartCtx));
    if (!part_ctx) {
        fprintf(stderr, "Error: out of memory allocating partition contexts\n");
        free(part_ctx);
        free_manifest(&manifest);
        http_close(&hr);
        return 1;
    }

    for (int p = 0; p < manifest.num_partitions; p++) {
        PartitionUpdate *pu = &manifest.partitions[p];
        if (!pu->partition_name || pu->num_operations <= 0) continue;

        if (!partition_selected(partitions, pu->partition_name)) continue;

        PartCtx *pc = &part_ctx[p];
        strncpy(pc->name, pu->partition_name, sizeof(pc->name) - 1);

        char out_path[1024];
        char img_name[320];
        int name_len = snprintf(img_name, sizeof(img_name), "%s.img", pu->partition_name);
        if (name_len < 0 || (size_t)name_len >= sizeof(img_name) ||
            build_output_path(out_path, sizeof(out_path), out_dir, img_name) != 0) {
            fprintf(stderr, "Error: output path is too long for partition %s\n", pu->partition_name);
            setup_failed = 1;
            continue;
        }
        wchar_t *wp_part = utf8_to_wide(out_path);
        pc->hOut = wp_part ? CreateFileW(wp_part, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
        free(wp_part);
        if (pc->hOut == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "Error: cannot create %s\n", out_path);
            pc->hOut = NULL;
            setup_failed = 1;
            continue;
        }
        InitializeCriticalSection(&pc->lock);

        uint64_t total_size = 0;
        validate_partition_layout(pu, manifest.block_size, &total_size);
        pc->total_size = total_size;
        if (total_size > 0) {
            LARGE_INTEGER li;
            li.QuadPart = total_size;
            if (!SetFilePointerEx(pc->hOut, li, NULL, FILE_BEGIN) ||
                !SetEndOfFile(pc->hOut)) {
                fprintf(stderr, "Error: cannot size %s\n", out_path);
                close_part_ctx(pc);
                setup_failed = 1;
                continue;
            }
            map_output_file(pc);
        }
        if (pu->num_operations > INT_MAX - total_ops) {
            fprintf(stderr, "Error: too many payload operations\n");
            close_part_ctx(pc);
            setup_failed = 1;
            continue;
        }
        total_ops += pu->num_operations;
        num_sel++;
    }

    if (num_sel == 0) {
        printf("No partitions to extract.\n");
        free(part_ctx);
        free_manifest(&manifest);
        http_close(&hr);
        return 1;
    }

    printf("\nExtracting partitions (workers: %d)...\n", num_threads);

    /* Build job list */
    int num_jobs = total_ops;
    JobEntry *jobs = malloc(num_jobs * sizeof(JobEntry));
    if (!jobs) {
        fprintf(stderr, "Error: out of memory allocating operation list\n");
        for (int p = 0; p < manifest.num_partitions; p++) {
            close_part_ctx(&part_ctx[p]);
        }
        free(part_ctx);
        free_manifest(&manifest);
        http_close(&hr);
        return 1;
    }
    int job_idx = 0;
    for (int p = 0; p < manifest.num_partitions; p++) {
        PartitionUpdate *pu = &manifest.partitions[p];
        if (!pu->partition_name || pu->num_operations <= 0) continue;
        /* Re-apply filter */
        if (!partition_selected(partitions, pu->partition_name)) continue;
        if (!part_ctx[p].hOut) continue;
        for (int j = 0; j < pu->num_operations; j++) {
            jobs[job_idx].part_idx = p;
            jobs[job_idx].op_idx = j;
            job_idx++;
        }
    }

    printf("  %d operations across %d partitions\n", num_jobs, num_sel);

    int extraction_failed = setup_failed;
    double extract_start = now_seconds();
    uint64_t payload_end = payload_off + payload_size;

    DumpCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.data_off = manifest.data_offset;
    ctx.payload_end = payload_end;
    ctx.http = &hr;
    ctx.block_size = manifest.block_size;
    ctx.num_parts = num_sel;
    ctx.parts = manifest.partitions;
    ctx.part_ctx = part_ctx;
    ctx.num_jobs = num_jobs;
    ctx.jobs = jobs;
    ctx.ops_done = 0;
    ctx.failed = extraction_failed;

    if (jobs_parallel_safe(&ctx)) {
        ThreadPool *pool = threadpool_create(num_threads);
        if (pool) {
            if (threadpool_run(pool, extract_http_op, &ctx, num_jobs) != 0)
                mark_failed(&ctx);
            threadpool_destroy(pool);
        } else {
            for (int i = 0; i < num_jobs; i++)
                extract_http_op(&ctx, i);
        }
        extraction_failed = ctx.failed;
        goto http_extract_done;
    }

    /* Dependency-bearing operations must preserve manifest order. */
    for (int i = 0; i < num_jobs; i++) {
        int p = jobs[i].part_idx;
        int op_i = jobs[i].op_idx;
        PartitionUpdate *pu = &manifest.partitions[p];
        InstallOperation *op = &pu->operations[op_i];
        PartCtx *pc = &part_ctx[p];

        uint64_t blob_off = 0;
        uint64_t blob_len = op->data_length;
        if (add_u64(manifest.data_offset, op->data_offset, &blob_off) != 0 ||
            blob_off > payload_end || blob_len > payload_end - blob_off ||
            blob_len > SIZE_MAX) {
            fprintf(stderr, "\nError: HTTP blob is out of payload bounds for %s op %d\n",
                    pc->name, op_i);
            extraction_failed = 1;
            continue;
        }

        /* Fetch blob data via HTTP */
        uint8_t *blob = NULL;
        if (blob_len > 0) {
            blob = malloc((size_t)blob_len);
            if (!blob) { extraction_failed = 1; continue; }
            if (http_read(&hr, blob_off, blob, (size_t)blob_len) != 0) {
                fprintf(stderr, "\nError: HTTP read failed for %s op %d\n", pc->name, op_i);
                free(blob);
                extraction_failed = 1;
                continue;
            }
        }

        /* Decompress */
        uint8_t *dec = NULL;
        size_t dec_len = 0;

        if (op->type == OP_ZERO) {
            int zero_failed = 0;
            zero_failed = zero_dst_extents(pc, op, manifest.block_size) != 0;
            free(blob);
            if (zero_failed) {
                fprintf(stderr, "\nError: %s op %d zero write failed\n", pc->name, op_i);
                goto op_failed;
            }
            goto op_done;
        }
        switch (op->type) {
        case OP_REPLACE:
            if (op->num_dst_extents > 0) {
                uint64_t write_len = dst_extents_total_bytes(op, manifest.block_size);
                if (write_len == UINT64_MAX || write_len != blob_len) {
                    fprintf(stderr, "\nError: %s op %d size mismatch\n", pc->name, op_i);
                    free(blob);
                    goto op_failed;
                }
                if (op->data_sha256) {
                    uint8_t hash[32];
                    if (sha256_compute(blob, (size_t)blob_len, hash) != 0 ||
                        memcmp(hash, op->data_sha256, 32) != 0) {
                        fprintf(stderr, "\nError: SHA256 mismatch %s op %d\n", pc->name, op_i);
                        free(blob);
                        goto op_failed;
                    }
                }
                if (write_buffer_to_dst_extents(pc, op, manifest.block_size, blob, (size_t)write_len) != 0) {
                    fprintf(stderr, "\nError: %s op %d write failed\n", pc->name, op_i);
                    free(blob);
                    goto op_failed;
                }
            }
            break;
        case OP_MOVE:
            if (op->num_src_extents > 0 && op->num_dst_extents > 0) {
                uint64_t src_len = src_extents_total_bytes(op, manifest.block_size);
                uint64_t dst_len = dst_extents_total_bytes(op, manifest.block_size);
                if (src_len == UINT64_MAX || src_len != dst_len || src_len > SIZE_MAX) {
                    fprintf(stderr, "\nError: %s op %d MOVE extent size mismatch\n", pc->name, op_i);
                    free(blob);
                    goto op_failed;
                }
                uint8_t *tmp = malloc((size_t)src_len);
                if (!tmp || read_buffer_from_src_extents(pc, op, manifest.block_size,
                                                         tmp, (size_t)src_len) != 0 ||
                    write_buffer_to_dst_extents(pc, op, manifest.block_size,
                                                tmp, (size_t)src_len) != 0) {
                    fprintf(stderr, "\nError: %s op %d MOVE failed\n", pc->name, op_i);
                    free(tmp); free(blob);
                    goto op_failed;
                }
                free(tmp);
            }
            break;
        case OP_REPLACE_BZ:
            dec = decompress_bz2(blob, (size_t)blob_len, &dec_len);
            if (!dec) { fprintf(stderr, "\nError: BZ2 decompress failed %s op %d\n", pc->name, op_i); free(blob); goto op_failed; }
            /* SHA256 verification (hash of compressed blob) */
            if (op->data_sha256) {
                uint8_t hash[32];
                if (sha256_compute(blob, (size_t)blob_len, hash) != 0) {
                    fprintf(stderr, "\nError: SHA256 compute failed %s op %d\n", pc->name, op_i);
                    free(dec); free(blob); goto op_failed;
                }
                if (memcmp(hash, op->data_sha256, 32) != 0) {
                    fprintf(stderr, "\nError: SHA256 mismatch %s op %d -- data may be corrupt\n", pc->name, op_i);
                    free(dec); free(blob); goto op_failed;
                }
            }
            if (op->num_dst_extents > 0) {
                uint64_t expected = dst_extents_total_bytes(op, manifest.block_size);
                if (expected == UINT64_MAX || dec_len != (size_t)expected) { fprintf(stderr, "\nError: %s op %d size mismatch\n", pc->name, op_i); free(dec); free(blob); goto op_failed; }
                if (write_buffer_to_dst_extents(pc, op, manifest.block_size, dec, dec_len) != 0) {
                    fprintf(stderr, "\nError: %s op %d write failed\n", pc->name, op_i);
                    free(dec); free(blob); goto op_failed;
                }
            }
            break;
        case OP_REPLACE_XZ:
            dec = decompress_xz(blob, (size_t)blob_len, &dec_len);
            if (!dec) { fprintf(stderr, "\nError: XZ decompress failed %s op %d\n", pc->name, op_i); free(blob); goto op_failed; }
            /* SHA256 verification (hash of compressed blob) */
            if (op->data_sha256) {
                uint8_t hash[32];
                if (sha256_compute(blob, (size_t)blob_len, hash) != 0) {
                    fprintf(stderr, "\nError: SHA256 compute failed %s op %d\n", pc->name, op_i);
                    free(dec); free(blob); goto op_failed;
                }
                if (memcmp(hash, op->data_sha256, 32) != 0) {
                    fprintf(stderr, "\nError: SHA256 mismatch %s op %d -- data may be corrupt\n", pc->name, op_i);
                    free(dec); free(blob); goto op_failed;
                }
            }
            if (op->num_dst_extents > 0) {
                uint64_t expected = dst_extents_total_bytes(op, manifest.block_size);
                if (expected == UINT64_MAX || dec_len != (size_t)expected) { fprintf(stderr, "\nError: %s op %d size mismatch\n", pc->name, op_i); free(dec); free(blob); goto op_failed; }
                if (write_buffer_to_dst_extents(pc, op, manifest.block_size, dec, dec_len) != 0) {
                    fprintf(stderr, "\nError: %s op %d write failed\n", pc->name, op_i);
                    free(dec); free(blob); goto op_failed;
                }
            }
            break;
        case OP_BROTLI_BSDIFF:
            fprintf(stderr, "\nError: %s op %d BROTLI_BSDIFF not yet supported (need bspatch)\n", pc->name, op_i);
            free(blob);
            goto op_failed;
        case OP_ZSTD:
            dec = decompress_zstd(blob, (size_t)blob_len, &dec_len);
            if (!dec) { fprintf(stderr, "\nError: ZSTD decompress failed %s op %d\n", pc->name, op_i); free(blob); goto op_failed; }
            /* SHA256 verification (hash of compressed blob) */
            if (op->data_sha256) {
                uint8_t hash[32];
                if (sha256_compute(blob, (size_t)blob_len, hash) != 0) {
                    fprintf(stderr, "\nError: SHA256 compute failed %s op %d\n", pc->name, op_i);
                    free(dec); free(blob); goto op_failed;
                }
                if (memcmp(hash, op->data_sha256, 32) != 0) {
                    fprintf(stderr, "\nError: SHA256 mismatch %s op %d -- data may be corrupt\n", pc->name, op_i);
                    free(dec); free(blob); goto op_failed;
                }
            }
            if (op->num_dst_extents > 0) {
                uint64_t expected = dst_extents_total_bytes(op, manifest.block_size);
                if (expected == UINT64_MAX || dec_len != (size_t)expected) { fprintf(stderr, "\nError: %s op %d size mismatch\n", pc->name, op_i); free(dec); free(blob); goto op_failed; }
                if (write_buffer_to_dst_extents(pc, op, manifest.block_size, dec, dec_len) != 0) {
                    fprintf(stderr, "\nError: %s op %d write failed\n", pc->name, op_i);
                    free(dec); free(blob); goto op_failed;
                }
            }
            break;
        default:
            fprintf(stderr, "\nError: unsupported op type %d in %s\n", op->type, pc->name);
            free(blob);
            goto op_failed;
        }

        free(dec);
        free(blob);
        goto op_done;

op_failed:
        extraction_failed = 1;

op_done:
        /* Progress */
        {
            int done_total = i + 1;
            int pct = (int)(done_total * 100LL / num_jobs);
            fprintf(stderr, "\r  %d/%d ops (%d%%)    ", done_total, num_jobs, pct);
            fflush(stderr);
            if (done_total == num_jobs) fprintf(stderr, "\n");
        }
    }

http_extract_done:
    ;
    double extract_seconds = now_seconds() - extract_start;

    free(jobs);

    /* Close files and report */
    printf("\nResults:\n");
    for (int p = 0; p < manifest.num_partitions; p++) {
        PartCtx *pc = &part_ctx[p];
        if (pc->hOut) {
            uint64_t sz = pc->total_size;
            close_part_ctx(pc);
            const char *unit = "B";
            double val = (double)sz;
            if (sz >= 1024LL*1024*1024) { val = sz / (1024.0*1024*1024); unit = "GB"; }
            else if (sz >= 1024*1024) { val = sz / (1024.0*1024); unit = "MB"; }
            else if (sz >= 1024) { val = sz / 1024.0; unit = "KB"; }
            printf("  %-20s %.1f %s\n", pc->name, val, unit);
        }
    }
    if (extraction_failed)
        fprintf(stderr, "\nExtraction failed; one or more output images are incomplete.\n");
    else {
        print_ops_timing(num_jobs, extract_seconds);
        printf("\nDone.\n");
    }

    free(part_ctx);
    free_manifest(&manifest);
    http_close(&hr);
    return extraction_failed ? 1 : 0;
}

/* ===== File reading ===== */
static uint8_t* map_file_read(const char *path, size_t *out_size, HANDLE *out_map) {
    setvbuf(stderr, NULL, _IONBF, 0);

    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath) {
        fprintf(stderr, "Error: utf8_to_wide failed for path\n");
        fprintf(stderr, "  path (UTF-8 bytes): ");
        const unsigned char *p = (const unsigned char*)path;
        while (*p) { fprintf(stderr, "%%%02x", *p); p++; }
        fprintf(stderr, "\n");
        return NULL;
    }
    HANDLE hFile = CreateFileW(wpath, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD err = GetLastError();
    free(wpath);
    if (hFile == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Error: cannot open file (GetLastError=%lu)\n", err);
        switch (err) {
        case 2:   fprintf(stderr, "  -> ERROR_FILE_NOT_FOUND\n"); break;
        case 3:   fprintf(stderr, "  -> ERROR_PATH_NOT_FOUND\n"); break;
        case 5:   fprintf(stderr, "  -> ERROR_ACCESS_DENIED\n"); break;
        case 53:  fprintf(stderr, "  -> ERROR_BAD_NETPATH\n"); break;
        case 67:  fprintf(stderr, "  -> ERROR_BAD_NET_NAME\n"); break;
        case 123: fprintf(stderr, "  -> ERROR_INVALID_NAME\n"); break;
        default:  break;
        }
        return NULL;
    }

    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li) || li.QuadPart <= 0 ||
        (uint64_t)li.QuadPart > SIZE_MAX) {
        fprintf(stderr, "Error: invalid or unsupported input file size\n");
        CloseHandle(hFile);
        return NULL;
    }
    *out_size = (size_t)li.QuadPart;

    HANDLE hMap = CreateFileMappingW(hFile, NULL, PAGE_READONLY, li.HighPart, li.LowPart, NULL);
    if (!hMap) {
        fprintf(stderr, "Error: CreateFileMapping failed (GetLastError=%lu)\n", GetLastError());
        CloseHandle(hFile);
        return NULL;
    }

    uint8_t *buf = (uint8_t*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!buf) {
        fprintf(stderr, "Error: MapViewOfFile failed (GetLastError=%lu)\n", GetLastError());
        CloseHandle(hMap);
        CloseHandle(hFile);
        return NULL;
    }

    *out_map = hMap;
    CloseHandle(hFile);
    return buf;
}

static void unmap_file(uint8_t *buf, HANDLE hMap) {
    if (buf) UnmapViewOfFile(buf);
    if (hMap) CloseHandle(hMap);
}
