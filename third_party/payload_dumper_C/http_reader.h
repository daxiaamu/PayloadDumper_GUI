#ifndef HTTP_READER_H
#define HTTP_READER_H

#include <stdint.h>
#include <stddef.h>
#include <windows.h>
#include <winhttp.h>

/* HTTP range reader for OTA payloads */
typedef struct {
    char     source_url[2048]; /* original URL passed by user */
    char     url[2048];
    char     host[256];
    char     path[2048];
    wchar_t  host_w[256];
    wchar_t  path_w[2048];
    int      use_tls;
    int      port;
    HINTERNET hSession;
    HINTERNET hConnect;
    HINTERNET hRequest;
    uint64_t file_size;   /* total file size from Content-Range */
    int      connected;
    int      using_downloadcheck;
} HttpReader;

/* Open HTTP connection. Returns 0 on success. */
int http_open(HttpReader *hr, const char *url);

/* Read data at offset. Returns 0 on success. */
int http_read(HttpReader *hr, uint64_t offset, uint8_t *buf, size_t size);

/* Get total file size */
uint64_t http_size(HttpReader *hr);

/* Close connection */
void http_close(HttpReader *hr);

#endif
