/**
 * decompress.c -- Decompression wrappers for zstd, brotli, bz2, xz
 */
#include "decompress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "zstd.h"

#include "brotli/decode.h"

#include "bzlib.h"

#include "lzma.h"

uint8_t* decompress_zstd(const uint8_t *in, size_t in_len, size_t *out_len) {
    if (!in || !out_len || in_len == 0) return NULL;
    *out_len = 0;
    unsigned long long dst_size = ZSTD_getFrameContentSize(in, in_len);
    if (dst_size == ZSTD_CONTENTSIZE_UNKNOWN || dst_size == ZSTD_CONTENTSIZE_ERROR) {
        /* Streaming decompress fallback */
        ZSTD_DCtx *dctx = ZSTD_createDCtx();
        if (!dctx) return NULL;
        size_t buf_size = ZSTD_DStreamOutSize() * 2;
        uint8_t *out = malloc(buf_size);
        if (!out) { ZSTD_freeDCtx(dctx); return NULL; }
        
        ZSTD_inBuffer input = {in, in_len, 0};
        ZSTD_outBuffer output = {out, buf_size, 0};
        size_t total = 0;
        
        size_t ret = 1;
        while (input.pos < input.size || ret != 0) {
            if (output.pos == output.size) {
                if (buf_size > SIZE_MAX / 2) {
                    free(out); ZSTD_freeDCtx(dctx); return NULL;
                }
                buf_size *= 2;
                uint8_t *tmp = realloc(out, buf_size);
                if (!tmp) { free(out); ZSTD_freeDCtx(dctx); return NULL; }
                out = tmp;
                output.dst = out;
                output.size = buf_size;
            }
            size_t old_in = input.pos;
            size_t old_out = output.pos;
            ret = ZSTD_decompressStream(dctx, &output, &input);
            if (ZSTD_isError(ret)) {
                free(out); ZSTD_freeDCtx(dctx); return NULL;
            }
            if (ret == 0 && input.pos == input.size) break;
            if (input.pos == old_in && output.pos == old_out) {
                free(out); ZSTD_freeDCtx(dctx); return NULL;
            }
            if (input.pos == input.size && ret != 0) {
                free(out); ZSTD_freeDCtx(dctx); return NULL;
            }
        }
        total = output.pos;
        *out_len = total;
        ZSTD_freeDCtx(dctx);
        return out;
    }
    
    if (dst_size > SIZE_MAX) return NULL;
    *out_len = (size_t)dst_size;
    uint8_t *out = malloc((size_t)dst_size);
    if (!out) return NULL;
    size_t ret = ZSTD_decompress(out, (size_t)dst_size, in, in_len);
    if (ZSTD_isError(ret) || ret != (size_t)dst_size) { free(out); return NULL; }
    *out_len = ret;
    return out;
}

uint8_t* decompress_brotli(const uint8_t *in, size_t in_len, size_t *out_len) {
    if (!in || !out_len || in_len == 0 || in_len > SIZE_MAX / 4) return NULL;
    *out_len = 0;
    BrotliDecoderResult result;
    size_t available_in = in_len;
    const uint8_t *next_in = in;
    size_t buf_size = in_len * 4;
    if (buf_size < 4096) buf_size = 4096;
    
    uint8_t *out = malloc(buf_size);
    if (!out) return NULL;
    size_t available_out = buf_size;
    uint8_t *next_out = out;
    
    BrotliDecoderState *state = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (!state) { free(out); return NULL; }
    
    result = BrotliDecoderDecompressStream(state, &available_in, &next_in,
                                           &available_out, &next_out, NULL);
    while (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
        size_t used = next_out - out;
        if (buf_size > SIZE_MAX / 2) {
            free(out); BrotliDecoderDestroyInstance(state); return NULL;
        }
        buf_size *= 2;
        uint8_t *tmp = realloc(out, buf_size);
        if (!tmp) { free(out); BrotliDecoderDestroyInstance(state); return NULL; }
        out = tmp;
        next_out = out + used;
        available_out = buf_size - used;
        result = BrotliDecoderDecompressStream(state, &available_in, &next_in,
                                               &available_out, &next_out, NULL);
    }
    
    BrotliDecoderDestroyInstance(state);
    if (result != BROTLI_DECODER_RESULT_SUCCESS) {
        free(out);
        return NULL;
    }
    *out_len = next_out - out;
    return out;
}

uint8_t* decompress_bz2(const uint8_t *in, size_t in_len, size_t *out_len) {
    if (!in || !out_len || in_len == 0 || in_len > UINT_MAX || in_len > SIZE_MAX / 4)
        return NULL;
    *out_len = 0;
    /* Guess output size: bzip2 typically achieves 2-4x */
    size_t buf_size = in_len * 4;
    if (buf_size < 4096) buf_size = 4096;
    
    uint8_t *out = malloc(buf_size);
    if (!out) return NULL;
    
    bz_stream strm = {0};
    strm.next_in = (char*)in;
    strm.avail_in = (unsigned int)in_len;
    
    int ret = BZ2_bzDecompressInit(&strm, 0, 0);
    if (ret != BZ_OK) { free(out); return NULL; }
    
    strm.next_out = (char*)out;
    strm.avail_out = (unsigned int)buf_size;
    
    while ((ret = BZ2_bzDecompress(&strm)) == BZ_OK) {
        if (strm.avail_out == 0) {
            size_t used = strm.total_out_lo32;
            if (buf_size > UINT_MAX / 2) {
                free(out); BZ2_bzDecompressEnd(&strm); return NULL;
            }
            buf_size *= 2;
            uint8_t *tmp = realloc(out, buf_size);
            if (!tmp) { free(out); BZ2_bzDecompressEnd(&strm); return NULL; }
            out = tmp;
            strm.next_out = (char*)(out + used);
            strm.avail_out = (unsigned int)(buf_size - used);
        }
    }
    
    BZ2_bzDecompressEnd(&strm);
    if (ret != BZ_STREAM_END) { free(out); return NULL; }
    *out_len = strm.total_out_lo32;
    return out;
}

