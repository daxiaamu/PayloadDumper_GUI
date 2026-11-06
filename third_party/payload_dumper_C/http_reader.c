/**
 * http_reader.c -- HTTP range request reader
 *
 * Each read creates a fresh WinHTTP session/connect/request.
 * No connection reuse to avoid CDN stickiness issues.
 * Based on proven behavior from test_winhttp.exe.
 */
#include "http_reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winhttp.h>

static int http_set_url(HttpReader *hr, const char *url) {
    if (!url || strlen(url) >= sizeof(hr->url)) return -1;
    strncpy(hr->url, url, sizeof(hr->url) - 1);
    hr->url[sizeof(hr->url) - 1] = '\0';

    /* Parse URL */
    const char *host_start, *path_start;
    hr->use_tls = 0;
    hr->port = 80;
    if (strncmp(url, "https://", 8) == 0) { hr->use_tls = 1; hr->port = 443; host_start = url + 8; }
    else if (strncmp(url, "http://", 7) == 0) { host_start = url + 7; }
    else return -1;
    path_start = strchr(host_start, '/');
    if (!path_start) return -1;

    const char *host_end = path_start;
    const char *port_sep = NULL;
    for (const char *p = host_start; p < host_end; p++) {
        if (*p == ':') port_sep = p;
    }
    if (port_sep) {
        char *parse_end = NULL;
        unsigned long parsed_port = strtoul(port_sep + 1, &parse_end, 10);
        if (parse_end != host_end || parsed_port == 0 || parsed_port > 65535)
            return -1;
        hr->port = (int)parsed_port;
        host_end = port_sep;
    }

    ptrdiff_t host_len = host_end - host_start;
    if (host_len <= 0 || host_len >= (ptrdiff_t)sizeof(hr->host)) return -1;
    int hl = (int)host_len;
    memcpy(hr->host, host_start, hl);
    hr->host[hl] = '\0';

    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, hr->host, -1,
                            hr->host_w, 256) <= 0 ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path_start, -1,
                            hr->path_w, 2048) <= 0)
        return -1;
    return 0;
}

static int http_probe_size(HttpReader *hr) {
    /* Do a HEAD-style request to get file size */
    /* Since WinHTTP doesn't easily do HEAD, use Range: 0-0 */
    HINTERNET hSession = WinHttpOpen(L"payload_dumper/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return -1;
    
    HINTERNET hConnect = WinHttpConnect(hSession, hr->host_w, hr->port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return -1; }
    
    DWORD flags = hr->use_tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", hr->path_w, NULL, NULL, NULL, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return -1; }
    
    WinHttpSetTimeouts(hRequest, 30000, 30000, 30000, 30000);
    
    LPCWSTR hdrs = L"Range: bytes=0-0\r\n";
    if (!WinHttpSendRequest(hRequest, hdrs, (DWORD)wcslen(hdrs), NULL, 0, 0, 0)
        || !WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }
    
    DWORD status = 0, ss = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        NULL, &status, &ss, NULL);
    hr->file_size = 0;
    if (status == 206) {
        wchar_t cr[128]; DWORD crs = sizeof(cr);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_RANGE,
                                NULL, cr, &crs, NULL)) {
            unsigned long long start = 0, finish = 0, total = 0;
            if (swscanf(cr, L"bytes %llu-%llu/%llu", &start, &finish, &total) == 3 &&
                start == 0 && finish == 0 && total > 0)
                hr->file_size = (uint64_t)total;
        }
    }
    
    /* Consume and discard the 1 byte response */
    uint8_t tmp[64]; DWORD rd;
    WinHttpReadData(hRequest, tmp, sizeof(tmp), &rd);
    
    /* Close all handles */
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (hr->file_size == 0) return -1;
    hr->connected = 1;
    return 0;
}

