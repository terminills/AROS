/*
 * Read-only DUT deployment check. Compare file contents, never OpenLibrary().
 * This establishes copy identity, not a driver load or a display test.
 * A cold reboot is still required after replacing a resident driver.
 * Native and host builds use the same source for success/failure checks.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>

int main(int argc, char **argv)
{
    FILE *first, *second;
    /* Large Mesa images otherwise cause thousands of tiny DOS packets and
     * serial diagnostics. Keep buffers out of the native Shell's small stack;
     * the byte, EOF, read-error and close-error checks below stay unchanged. */
    static unsigned char left[256 * 1024], right[256 * 1024];
    unsigned long long total = 0;
    int result = 20;

    if (argc != 3) {
        printf("Usage: AmdgpuFileCompare first-file second-file\n");
        return 20;
    }
    first = fopen(argv[1], "rb");
    if (!first) {
        printf("AmdgpuFileCompare: %s: %s\n", argv[1], strerror(errno));
        return 20;
    }
    second = fopen(argv[2], "rb");
    if (!second) {
        printf("AmdgpuFileCompare: %s: %s\n", argv[2], strerror(errno));
        fclose(first);
        return 20;
    }
    /* AROS FRead fills its DOS FileHandle buffer, not the caller's array.
     * Set that buffer too; changing only fread's request size leaves 4KiB
     * device packets underneath. Do this before either stream is read. */
    if (setvbuf(first, NULL, _IOFBF, sizeof(left)) ||
        setvbuf(second, NULL, _IOFBF, sizeof(right))) {
        printf("AmdgpuFileCompare: buffer setup failed\n");
        fclose(first);
        fclose(second);
        return 20;
    }
    for (;;) {
        size_t a = fread(left, 1, sizeof(left), first);
        size_t b = fread(right, 1, sizeof(right), second);
        if (ferror(first) || ferror(second)) {
            printf("AmdgpuFileCompare: READ ERROR after %llu bytes\n", total);
            break;
        }
        /* NetShell captures stdout but may inherit NIL: as stderr. Failures
         * must be just as observable as success, without changing acceptance. */
        if (a != b) {
            printf("AmdgpuFileCompare: LENGTH DIFFERENCE after %llu bytes: %zu != %zu\n",
                   total, a, b);
            break;
        }
        if (memcmp(left, right, a)) {
            size_t offset = 0;
            while (offset < a && left[offset] == right[offset])
                ++offset;
            printf("AmdgpuFileCompare: DIFFERENT at byte %llu: %02x != %02x\n",
                   total + offset, left[offset], right[offset]);
            break;
        }
        total += a;
        if (!a) {
            if (!feof(first) || !feof(second) || !total) {
                printf("AmdgpuFileCompare: incomplete or empty input\n");
                break;
            }
            printf("AmdgpuFileCompare: IDENTICAL %llu bytes: %s == %s\n",
                   total, argv[1], argv[2]);
            result = 0;
            break;
        }
    }
    int first_close = fclose(first);
    int second_close = fclose(second);
    if (first_close || second_close) {
        printf("AmdgpuFileCompare: close error\n");
        result = 20;
    }
    return result;
}
