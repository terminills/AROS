/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Standalone diagnostic caller; does not replace transparent DOS dispatch.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/m68kemu.h>
#include <stdio.h>
#include <string.h>

struct Library *M68KEmuBase;

int main(int argc, char **argv)
{
    char args[4096];
    LONG result;
    BPTR seg;
    const char *text = argc == 3 ? argv[2] : "";
    if (argc < 2 || argc > 3 || strlen(text) > sizeof(args) - 2) {
        puts("Usage: native-run program [\"argument string\"]");
        return 20;
    }
    strcpy(args, text);
    strcat(args, "\n");
    M68KEmuBase = OpenLibrary("m68kemu.library", 0);
    if (!M68KEmuBase) {
        puts("NATIVE-RUN FAIL open library");
        return 20;
    }
    seg = LoadSeg(argv[1]);
    if (!seg) {
        puts("NATIVE-RUN FAIL LoadSeg");
        CloseLibrary(M68KEmuBase);
        return 20;
    }
    result = RunHunk(seg, 524288, args, (ULONG)strlen(args));
    UnLoadSeg(seg);
    CloseLibrary(M68KEmuBase);
    printf("NATIVE-RUN returned %ld\n", (long)result);
    return result;
}