static int query_downloadcheck_location(const char *url, char *location, size_t location_size) {
    HttpReader tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (http_set_url(&tmp, url) != 0) return -1;

    HINTERNET hSession = WinHttpOpen(L"payload_dumper/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return -1;

    HINTERNET hConnect = WinHttpConnect(hSession, tmp.host_w, tmp.port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return -1; }

    DWORD flags = tmp.use_tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"HEAD", tmp.path_w,
                                            NULL, NULL, NULL, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    DWORD disable = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    WinHttpSetTimeouts(hRequest, 30000, 30000, 30000, 30000);

    LPCWSTR hdrs = L"userid: oplus-ota|\r\n";
    if (!WinHttpSendRequest(hRequest, hdrs, (DWORD)wcslen(hdrs), NULL, 0, 0, 0)
        || !WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    wchar_t loc_w[2048];
    DWORD loc_size = sizeof(loc_w);
    int ok = -1;
    if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                            NULL, loc_w, &loc_size, NULL)) {
        int n = WideCharToMultiByte(CP_UTF8, 0, loc_w, -1,
                                    location, (int)location_size, NULL, NULL);
        if (n > 0) ok = 0;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

static int refresh_downloadcheck_location(HttpReader *hr) {
    char location[2048];
    if (!hr->using_downloadcheck) return -1;
    if (query_downloadcheck_location(hr->source_url, location, sizeof(location)) != 0)
        return -1;
    if (http_set_url(hr, location) != 0)
        return -1;
    return http_probe_size(hr);
}

int http_open(HttpReader *hr, const char *url) {
    memset(hr, 0, sizeof(*hr));
    if (!url || strlen(url) >= sizeof(hr->source_url)) return -1;
    strncpy(hr->source_url, url, sizeof(hr->source_url) - 1);
    hr->source_url[sizeof(hr->source_url) - 1] = '\0';

    if (http_set_url(hr, url) == 0 && http_probe_size(hr) == 0)
        return 0;

    char location[2048];
    if (query_downloadcheck_location(url, location, sizeof(location)) == 0) {
        memset(hr, 0, sizeof(*hr));
        strncpy(hr->source_url, url, sizeof(hr->source_url) - 1);
        hr->source_url[sizeof(hr->source_url) - 1] = '\0';
        hr->using_downloadcheck = 1;
        if (http_set_url(hr, location) == 0 && http_probe_size(hr) == 0)
            return 0;
    }

    return -1;
}

static int http_read_once(HttpReader *hr, uint64_t offset, uint8_t *buf, size_t size) {
    if (!hr->connected || size == 0) return -1;
    if (offset > hr->file_size || (uint64_t)size > hr->file_size - offset)
        return -1;

    /* Create fresh session for each read */
    HINTERNET hSession = WinHttpOpen(L"payload_dumper/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return -1;

    HINTERNET hConnect = WinHttpConnect(hSession, hr->host_w, hr->port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return -1; }

    DWORD flags = hr->use_tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", hr->path_w,
                                            NULL, NULL, NULL, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    WinHttpSetTimeouts(hRequest, 60000, 60000, 30000, 30000);
    
    wchar_t range_hdr[256];
    swprintf(range_hdr, 256, L"Range: bytes=%I64u-%I64u\r\n",
             (unsigned long long)offset, (unsigned long long)(offset + size - 1));

    if (!WinHttpSendRequest(hRequest, range_hdr, (DWORD)wcslen(range_hdr),
                            NULL, 0, 0, 0)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    DWORD status = 0, ss = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        NULL, &status, &ss, NULL);
    if (status != 206) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    wchar_t content_range[128];
    DWORD content_range_size = sizeof(content_range);
    unsigned long long range_start = 0, range_end = 0, range_total = 0;
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_RANGE, NULL,
                             content_range, &content_range_size, NULL) ||
        swscanf(content_range, L"bytes %llu-%llu/%llu",
                &range_start, &range_end, &range_total) != 3 ||
        range_start != offset || range_end < range_start ||
        range_end - range_start + 1 != (uint64_t)size ||
        range_total != hr->file_size) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    /* Read data */
    size_t total = 0;
    uint8_t *p = buf;
    while (total < size) {
        DWORD read = 0;
        size_t remaining = size - total;
        DWORD chunk = (DWORD)(remaining > 0x40000000ULL ? 0x40000000ULL : remaining);
        if (!WinHttpReadData(hRequest, p, chunk, &read)) break;
        if (read == 0) break;
        total += read;
        p += read;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return (total == size) ? 0 : -1;
}

int http_read(HttpReader *hr, uint64_t offset, uint8_t *buf, size_t size) {
    if (http_read_once(hr, offset, buf, size) == 0)
        return 0;

    if (refresh_downloadcheck_location(hr) == 0)
        return http_read_once(hr, offset, buf, size);

    return -1;
}

uint64_t http_size(HttpReader *hr) { return hr->file_size; }

void http_close(HttpReader *hr) {
    hr->connected = 0;
}
