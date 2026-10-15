/* Native -nix regression for statvfs paths and non-mutating locale queries. */
#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

int main(void)
{
    const char *paths[] = {
        "SYS:", ".", "/SYS", "/SYS/S", "/SYS/S/Startup-Sequence"
    };
    struct statvfs fs;
    unsigned int i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
    {
        if (statvfs(paths[i], &fs) != 0 || fs.f_bsize == 0 ||
            fs.f_blocks == 0 || fs.f_bavail > fs.f_blocks)
        {
            fprintf(stderr, "statvfs failed: %s errno=%d\n", paths[i], errno);
            return 1;
        }
    }
    errno = 0;
    if (statvfs("/SYS/__native_statvfs_missing_0127__", &fs) != -1 ||
        errno != ENOENT)
        return 2;
    errno = 0;
    if (statvfs(NULL, &fs) != -1 || errno != EINVAL)
        return 3;
    errno = 0;
    if (statvfs("/SYS", NULL) != -1 || errno != EINVAL)
        return 4;

    if (!setlocale(LC_CTYPE, "C") || strcmp(nl_langinfo(CODESET), "ASCII"))
        return 5;
    if (!setlocale(LC_CTYPE, "C.UTF-8"))
        return 6;
    for (i = 0; i < 10; ++i)
    {
        const char *current = setlocale(LC_CTYPE, NULL);
        if (!current || strcmp(current, "C.UTF-8") ||
            strcmp(nl_langinfo(CODESET), "UTF-8") || MB_CUR_MAX <= 1)
            return 7;
    }
    if (setlocale(LC_CTYPE, "not-a-supported-locale") != NULL ||
        setlocale(-1, NULL) != NULL ||
        strcmp(setlocale(LC_CTYPE, NULL), "C.UTF-8"))
        return 8;
    if (!setlocale(LC_CTYPE, "C") || strcmp(nl_langinfo(CODESET), "ASCII"))
        return 9;
    puts("NATIVE-FILESYSTEM-LOCALE-PASS");
    return 0;
}
