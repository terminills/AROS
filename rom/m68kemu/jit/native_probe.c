/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Standalone x86-64 AROS capability probe; never part of m68kemu.library.
 * A successful call does not prove W^X: KrnSetProtection returns void and
 * native x86-64 may not enforce NX on the initial writable allocation.
 */
#include <exec/memory.h>
#include <aros/kernel.h>
#include <proto/exec.h>
#include <proto/kernel.h>
#include <stdio.h>
#include <string.h>

#if !defined(__AROS__) || !defined(__x86_64__)
#error This probe requires x86-64 AROS
#endif

APTR KernelBase;

int main(void)
{
    /* ENDBR64; mov eax,0x24681357; ret. No embedded guest pointers. */
    static const unsigned char code[] = {
        0xf3, 0x0f, 0x1e, 0xfa, 0xb8, 0x57, 0x13, 0x68, 0x24, 0xc3
    };
    unsigned int iteration;
    KernelBase = OpenResource("kernel.resource");
    if (!KernelBase) {
        puts("NATIVE-PROBE FALLBACK no kernel.resource");
        return 20;
    }
    for (iteration = 0; iteration < 32; ++iteration) {
        void *page = KrnAllocPages(NULL, 4096, MEMF_ANY);
        unsigned int value;
        if (!page) {
            printf("NATIVE-PROBE FALLBACK allocation iteration=%u\n", iteration);
            return 20;
        }
        memcpy(page, code, sizeof(code));
        CacheClearE(page, sizeof(code), CACRF_ClearI | CACRF_ClearD);
        KrnSetProtection(page, 4096, MAP_Readable | MAP_Executable);
        value = ((unsigned int (*)(void))page)();
        KrnFreePages(page, 4096);
        if (value != 0x24681357U) {
            printf("NATIVE-PROBE FAIL return=%08x iteration=%u\n", value, iteration);
            return 20;
        }
    }
    puts("NATIVE-PROBE PASS 32 allocate/emit/protect/call/free cycles");
    puts("NATIVE-PROBE LIMIT no NX or write-protection enforcement proof");
    return 0;
}
