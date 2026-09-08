/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    credentials.library init/expunge.
*/

#include <aros/symbolsets.h>
#include <aros/debug.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "credentials_private.h"
#include LC_LIBDEFS_FILE

/* Global dos base used by the identity resolver (GetVar). dos.library may be
   absent very early; the resolver tolerates a NULL DOSBase and falls back to
   the non-synthetic defaults, so library init still succeeds. */
struct DosLibrary *DOSBase = NULL;

static int Credentials_Init(LIBBASETYPEPTR LIBBASE)
{
    D(bug("[credentials] Init\n"));

    InitSemaphore(&LIBBASE->cb_Lock);
    LIBBASE->cb_IdentityValid = FALSE;

    if (!DOSBase)
        DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 0);

    return TRUE;
}

static int Credentials_Expunge(LIBBASETYPEPTR LIBBASE)
{
    D(bug("[credentials] Expunge\n"));

    (void)LIBBASE;
    if (DOSBase)
    {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }

    return TRUE;
}

ADD2INITLIB(Credentials_Init, 0)
ADD2EXPUNGELIB(Credentials_Expunge, 0)
