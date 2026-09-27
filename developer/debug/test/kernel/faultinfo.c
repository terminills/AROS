/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Check that a page fault handler can read the fault's address and
          error code through KrnGetSystemAttr(KATTR_X86FaultAddress/Error).
*/

#include <aros/kernel.h>
#if defined(__x86_64__)
#include <aros/x86_64/cpucontext.h>
#else
#include <aros/i386/cpucontext.h>
#endif
#include <proto/exec.h>
#include <proto/kernel.h>

#include <stdio.h>

int __nocommandline = 1;

#define PROBE_ADDRESS 0x10     /* in the unmapped page at 0 */
#define PF_WRITE      (1 << 1)

APTR KernelBase;
static volatile IPTR seen_address, seen_error;
static volatile ULONG faults;

/* Runs in the fault: record what the kernel reports, then step over the
 * two-byte probe instruction so the task continues. */
static int fault_handler(struct ExceptionContext *ctx, void *data, void *data2)
{
    seen_address = KrnGetSystemAttr(KATTR_X86FaultAddress);
    seen_error = KrnGetSystemAttr(KATTR_X86FaultError);
    faults++;
#if defined(__x86_64__)
    ctx->rip += 2;
#else
    ctx->eip += 2;
#endif
    return 1;
}

static void probe(IPTR address, int write)
{
    ULONG value = 0x5a5a5a5a;

    if (write)
        asm volatile(".byte 0x89, 0x02"     /* mov %eax, (%rdx) */
                     : : "a" (value), "d" (address) : "memory");
    else
        asm volatile(".byte 0x8b, 0x02"     /* mov (%rdx), %eax */
                     : "+a" (value) : "d" (address) : "memory");
}

int main(void)
{
    APTR handle;
    int failed = 0, write;

    KernelBase = OpenResource("kernel.resource");
    if (!KernelBase)
    {
        printf("Failed to open kernel.resource\n");
        return 20;
    }

    handle = KrnAddExceptionHandler(14, fault_handler, NULL, NULL);
    if (!handle)
    {
        printf("Failed to add a page fault handler\n");
        return 20;
    }

    for (write = 0; write < 2; write++)
    {
        ULONG before = faults;

        seen_address = seen_error = ~0;
        probe(PROBE_ADDRESS, write);

        if (faults != before + 1)
        {
            printf("FAIL: %s of 0x%x: handler ran %u times\n",
                   write ? "write" : "read", PROBE_ADDRESS,
                   (unsigned)(faults - before));
            failed = 1;
        }
        else if (seen_address != PROBE_ADDRESS ||
                 ((seen_error & PF_WRITE) != 0) != write)
        {
            printf("FAIL: %s of 0x%x: address 0x%lx, error 0x%lx\n",
                   write ? "write" : "read", PROBE_ADDRESS,
                   (unsigned long)seen_address, (unsigned long)seen_error);
            failed = 1;
        }
        else
            printf("PASS: %s of 0x%x: address 0x%lx, error 0x%lx\n",
                   write ? "write" : "read", PROBE_ADDRESS,
                   (unsigned long)seen_address, (unsigned long)seen_error);
    }

    KrnRemExceptionHandler(handle);
    return failed ? 10 : 0;
}
