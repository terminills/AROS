#ifndef ASM_TLS_H
#define ASM_TLS_H

#include <aros/config.h>

#if defined(__AROSEXEC_SMP__)
#include <kernel_scheduler.h>
#endif

#include "apic.h"

typedef struct tls
{
    struct ExecBase             *SysBase;
    void                        *KernelBase;    /* Base of kernel.resource              */
#if defined(__AROSEXEC_SMP__)
    struct X86SchedulerPrivate  *ScheduleData;
#endif
    apicid_t                    CPUNumber;
} tls_t;

#define TLSSF_Quantum   (1 << 0)
#define TLSSF_Switch    (1 << 1)
#define TLSSF_Dispatch  (1 << 2)

/* TLS_GET/TLS_SET move a field of 1, 2, 4 or 8 bytes (apicid_t CPUNumber is
   1 byte; without a case for it the set was dropped and the get returned an
   uninitialised register). */
#define TLS_OFFSET(name) ((char *)&(((tls_t *)0)->name)-(char *)0)

#define TLS_GET(name) \
    ({ \
        tls_t *__tls = (tls_t *)0; \
        typeof(__tls -> name) __ret; \
        _Static_assert(sizeof(__tls -> name) == 1 || sizeof(__tls -> name) == 2 || \
                       sizeof(__tls -> name) == 4 || sizeof(__tls -> name) == 8, \
                       "TLS_GET: unsupported field size"); \
        switch (sizeof(__tls -> name)) { \
case 1: asm volatile("movb %%gs:%P1,%0":"=q"(__ret):"n"(TLS_OFFSET(name)):"memory"); break; \
case 2: asm volatile("movw %%gs:%P1,%0":"=r"(__ret):"n"(TLS_OFFSET(name)):"memory"); break; \
case 4: asm volatile("movl %%gs:%P1,%0":"=r"(__ret):"n"(TLS_OFFSET(name)):"memory"); break; \
case 8: asm volatile("movq %%gs:%P1,%0":"=r"(__ret):"n"(TLS_OFFSET(name)):"memory"); break; \
        } \
        __ret;  \
    })

#define TLS_SET(name, val) \
    do { \
        tls_t *__tls = (tls_t *)0; \
        typeof(__tls -> name) __set = val; \
        _Static_assert(sizeof(__tls -> name) == 1 || sizeof(__tls -> name) == 2 || \
                       sizeof(__tls -> name) == 4 || sizeof(__tls -> name) == 8, \
                       "TLS_SET: unsupported field size"); \
        switch (sizeof(__tls -> name)) { \
case 1: asm volatile("movb %0,%%gs:%P1"::"q"(__set),"n"(TLS_OFFSET(name)):"memory"); break; \
case 2: asm volatile("movw %0,%%gs:%P1"::"r"(__set),"n"(TLS_OFFSET(name)):"memory"); break; \
case 4: asm volatile("movl %0,%%gs:%P1"::"r"(__set),"n"(TLS_OFFSET(name)):"memory"); break; \
case 8: asm volatile("movq %0,%%gs:%P1"::"r"(__set),"n"(TLS_OFFSET(name)):"memory"); break; \
        } \
    } while(0);

#endif
