/* Copyright 2026 The AROS Development Team. SPDX-License-Identifier: BSD-2-Clause
 * Read-only RAM deployment gate, not a GPU or filesystem test. Direct DOS
 * reads deliberately avoid FILE/setvbuf while preserving full SHA256 checks.
 * Two independent opens must match the supplied host digest and exact size.
 * Build with build_file_sha256.sh's explicit native SDK/compiler flags.
 */
#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <exec/memory.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "../../../network/WirelessManager/src/crypto/sha256-internal.c"

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int main(int argc, char **argv)
{
    unsigned char expected[32], digest[32];
    unsigned long long expected_size;
    unsigned char *buffer;
    char *end;
    int result = 20;

    if (argc != 4 || strncmp(argv[1], "RAM:", 4) || !argv[1][4] ||
        strlen(argv[2]) != 64 || argv[3][0] < '0' || argv[3][0] > '9') {
        printf("Usage: AmdgpuFileSHA256Dos RAM:file expected-sha256 expected-bytes\n");
        return 20;
    }
    for (unsigned int i = 0; i < sizeof(expected); i++) {
        int high = hex_digit(argv[2][i * 2]);
        int low = hex_digit(argv[2][i * 2 + 1]);
        if (high < 0 || low < 0) return 20;
        expected[i] = (high << 4) | low;
    }
    errno = 0;
    expected_size = strtoull(argv[3], &end, 10);
    if (errno || *end || !expected_size) return 20;
    buffer = AllocVec(65536, MEMF_PUBLIC);
    if (!buffer) return 20;

    for (unsigned int pass = 1; pass <= 2; pass++) {
        struct sha256_state state;
        unsigned long long total = 0;
        BPTR file = Open((CONST_STRPTR)argv[1], MODE_OLDFILE);
        BOOL read_ok = TRUE;
        if (!file) {
            printf("DOS-SHA256 pass%u open failed IoErr=%ld\n", pass, (long)IoErr());
            goto done;
        }
        sha256_init(&state);
        for (;;) {
            LONG count = Read(file, buffer, 65536);
            if (count < 0) {
                printf("DOS-SHA256 pass%u read failed IoErr=%ld\n", pass, (long)IoErr());
                read_ok = FALSE;
                break;
            }
            if (!count) break;
            if (count > 65536 || (unsigned long long)count > expected_size - total ||
                sha256_process(&state, buffer, count)) {
                read_ok = FALSE;
                break;
            }
            total += count;
        }
        if (!Close(file)) read_ok = FALSE;
        if (!read_ok || total != expected_size || sha256_done(&state, digest)) {
            printf("DOS-SHA256 pass%u incomplete/failed bytes=%llu expected=%llu\n",
                   pass, total, expected_size);
            goto done;
        }
        printf("DOS-SHA256 pass%u ", pass);
        for (unsigned int i = 0; i < sizeof(digest); i++) printf("%02x", digest[i]);
        printf(" %llu bytes %s\n", total, argv[1]);
        if (memcmp(digest, expected, sizeof(digest))) {
            printf("DOS-SHA256 FAIL: host digest mismatch\n");
            goto done;
        }
    }
    printf("DOS-SHA256 PASS: both complete reads match host digest and size\n");
    result = 0;
done:
    FreeVec(buffer);
    return result;
}
