/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.
*/

#include <proto/exec.h>

#include <aros/config.h>
#include <aros/symbolsets.h>
#include <exec/types.h>
#include <asm/cpu.h>

#include "kernel_base.h"
#include <kernel_debug.h>

#define D(x)

static int cpu_Init(struct KernelBase *KernelBase)
{
#if (AROS_FLAVOUR == AROS_FLAVOUR_STANDALONE) || defined(SIGCORE_FPU_XSAVE)
    ULONG AVXOffs = 0;
#endif

    D(bug("[Kernel] %s: KernelBase @ 0x%p\n", __func__, KernelBase);)

    KernelBase->kb_ContextSize = 0;
#if (AROS_FLAVOUR == AROS_FLAVOUR_STANDALONE)
    D(bug("[Kernel] %s: Checking for XSAVE/AVX availability...\n", __func__, KernelBase);)

    asm volatile (
        "    mov $0x01, %%eax\n\t"
        "    cpuid\n\t"
        "    mov        $0x04000000, %%eax\n\t"                                         /* First check for XSAVE (bit 26)               */
        "    and        %%ecx, %%eax\n\t"
        "    jz .xscheck_done\n\t"
        "    mov        $0x10000000, %%eax\n\t"                                         /* and then check for AVX (bit 28)              */
        "    and        %%ecx, %%eax\n\t"
        "    jz .no_avx\n\t"
        "    mov        $0x0d, %%eax\n\t"                                               /* ok get the AVX offset                        */
        "    mov        $0x02, %%ecx\n\t"
        "    cpuid\n\t"
        "    mov        %%ebx, %0\n\t\n"
        ".no_avx:\n\t"
        ".xscheck_done:\n\t"
        : "+m"(AVXOffs)
        :
        : "%eax", "%ebx", "%ecx", "%edx"
    );

    if (AVXOffs)
    {
        ULONG featMask;
        ULONG XContextSize = 0;

        D(bug("[Kernel] %s: Adjusting CR0 flags ...\n", __func__);)
        /* Enable monitoring media instruction to generate #NM when CR0.TS is set.
         * Disable coprocessor emulation.
         */
        wrcr(cr0, (rdcr(cr0) & ~_CR0_EM) | _CR0_MP);
        
        D(bug("[Kernel] %s: Enabling #XF & XGETVB ...\n", __func__);)
        /* Enable #XF instead of #UD when a SIMD exception occurs
         * Enable xgetvb/xsetvb
         */
        wrcr(cr4, rdcr(cr4) | _CR4_OSXMMEXCPT | _CR4_OSXSBV);

        D(bug("[Kernel] %s: Enabling AVX context Load/Save...\n", __func__);)
        asm volatile (
            "    xor        %%rcx, %%rcx\n\t"
            "    xgetbv\n\t"                                                            /* Load XCR0 register                           */
            "    or         $0b111, %%eax\n\t"                                          /* Set FPU, XMM, YMM bits                       */
            "    mov        %%eax, %0\n\t\n"
            "    xsetbv\n\t"                                                            /* Save back to XCR0                            */
            : "=m"(featMask) : : "%rax",  "%rcx",  "%rdx"
            );

        asm volatile (
            "    mov        $0x0d, %%eax\n\t"                                           /* Get the size of the XSAVE area               */
            "    xor        %%ecx, %%ecx\n\t"                                           /* for Extended Control Register 0              */
            "    cpuid\n\t"
            "    mov        %%ebx, %0\n\t\n"
            : "+m"(XContextSize)
            :
            : "%eax", "%ebx", "%ecx", "%edx"
        );

        D(
            bug("[Kernel] %s: AVX feature mask %08x\n", __func__, featMask);
            bug("[Kernel] %s: AVX required size = %u\n", __func__, XContextSize);
        )
        KernelBase->kb_ContextSize = sizeof(struct AROSCPUContext) + XContextSize +  63;
    }
#elif defined(SIGCORE_FPU_XSAVE)
    /*
     * Hosted: the host kernel owns CR0/CR4/XCR0 and hands us its XSAVE area
     * in every signal frame. Only ask whether it enabled XSAVE (OSXSAVE,
     * bit 27) and AVX (bit 28), then size the context from what XCR0 covers.
     */
    D(bug("[Kernel] %s: Checking for OSXSAVE/AVX availability...\n", __func__);)

    asm volatile (
        "    mov $0x01, %%eax\n\t"
        "    cpuid\n\t"
        "    mov        $0x18000000, %%eax\n\t"
        "    and        %%ecx, %%eax\n\t"
        "    cmp        $0x18000000, %%eax\n\t"
        "    jne .xscheck_done\n\t"
        "    mov        $0x0d, %%eax\n\t"                                               /* Size of the XSAVE area for XCR0              */
        "    xor        %%ecx, %%ecx\n\t"
        "    cpuid\n\t"
        "    mov        %%ebx, %0\n\t\n"
        ".xscheck_done:\n\t"
        : "+m"(AVXOffs)
        :
        : "%eax", "%ebx", "%ecx", "%edx"
    );

    if (AVXOffs)
    {
        D(bug("[Kernel] %s: XSAVE area size = %u\n", __func__, AVXOffs);)
        KernelBase->kb_ContextSize = sizeof(struct AROSCPUContext) + AVXOffs +  63;
    }
#endif
    if (KernelBase->kb_ContextSize == 0)
    {
        /* All x86-64 processors have SSE/FXSAVE */
        KernelBase->kb_ContextSize = sizeof(struct AROSCPUContext) + sizeof(struct FPFXSContext) + 15;
    }

    D(bug("[Kernel] %s: CPU Context size = %u bytes\n", __func__, KernelBase->kb_ContextSize);)

    return TRUE;
}

ADD2INITLIB(cpu_Init, 10);
