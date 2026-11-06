/**
 * ziputil.c -- Minimal ZIP parser for stored entries only.
 *
 * Used to find payload.bin offset inside OTA ZIP files.
 * Only handles STORED (method=0) entries - no DEFLATE decompression.
 */
#include "ziputil.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ZIP structures (little-endian) */

typedef struct {
    uint32_t signature;       /* 0x04034b50 */
    uint16_t version_needed;
    uint16_t flags;
    uint16_t method;          /* 0=stored */
    uint16_t mod_time;
    uint16_t mod_date;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_len;
    uint16_t extra_len;
    /* filename[filename_len] */
    /* extra[extra_len] */
} __attribute__((packed)) LocalFileHeader;

typedef struct {
    uint32_t signature;       /* 0x02014b50 */
    uint16_t version_made;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t method;          /* 0=stored */
    uint16_t mod_time;
    uint16_t mod_date;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_len;
    uint16_t extra_len;
    uint16_t comment_len;
    uint16_t disk_start;
    uint16_t internal_attrs;
    uint32_t external_attrs;
    uint32_t local_header_offset;
    /* filename[filename_len] */
    /* extra[extra_len] */
    /* comment[comment_len] */
} __attribute__((packed)) CentralDirEntry;

typedef struct {
    uint32_t signature;       /* 0x06054b50 */
    uint16_t disk_num;
    uint16_t disk_start;
    uint16_t num_entries_disk;
    uint16_t num_entries_total;
    uint32_t central_dir_size;
    uint32_t central_dir_offset;
    uint16_t comment_len;
    /* comment[comment_len] */
} __attribute__((packed)) EOCD;

typedef struct {
    uint32_t signature;       /* 0x06064b50 */
    uint64_t size;            /* size of record (minus sig+size) */
    uint16_t version_made;
    uint16_t version_needed;
    uint32_t disk_num;
    uint32_t disk_start;
    uint64_t num_entries_disk;
    uint64_t num_entries_total;
    uint64_t central_dir_size;
    uint64_t central_dir_offset;
    /* extensible data follows */
} __attribute__((packed)) Zip64EOCD;

typedef struct {
    uint32_t signature;       /* 0x07064b50 */
    uint32_t disk_with_eocd;
    uint64_t eocd_offset;
    uint32_t total_disks;
} __attribute__((packed)) Zip64EOCDLocator;

