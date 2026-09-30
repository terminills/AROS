/* Read-only native deployment verifier, not a driver runtime test.
 * Reuse the existing SHA256 implementation without copying or modifying it.
 * Use build_file_sha256.sh with the target's native Developer SDK. The cross
 * compiler's default SDK may point at a different hosted ABI/library layout.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "../../../network/WirelessManager/src/crypto/sha256-internal.c"

int main(int argc, char **argv)
{
    /* Keep pre-repair reads small while the installed NVMe PRP path is faulty.
     * This bounds our request, not any filesystem-internal read-ahead. */
    static unsigned char buffer[256 * 1024];
    size_t read_size = 4096;
    unsigned char digest[32];
    struct sha256_state state;
    unsigned long long total = 0;
    FILE *input;
    size_t count;
    int result = 20;

    if (argc != 2 && argc != 3) {
        printf("Usage: AmdgpuFileSHA256 file [RAMFAST]\n");
        return 20;
    }
    /* Explicitly opt in only for literal RAM: files staged on the DUT.
     * Do not enlarge disk reads or infer safety through assigns. Larger RAM
     * reads reduce serial-traced DOS calls; this is not a storage repair. */
    if (argc == 3) {
        if (strcmp(argv[2], "RAMFAST") || strncmp(argv[1], "RAM:", 4)) {
            printf("AmdgpuFileSHA256: RAMFAST requires a literal RAM: path\n");
            return 20;
        }
        read_size = sizeof(buffer);
    }
    input = fopen(argv[1], "rb");
    if (!input) {
        printf("AmdgpuFileSHA256: open failed: %s\n", strerror(errno));
        return 20;
    }
    if (setvbuf(input, NULL, _IOFBF, read_size)) {
        printf("AmdgpuFileSHA256: buffer setup failed\n");
        fclose(input);
        return 20;
    }
    sha256_init(&state);
    while ((count = fread(buffer, 1, read_size, input)) != 0) {
        if (sha256_process(&state, buffer, count))
            goto done;
        total += count;
    }
    if (ferror(input) || !feof(input) || sha256_done(&state, digest))
        goto done;
    result = 0;
done:
    if (fclose(input))
        result = 20;
    if (result) {
        printf("AmdgpuFileSHA256: read/hash/close failed after %llu bytes\n", total);
        return result;
    }
    printf("AmdgpuFileSHA256: ");
    for (unsigned int i = 0; i < sizeof(digest); ++i)
        printf("%02x", digest[i]);
    printf(" %llu bytes %s\n", total, argv[1]);
    return 0;
}
