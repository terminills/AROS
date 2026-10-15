/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <aros/asmcall.h>
#include <dos/dos.h>
#include <exec/interrupts.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include <stdio.h>

#define HANDLER_MAGIC ((APTR)(IPTR)0x4D48444CUL)

static struct MemHandlerData *seen_mhd;
static APTR seen_data;
static IPTR seen_size;
static ULONG calls;

AROS_UFH3S(LONG, TestMemHandler,
    AROS_UFHA(struct MemHandlerData *, mhd, A0),
    AROS_UFHA(APTR, data, A1),
    AROS_UFHA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT

    calls++;
    seen_mhd = mhd;
    seen_data = data;
    seen_size = mhd ? mhd->memh_RequestSize : 0;

    return MEM_DID_NOTHING;

    AROS_USERFUNC_EXIT
}

int main(void)
{
    struct Interrupt handler;
    IPTR total = AvailMem(MEMF_TOTAL);
    IPTR request = total + 0x100000;
    APTR mem;
    int failures = 0;

    if (request < total)
        request = ~(IPTR)0 & ~(IPTR)0xFFFF;

    handler.is_Node.ln_Type = NT_INTERRUPT;
    handler.is_Node.ln_Pri = 0;
    handler.is_Node.ln_Name = "memhandler test";
    handler.is_Data = HANDLER_MAGIC;
    handler.is_Code = (VOID_FUNC)TestMemHandler;

    AddMemHandler(&handler);

    /* A request larger than all memory must fail and run the handlers. */
    mem = AllocMem(request, MEMF_ANY);

    RemMemHandler(&handler);

    if (mem)
    {
        FreeMem(mem, request);
        printf("memhandler: FAIL allocation of %lu bytes succeeded\n", (unsigned long)request);
        return RETURN_FAIL;
    }

    if (calls == 0)
    {
        printf("memhandler: FAIL handler was not called\n");
        failures++;
    }
    if (seen_data != HANDLER_MAGIC)
    {
        printf("memhandler: FAIL is_Data 0x%p, expected 0x%p\n", seen_data, HANDLER_MAGIC);
        failures++;
    }
    if (seen_mhd == NULL || seen_mhd == HANDLER_MAGIC)
    {
        printf("memhandler: FAIL MemHandlerData 0x%p\n", seen_mhd);
        failures++;
    }
    else if (seen_size != request)
    {
        printf("memhandler: FAIL memh_RequestSize %lu, expected %lu\n",
               (unsigned long)seen_size, (unsigned long)request);
        failures++;
    }

    if (failures)
        return RETURN_FAIL;

    printf("memhandler: PASS (%lu call(s))\n", (unsigned long)calls);
    return RETURN_OK;
}