int zip_find_stored_entry(const uint8_t *data, size_t datasize,
                          const char *entry_name,
                          uint64_t *entry_offset, uint64_t *entry_size)
{
    if (datasize < sizeof(EOCD)) return -1;

    /* Find EOCD by scanning backwards for the magic */
    const EOCD *eocd = NULL;
    uint64_t eocd_offset = 0;
    int64_t scan_start = (int64_t)datasize - 65557;
    if (scan_start < 0) scan_start = 0;
    
    
    for (int64_t pos = (int64_t)datasize - sizeof(EOCD); pos >= scan_start; pos--) {
        if (*(uint32_t*)(data + pos) == 0x06054b50) {
            const EOCD *candidate = (const EOCD*)(data + pos);
            uint64_t record_end = (uint64_t)pos + sizeof(EOCD) + candidate->comment_len;
            if (record_end == datasize) {
                eocd = candidate;
                eocd_offset = (uint64_t)pos;
                break;
            }
        }
    }
    if (!eocd) {
        fprintf(stderr, "Error: ZIP EOCD not found\n");
        return -1;
    }
    

    /* Check for ZIP64 */
    uint64_t cd_offset = eocd->central_dir_offset;
    uint64_t cd_size = eocd->central_dir_size;
    uint64_t num_entries = eocd->num_entries_total;
    

    /* Look for ZIP64 EOCD locator before EOCD */
    if (eocd_offset >= sizeof(Zip64EOCDLocator)) {
        const Zip64EOCDLocator *loc = (const Zip64EOCDLocator*)(data + eocd_offset - sizeof(Zip64EOCDLocator));
        uint32_t loc_sig = loc->signature;
        if (loc_sig == 0x07064b50) {
            if (loc->eocd_offset <= datasize &&
                sizeof(Zip64EOCD) <= datasize - loc->eocd_offset) {
                const Zip64EOCD *z64 = (const Zip64EOCD*)(data + (size_t)loc->eocd_offset);
                if (z64->signature == 0x06064b50) {
                    cd_offset = (uint64_t)z64->central_dir_offset;
                    cd_size = (uint64_t)z64->central_dir_size;
                    num_entries = z64->num_entries_total;
                } else {
                
                }
            } else {
            
            }
        }
    } else {
    
    }

    if (cd_offset > datasize || cd_size > datasize - cd_offset) {
        fprintf(stderr, "Error: central directory out of bounds\n");
        return -1;
    }

    /* Search central directory for entry */
    const uint8_t *cd = data + cd_offset;
    size_t cd_pos = 0;
    size_t namelen = strlen(entry_name);

    for (uint64_t i = 0; i < num_entries; i++) {
        if (cd_pos + sizeof(CentralDirEntry) > cd_size) break;
        const CentralDirEntry *entry = (const CentralDirEntry*)(cd + cd_pos);
        if (entry->signature != 0x02014b50) break;

        uint16_t fnlen = entry->filename_len;
        uint16_t exlen = entry->extra_len;
        uint16_t clen = entry->comment_len;
        uint64_t entry_len = sizeof(CentralDirEntry) +
                             (uint64_t)fnlen + exlen + clen;
        if (entry_len > cd_size - cd_pos) break;

        /* Resolve ZIP64 fields from extra data if present */
        uint64_t comp_size = entry->compressed_size;
        uint64_t uncomp_size = entry->uncompressed_size;
        uint64_t lfh_off = entry->local_header_offset;
        uint16_t disk_start = entry->disk_start;
        int is_z64 = (comp_size == 0xFFFFFFFF || uncomp_size == 0xFFFFFFFF ||
                       lfh_off == 0xFFFFFFFF || disk_start == 0xFFFF);

        if (is_z64 && exlen > 0) {
    
            const uint8_t *ep = cd + cd_pos + sizeof(CentralDirEntry) + fnlen;
            const uint8_t *ee = ep + exlen;
            while (ee - ep >= 4) {
                uint16_t hdr_id = *(uint16_t*)ep;
                uint16_t fld_sz = *(uint16_t*)(ep + 2);
                if ((uint32_t)(fld_sz) + (uint32_t)(ep - (cd + cd_pos + sizeof(CentralDirEntry) + fnlen)) > exlen) break;
                if (hdr_id == 1) { /* ZIP64 extended information */
                    const uint8_t *ed = ep + 4;
                    const uint8_t *ed_end = ed + fld_sz;
                    /* Read ZIP64 fields in order: uncomp_size, comp_size, lfh_off, disk_start */
                    if (uncomp_size == 0xFFFFFFFF && ed + 8 <= ed_end) {
                        uncomp_size = *(uint64_t*)ed;
                        ed += 8;
                    }
                    if (comp_size == 0xFFFFFFFF && ed + 8 <= ed_end) {
                        comp_size = *(uint64_t*)ed;
                        ed += 8;
                    }
                    if (lfh_off == 0xFFFFFFFF && ed + 8 <= ed_end) {
                        lfh_off = *(uint64_t*)ed;
                        ed += 8;
                    }
                    if (disk_start == 0xFFFF && ed + 4 <= ed_end) {
                        disk_start = *(uint16_t*)ed;
                        ed += 4;
                    }
                }
                ep += 4 + fld_sz;
            }
        }

        if (fnlen == namelen && cd_pos + sizeof(CentralDirEntry) + fnlen <= cd_size) {
            const char *fname = (const char*)(cd + cd_pos + sizeof(CentralDirEntry));
            /* Debug: print entry info */
            if (memcmp(fname, entry_name, namelen) == 0) {
                if (entry->method != 0) {
                    fprintf(stderr, "Error: ZIP entry %s is compressed (method=%u), only stored entries are supported\n",
                            entry_name, entry->method);
                    return -1;
                }
                /* Found! Use resolved ZIP64 values for offset and size */
                uint64_t local_off_val = lfh_off;  /* resolved ZIP64 value */
                if (local_off_val > datasize ||
                    sizeof(LocalFileHeader) > datasize - local_off_val) return -1;
                const LocalFileHeader *local = (const LocalFileHeader*)(data + local_off_val);
                if (local->signature != 0x04034b50) return -1;
                
                uint64_t header_len = sizeof(LocalFileHeader) +
                                      (uint64_t)local->filename_len + local->extra_len;
                if (header_len > datasize - local_off_val) return -1;
                uint64_t data_off = local_off_val + header_len;
                if (uncomp_size > datasize - data_off) return -1;
                *entry_offset = data_off;
                /* Use uncompressed size (stored = same as compressed) */
                *entry_size = uncomp_size;
                
                return 0;
            }
        }

        cd_pos += (size_t)entry_len;
    }

    return -1;
}


