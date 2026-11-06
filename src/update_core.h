#ifndef PD_UPDATE_CORE_H
#define PD_UPDATE_CORE_H
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include "version.h"
#define UP_PATH 32768
#define UP_URL 2048
#define UP_MAX_URLS 16
#define UP_MAX_ZIP (32U*1024U*1024U)
#define UP_MAX_META (1024U*1024U)
#define UP_MAX_NOTES 32768
#ifdef _WIN64
#define UP_ARCH "x64"
#define UP_ARCH_W L"x64"
#define UP_MACHINE 0x8664
#else
#define UP_ARCH "x86"
#define UP_ARCH_W L"x86"
#define UP_MACHINE 0x14c
#endif
typedef struct {
    uint64_t revision;
    char sha256[65];
    wchar_t manifest_url[UP_URL];
} UpPointer;
typedef struct {
    uint64_t revision, version, forced;
    wchar_t version_name[64], published[64], release_url[UP_URL];
    wchar_t notes[UP_MAX_NOTES];
    char sha256[65];
    DWORD size;
    int url_count;
    wchar_t urls[UP_MAX_URLS][UP_URL];
} UpManifest;
BOOL up_sha_memory(const void *data, size_t size, char out[65]);
BOOL up_sha_file(const wchar_t *path, char out[65]);
BOOL up_parse_pointer(const char *json, size_t len, UpPointer *out);
BOOL up_parse_manifest(const char *json, size_t len, UpManifest *out);
BOOL up_select_pointer(const UpPointer *pointers, const BOOL *valid, size_t count,
                       uint64_t floor, const char *accepted_hash, UpPointer *out);
BOOL up_join(wchar_t *out, const wchar_t *dir, const wchar_t *name);
BOOL up_new_temp(wchar_t *dir);
BOOL up_quote(wchar_t *cmd, size_t cap, const wchar_t *arg);
BOOL up_extract_zip(const wchar_t *zip, const wchar_t *stage, uint64_t expected_version);
BOOL up_verify_exe(const wchar_t *file, uint64_t version, BOOL main_exe);
BOOL up_local_time(const wchar_t *iso, wchar_t *out, size_t cap);
extern const wchar_t *const up_package_files[];
extern const size_t up_package_file_count;
#endif
