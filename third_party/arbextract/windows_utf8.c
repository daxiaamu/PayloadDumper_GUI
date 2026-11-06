/* Windows path adapter for koaaN/arbextract. The upstream parser is unchanged.
 * Upstream main/fopen use the active ANSI codepage. Convert the wide Windows
 * command line to UTF-8 and use _wfopen for filesystem access. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
static FILE *utf8_fopen(const char *path, const char *mode) {
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (!size) return NULL;
    wchar_t *wide = malloc((size_t)size * sizeof(wchar_t));
    wchar_t wmode[16];
    if (!wide) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, size);
    if (!MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 16)) { free(wide); return NULL; }
    FILE *f = _wfopen(wide, wmode);
    free(wide); return f;
}
#define fopen utf8_fopen
#define main arbextract_main
#include "upstream/arbextract.c"
#undef main
#undef fopen
int main(void) {
    int argc;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide) return 1;
    char **args = calloc((size_t)argc+1, sizeof(char*));
    if (!args) { LocalFree(wide); return 1; }
    int rc = 1, i;
    for (i=0; i<argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
        args[i] = malloc((size_t)n);
        if (!args[i]) goto done;
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, args[i], n, NULL, NULL);
    }
    rc = arbextract_main(argc, args);
done:
    for (i=0; i<argc; ++i) free(args[i]);
    free(args); LocalFree(wide); return rc;
}