/* Find a stored entry in a ZIP file accessible via HTTP range reader. */
int zip_find_stored_entry_http(uint64_t file_size,
                                int (*http_read_fn)(void *ctx, uint64_t offset, uint8_t *buf, size_t size),
                                void *http_ctx,
                                const char *entry_name,
                                uint64_t *out_offset, uint64_t *out_size) {
    /* Read last 64KB to find EOCD record */
    uint64_t eocd_search = file_size > 65536 ? file_size - 65536 : 0;
    uint64_t eocd_size = (uint64_t)(file_size - eocd_search);
    if (eocd_size < sizeof(EOCD)) return -1;
    uint8_t *eocd_buf = malloc((size_t)eocd_size + 4);
    if (!eocd_buf) return -1;
    
if (http_read_fn(http_ctx, eocd_search, eocd_buf, (size_t)eocd_size) != 0) {
        free(eocd_buf);
        return -1;
    }
    
    /* Find EOCD signature 0x06054b50 */
    int found = -1;
    uint64_t eocd_abs = 0;
    for (uint64_t i = eocd_size - sizeof(EOCD) + 1; i-- > 0;) {
        if (eocd_buf[i] == 0x50 && i + 3 < eocd_size &&
            eocd_buf[i+1] == 0x4b && eocd_buf[i+2] == 0x05 && eocd_buf[i+3] == 0x06) {
            if (i + sizeof(EOCD) > eocd_size) continue;
            uint16_t comment_len = (uint16_t)eocd_buf[i + 20] |
                                   ((uint16_t)eocd_buf[i + 21] << 8);
            if (i + sizeof(EOCD) + comment_len != eocd_size) continue;
            eocd_abs = eocd_search + i;
            found = 0;
            break;
        }
    }
if (found != 0) { free(eocd_buf); return -1; }
    
    uint64_t eocd_rel = eocd_abs - eocd_search;
    
    /* Parse EOCD */
    uint32_t cd_size32, cd_offset32, cd_entries32;
    memcpy(&cd_size32, eocd_buf + eocd_rel + 12, 4);
    memcpy(&cd_offset32, eocd_buf + eocd_rel + 16, 4);
    memcpy(&cd_entries32, eocd_buf + eocd_rel + 8, 4);
    
    uint64_t cd_offset = cd_offset32;
    uint64_t cd_size = cd_size32;
    
    /* Check for Zip64 */
    if (cd_offset32 == 0xFFFFFFFF || cd_size32 == 0xFFFFFFFF) {
        /* Find Zip64 EOCD locator */
        for (uint64_t i = eocd_rel; i >= 20; i--) {
            uint32_t loc_sig;
            memcpy(&loc_sig, eocd_buf + i - 20, 4);
            if (loc_sig == 0x07064b50) {
                uint64_t z64_eocd_off;
                memcpy(&z64_eocd_off, eocd_buf + i - 12, 8);
                uint8_t z64[56];
                if (http_read_fn(http_ctx, z64_eocd_off, z64, 56) == 0) {
                    memcpy(&cd_size, z64 + 40, 8);
                    memcpy(&cd_offset, z64 + 48, 8);
                }
                break;
            }
        }
    }
    
    free(eocd_buf);
    
if (cd_size == 0 || cd_offset > file_size || cd_size > file_size - cd_offset ||
    cd_size > SIZE_MAX)
        return -1;
    
uint8_t *cd = malloc((size_t)cd_size);
    if (!cd) return -1;
    if (http_read_fn(http_ctx, cd_offset, cd, (size_t)cd_size) != 0) {
free(cd);
        return -1;
    }
    

    
    /* Scan central directory entries. */
    int name_len = (int)strlen(entry_name);
    uint64_t pos = 0;
    uint8_t *entry_found = NULL;
    while (pos + 46 <= cd_size) {
        if (cd[pos] != 0x50 || cd[pos+1] != 0x4b || cd[pos+2] != 0x01 || cd[pos+3] != 0x02)
            break;

        uint16_t fn = (uint16_t)(cd[pos+28]) | ((uint16_t)(cd[pos+29]) << 8);
        uint16_t ef_len = (uint16_t)(cd[pos+30]) | ((uint16_t)(cd[pos+31]) << 8);
        uint16_t comment_len = (uint16_t)(cd[pos+32]) | ((uint16_t)(cd[pos+33]) << 8);
        uint16_t method = (uint16_t)(cd[pos+10]) | ((uint16_t)(cd[pos+11]) << 8);
        uint64_t entry_len = 46ULL + fn + ef_len + comment_len;
        if (pos + entry_len > cd_size)
            break;

        if (fn == name_len && memcmp(cd + pos + 46, entry_name, name_len) == 0) {
            if (method != 0) {
                fprintf(stderr, "Error: ZIP entry %s is compressed (method=%u), only stored entries are supported\n",
                        entry_name, method);
                free(cd);
                return -1;
            }
            entry_found = cd + pos;
            break;
        }

        pos += entry_len;
    }
    
    if (!entry_found) {
        free(cd);
        return -1;
    }
    
    
    /* Read fields */
    uint16_t fn_len = (uint16_t)(cd[pos+28]) | ((uint16_t)(cd[pos+29]) << 8);
    uint16_t ef_len = (uint16_t)(cd[pos+30]) | ((uint16_t)(cd[pos+31]) << 8);

    uint32_t local_off32, csize32, usize32;
    memcpy(&csize32, cd + pos + 20, 4);
    memcpy(&usize32, cd + pos + 24, 4);
    memcpy(&local_off32, cd + pos + 42, 4);
    uint64_t local_off = local_off32;
    uint64_t csize = csize32;
    uint64_t usize = usize32;
    (void)csize;

    /* Handle Zip64 */
    if (local_off32 == 0xFFFFFFFF || csize32 == 0xFFFFFFFF || usize32 == 0xFFFFFFFF) {
        uint64_t ep = pos + 46 + fn_len;
        uint64_t ee = ep + ef_len;
        while (ee >= ep + 4) {
            uint16_t eid = (uint16_t)(cd[ep]) | ((uint16_t)(cd[ep+1]) << 8);
            uint16_t esz = (uint16_t)(cd[ep+2]) | ((uint16_t)(cd[ep+3]) << 8);
            if (ep + 4 + esz > ee)
                break;
            if (eid == 0x0001) {
                uint64_t off = ep + 4;
                if (usize32 == 0xFFFFFFFF && off + 8 <= ep + 4 + esz) {
                    memcpy(&usize, cd + off, 8);
                    off += 8;
                }
                if (csize32 == 0xFFFFFFFF && off + 8 <= ep + 4 + esz) {
                    memcpy(&csize, cd + off, 8);
                    off += 8;
                }
                if (local_off32 == 0xFFFFFFFF && off + 8 <= ep + 4 + esz) {
                    memcpy(&local_off, cd + off, 8);
                    off += 8;
                }
                break;
            }
            ep += 4 + esz;
        }
    }

    *out_offset = local_off;
    *out_size = usize;

    /* Compute actual data offset (skip local file header) */
    uint8_t lfh[30];
    if (local_off > file_size || sizeof(lfh) > file_size - local_off) {
        free(cd); return -1;
    }
    if (http_read_fn(http_ctx, local_off, lfh, 30) != 0) {
        free(cd); return -1;
    }
    uint16_t lfn_len = (uint16_t)(lfh[26]) | ((uint16_t)(lfh[27]) << 8);
    uint16_t lef_len = (uint16_t)(lfh[28]) | ((uint16_t)(lfh[29]) << 8);
    uint64_t local_header_size = 30ULL + lfn_len + lef_len;
    if (local_header_size > file_size - local_off ||
        usize > file_size - (local_off + local_header_size)) {
        free(cd); return -1;
    }
    *out_offset = local_off + local_header_size;
    
    free(cd);
    return 0;
}
