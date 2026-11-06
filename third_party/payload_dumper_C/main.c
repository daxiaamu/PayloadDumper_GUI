/**
 * payload_dumper.c -- CLI entry point
 * Dump Android OTA payload.bin partition images.
 * Uses wide command line for full Unicode (Chinese) path support.
 * Manual argument parser (no getopt dependency).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>

#include "payload.h"
#include "ziputil.h"
#include "dumper.h"

static int stdout_is_console(void) {
    DWORD mode;
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    return output != INVALID_HANDLE_VALUE && output != NULL &&
           GetConsoleMode(output, &mode);
}

static void restore_console_cp(UINT old_in_cp, UINT old_out_cp) {
    if (old_in_cp != 0)
        SetConsoleCP(old_in_cp);
    if (old_out_cp != 0)
        SetConsoleOutputCP(old_out_cp);
}

static void print_banner(FILE *stream) {
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO old_info;
    BOOL has_old_info = FALSE;

    if (stream == stdout && console != INVALID_HANDLE_VALUE && console != NULL) {
        has_old_info = GetConsoleScreenBufferInfo(console, &old_info);
        SetConsoleTextAttribute(console,
            (old_info.wAttributes & (BACKGROUND_BLUE | BACKGROUND_GREEN |
                                     BACKGROUND_RED | BACKGROUND_INTENSITY)) |
            FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    }

    fprintf(stream,
        "\n"
        "██████╗  █████╗ ██╗  ██╗██╗ █████╗  █████╗ ███╗   ███╗██╗   ██╗\n"
        "██╔══██╗██╔══██╗╚██╗██╔╝██║██╔══██╗██╔══██╗████╗ ████║██║   ██║\n"
        "██║  ██║███████║ ╚███╔╝ ██║███████║███████║██╔████╔██║██║   ██║\n"
        "██║  ██║██╔══██║ ██╔██╗ ██║██╔══██║██╔══██║██║╚██╔╝██║██║   ██║\n"
        "██████╔╝██║  ██║██╔╝ ██╗██║██║  ██║██║  ██║██║ ╚═╝ ██║╚██████╔╝\n"
        "╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═╝╚═╝╚═╝  ╚═╝╚═╝  ╚═╝╚═╝     ╚═╝ ╚═════╝ \n"
        "\n");
    fflush(stream);

    if (has_old_info)
        SetConsoleTextAttribute(console, old_info.wAttributes);
}

static void print_usage(FILE *stream, const char *prog, int num_cpus) {
    fprintf(stream,
        "usage: %s [-h] [-o OUT] [-p PARTITIONS] [-w WORKERS]\n"
        "           [--list] [--metadata] [--kernel-version] [--arb] payloadfile\n"
        "\n"
        "OTA payload dumper\n"
        "\n"
        "positional arguments:\n"
        "  payloadfile           payload file name\n"
        "\n"
        "options:\n"
        "  -h, --help\n"
        "      show this help message and exit\n"
        "  -o, --out OUT\n"
        "      output directory (default: 'output')\n"
        "  -p, --partitions PARTITIONS\n"
        "      comma separated list of partitions to extract\n"
        "      (default: extract all)\n"
        "  -w, --workers WORKERS\n"
        "      number of workers (default: CPU count - %d)\n"
        "  --list, -l\n"
        "      list partitions in the payload file\n"
        "  --metadata, -m\n"
        "      extract and display metadata file from the payload\n"
        "  --kernel-version\n"
        "      extract kernel release version from boot image\n"
        "  --arb\n"
        "      read anti-rollback version from the ROM\n",
        prog ? prog : "payload_dumper",
        num_cpus);
}

int main() {
    UINT old_in_cp = GetConsoleCP();
    UINT old_out_cp = GetConsoleOutputCP();
    int have_console_stdout = stdout_is_console();

    if (have_console_stdout) {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }

    /* Get wide command line (bypasses ANSI codepage corruption) */
    int argc_w;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc_w);
    if (!wargv) {
        fprintf(stderr, "FATAL: CommandLineToArgvW failed\n");
        restore_console_cp(old_in_cp, old_out_cp);
        return 1;
    }

    /* Convert all args to UTF-8 */
    char **argv_u8 = malloc((argc_w + 1) * sizeof(char*));
    if (!argv_u8) {
        LocalFree(wargv);
        restore_console_cp(old_in_cp, old_out_cp);
        return 1;
    }
    for (int i = 0; i < argc_w; i++) {
        const wchar_t *ws = wargv[i];
        if (!ws) { argv_u8[i] = NULL; continue; }
        int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
        argv_u8[i] = malloc(len);
        if (argv_u8[i])
            WideCharToMultiByte(CP_UTF8, 0, ws, -1, argv_u8[i], len, NULL, NULL);
    }
    argv_u8[argc_w] = NULL;
    LocalFree(wargv);

    if (have_console_stdout)
        print_banner(stdout);

    /* Manual argv parsing (no getopt dependency) */
    const char *out_dir = "output";
    const char *partitions = NULL;
    const char *payload_path = NULL;
    int list_only = 0;
    int metadata_only = 0;
    int kernel_version_only = 0;
    int arb_only = 0;
    int num_threads = 0;
    /* Parse all args in any order (like Python argparse) */
    int payload_found = 0;
    for (int i = 1; i < argc_w; i++) {
        if (!argv_u8[i] || !argv_u8[i][0]) continue;

        /* If it doesn't start with '-', it's the positional payload path */
        if (argv_u8[i][0] != '-') {
            if (!payload_found) {
                payload_path = argv_u8[i];
                payload_found = 1;
            }
            continue;
        }

        const char *arg = argv_u8[i];
        /* Skip redundant leading dashes */
        if (arg[0] == '-' && arg[1] == '-') arg += 2;
        else if (arg[0] == '-') arg += 1;

        if (strcmp(arg, "h") == 0 || strcmp(arg, "help") == 0) {
            SYSTEM_INFO info;
            GetSystemInfo(&info);
            int num_cpus = (int)info.dwNumberOfProcessors;
            print_usage(stdout, argv_u8[0], num_cpus);
            goto cleanup;
        } else if (strcmp(arg, "m") == 0 || strcmp(arg, "metadata") == 0) {
            metadata_only = 1;
        } else if (strcmp(arg, "kernel-version") == 0) {
            kernel_version_only = 1;
        } else if (strcmp(arg, "arb") == 0) {
            arb_only = 1;
        } else if (strcmp(arg, "l") == 0 || strcmp(arg, "list") == 0) {
            list_only = 1;
        } else if (strcmp(arg, "o") == 0 || strcmp(arg, "out") == 0) {
            i++;
            if (i >= argc_w) {
                fprintf(stderr, "error: argument --out: expected one argument\n");
                goto cleanup_err;
            }
            out_dir = argv_u8[i];
        } else if (strcmp(arg, "p") == 0 || strcmp(arg, "partitions") == 0) {
            i++;
            if (i >= argc_w) {
                fprintf(stderr, "error: argument --partitions: expected one argument\n");
                goto cleanup_err;
            }
            partitions = argv_u8[i];
        } else if (strcmp(arg, "w") == 0 || strcmp(arg, "workers") == 0) {
            i++;
            if (i >= argc_w) {
                fprintf(stderr, "Error: --workers requires an argument\n");
                goto cleanup_err;
            }
            char *endptr = NULL;
            long workers = strtol(argv_u8[i], &endptr, 10);
            if (!argv_u8[i][0] || !endptr || *endptr != '\0' ||
                workers < 1 || workers > 256) {
                fprintf(stderr, "Error: --workers must be an integer from 1 to 256\n");
                goto cleanup_err;
            }
            num_threads = (int)workers;
        } else {
            fprintf(stderr, "error: unrecognized arguments: %s\n", argv_u8[i]);
            {
                SYSTEM_INFO info;
                GetSystemInfo(&info);
                print_usage(stderr, argv_u8[0], (int)info.dwNumberOfProcessors);
            }
            goto cleanup_err;
        }
    }

    if (!payload_found) {
        fprintf(stderr, "error: the following arguments are required: payloadfile\n");
        {
            SYSTEM_INFO info;
            GetSystemInfo(&info);
            print_usage(stderr, argv_u8[0], (int)info.dwNumberOfProcessors);
        }
        goto cleanup_err;
    }

    if (num_threads <= 0) {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        num_threads = (int)info.dwNumberOfProcessors;
        if (num_threads > 256) num_threads = 256;
    }

    int rc = dumper_run(payload_path, out_dir, partitions, num_threads,
                        list_only, metadata_only, kernel_version_only, arb_only);

    for (int i = 0; i < argc_w; i++) free(argv_u8[i]);
    free(argv_u8);
    restore_console_cp(old_in_cp, old_out_cp);
    return rc;

cleanup:
    for (int i = 0; i < argc_w; i++) free(argv_u8[i]);
    free(argv_u8);
    restore_console_cp(old_in_cp, old_out_cp);
    return 0;

cleanup_err:
    for (int i = 0; i < argc_w; i++) free(argv_u8[i]);
    free(argv_u8);
    restore_console_cp(old_in_cp, old_out_cp);
    return 1;
}