static uint8_t* decompress_xz_flags(const uint8_t *in, size_t in_len,
                                    size_t *out_len, uint32_t flags) {
    if (!in || !out_len || in_len == 0 || in_len > SIZE_MAX / 4) return NULL;
    *out_len = 0;
    size_t buf_size = in_len * 4;
    if (buf_size < 4096) buf_size = 4096;
    for (;;) {
        uint8_t *out = malloc(buf_size);
        if (!out) return NULL;
        lzma_stream strm = LZMA_STREAM_INIT;
        lzma_ret ret = lzma_stream_decoder(&strm, UINT64_MAX, flags);
        if (ret != LZMA_OK) { free(out); return NULL; }
        strm.next_in = in;
        strm.avail_in = in_len;
        strm.next_out = out;
        strm.avail_out = buf_size;

        int retry_larger = 0;
        for (;;) {
            uint64_t old_in = strm.total_in;
            uint64_t old_out = strm.total_out;
            ret = lzma_code(&strm, LZMA_FINISH);
            if (ret == LZMA_STREAM_END && strm.avail_in == 0 && strm.total_out > 0) {
                *out_len = (size_t)strm.total_out;
                lzma_end(&strm);
                return out;
            }
            if (strm.avail_out == 0) {
                retry_larger = 1;
                break;
            }
            if (ret != LZMA_OK ||
                (old_in == strm.total_in && old_out == strm.total_out))
                break;
        }
        lzma_end(&strm);
        free(out);
        if (!retry_larger || buf_size > SIZE_MAX / 2)
            return NULL;
        buf_size *= 2;
    }
}

uint8_t* decompress_xz(const uint8_t *in, size_t in_len, size_t *out_len) {
    return decompress_xz_flags(in, in_len, out_len, LZMA_CONCATENATED);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int ensure_capacity(uint8_t **buf, size_t *cap, size_t need) {
    if (need <= *cap)
        return 0;
    size_t new_cap = *cap ? *cap : 65536;
    while (new_cap < need) {
        if (new_cap > SIZE_MAX / 2)
            return -1;
        new_cap *= 2;
    }
    uint8_t *tmp = realloc(*buf, new_cap);
    if (!tmp)
        return -1;
    *buf = tmp;
    *cap = new_cap;
    return 0;
}

static int lz4_block_decode(const uint8_t *in, size_t in_len,
                            uint8_t **out, size_t *out_len, size_t *out_cap) {
    size_t ip = 0;
    size_t op = *out_len;

    while (ip < in_len) {
        uint8_t token = in[ip++];
        size_t lit_len = token >> 4;
        size_t match_len = token & 0x0F;

        if (lit_len == 15) {
            uint8_t s;
            do {
                if (ip >= in_len || lit_len > SIZE_MAX - 255)
                    return -1;
                s = in[ip++];
                lit_len += s;
            } while (s == 255);
        }

        if (lit_len > in_len - ip || ensure_capacity(out, out_cap, op + lit_len) != 0)
            return -1;
        memcpy(*out + op, in + ip, lit_len);
        ip += lit_len;
        op += lit_len;

        if (ip == in_len) {
            *out_len = op;
            return 0;
        }
        if (ip + 2 > in_len)
            return -1;

        size_t offset = (size_t)in[ip] | ((size_t)in[ip + 1] << 8);
        ip += 2;
        if (offset == 0 || offset > op)
            return -1;

        match_len += 4;
        if (match_len == 19) {
            uint8_t s;
            do {
                if (ip >= in_len || match_len > SIZE_MAX - 255)
                    return -1;
                s = in[ip++];
                match_len += s;
            } while (s == 255);
        }

        if (ensure_capacity(out, out_cap, op + match_len) != 0)
            return -1;
        for (size_t i = 0; i < match_len; i++) {
            (*out)[op] = (*out)[op - offset];
            op++;
        }
    }

    *out_len = op;
    return 0;
}

uint8_t* decompress_lz4_legacy(const uint8_t *in, size_t in_len, size_t *out_len) {
    if (!in || !out_len || in_len < 8 ||
        in[0] != 0x02 || in[1] != 0x21 || in[2] != 0x4C || in[3] != 0x18)
        return NULL;

    *out_len = 0;
    size_t pos = 4;
    size_t out_cap = in_len * 4;
    if (out_cap < 65536)
        out_cap = 65536;
    uint8_t *out = malloc(out_cap);
    if (!out)
        return NULL;

    while (pos + 4 <= in_len) {
        uint32_t block_size = read_le32(in + pos);
        pos += 4;
        if (block_size == 0)
            break;
        if (block_size > in_len - pos) {
            free(out);
            return NULL;
        }
        if (lz4_block_decode(in + pos, block_size, &out, out_len, &out_cap) != 0) {
            free(out);
            return NULL;
        }
        pos += block_size;
    }

    if (*out_len == 0) {
        free(out);
        return NULL;
    }
    return out;
}
