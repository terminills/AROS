/*
    Copyright (C) 2012-2026, The AROS Development Team. All rights reserved.

    Internal initialisation code for posixc.library
*/

#include <aros/debug.h>

#include <proto/exec.h>
#include <aros/symbolsets.h>

#include <libraries/posixc.h>

#include "__posixc_intbase.h"

/* We handle StdCBase */
const ULONG __aros_rellib_base_StdCBase = 0;
SETRELLIBOFFSET(StdCBase, struct PosixCBase, StdCBase)

/* We handle StdCIOBase */
const ULONG __aros_rellib_base_StdCIOBase = 0;
SETRELLIBOFFSET(StdCIOBase, struct PosixCBase, StdCIOBase)

static int __posixc_open(struct PosixCIntBase *PosixCBase)
{
    D(bug("[posixc] %s(0x%p)\n", __func__, PosixCBase));

    /* The pool serves every Task that calls through this base (a Process
       and all of its pthread workers), so it must serialise itself: an
       unprotected pool corrupts its TLSF free lists under concurrent
       AllocPooled()/FreePooled(). */
    PosixCBase->internalpool = CreatePool(MEMF_PUBLIC|MEMF_CLEAR|MEMF_SEM_PROTECTED, 256, 256);

    return PosixCBase->internalpool != NULL;
}

static void __posixc_close(struct PosixCIntBase *PosixCBase)
{
    D(bug("[posixc] %s(0x%p)\n", __func__, PosixCBase));
    DeletePool(PosixCBase->internalpool);
}

ADD2OPENLIB(__posixc_open, -50);
ADD2CLOSELIB(__posixc_close, -50);
