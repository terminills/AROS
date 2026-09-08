/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    LinuxSO -- load a Linux x86-64 shared object and call an exported symbol.

    Usage: LinuxSO <path> [symbol ...]

    Every named symbol is looked up and, if found, called as
    `const char *fn(void)` with the result printed as a string.  The
    default symbol list is the Widevine CDM's GetCdmVersion, which is the
    smallest end-to-end proof: the object mapped, relocated, ran its
    constructors and returned through the shim.
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <linuxso.h>

int main(int argc, char **argv)
{
    static const char *defsyms[] = { "GetCdmVersion", NULL };
    const char **syms = defsyms;
    char err[512];
    struct LinuxSO *so;
    int i, rc = 0;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <path> [symbol ...]\n", argv[0]);
        return 20;
    }
    if (argc > 2)
        syms = (const char **)&argv[2];

    printf("LinuxSO: loading %s%s\n", argv[1], linuxso_debug_enabled() ? " (debug on)" : "");
    fflush(stdout);
    so = linuxso_open(argv[1], err, sizeof(err));
    if (!so)
    {
        fprintf(stderr, "LinuxSO: %s: %s\n", argv[1], err);
        return 10;
    }
    printf("LinuxSO: loaded\n");
    fflush(stdout);

    for (i = 0; syms[i]; i++)
    {
        const char *(*fn)(void) = linuxso_sym(so, syms[i]);

        if (!fn)
        {
            printf("LinuxSO: %s: not exported\n", syms[i]);
            rc = 5;
            continue;
        }
        printf("LinuxSO: calling %s at %p\n", syms[i], (void *)fn);
        fflush(stdout);
        {
            const char *r = fn();
            printf("LinuxSO: %s() = %p", syms[i], (const void *)r);
            if (r && (size_t)(uintptr_t)r > 0x10000)
                printf(" \"%.80s\"", r);
            printf("\n");
        }
        fflush(stdout);
    }

    linuxso_close(so);
    printf("LinuxSO: closed\n");
    return rc;
}
