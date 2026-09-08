/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Identity layer: the single non-NULL source of truth for uv_os_get_passwd,
    Node os.userInfo and posixc getpwuid.
*/

#include <aros/symbolsets.h>
#include <aros/libcall.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/var.h>

#include "credentials_private.h"
#include LC_LIBDEFS_FILE

extern struct DosLibrary *DOSBase;   /* opened in credentials_init.c */

/* Bounded copy, always NUL-terminated. */
static void cred_copy(STRPTR buf, ULONG size, CONST_STRPTR src)
{
    ULONG i = 0;
    if (!src) src = "";
    if (size == 0) return;
    for (; i + 1 < size && src[i]; i++)
        buf[i] = src[i];
    buf[i] = '\0';
}

/* Fill buf from ENV:<name> (global tier) if set non-empty, else from def. */
static void cred_from_env(STRPTR buf, ULONG size, CONST_STRPTR name, CONST_STRPTR def)
{
    LONG n = -1;

    buf[0] = '\0';
    if (DOSBase)
        n = GetVar((STRPTR)name, buf, size, GVF_GLOBAL_ONLY);
    if (n <= 0 || buf[0] == '\0')
        cred_copy(buf, size, def);
    else if (n < (LONG)size)
        buf[n] = '\0';
    else
        buf[size - 1] = '\0';
}

/* Multi-user path: when MuFS/usergroup is configured the current user's real
   record is read here. Not yet wired -- returns FALSE so we fall through to
   the single-user resolution. (JOB-0081 follow-up: usergroup.library.) */
static BOOL cred_try_usergroup(struct CredentialsBase *CredentialsBase)
{
    (void)CredentialsBase;
    return FALSE;
}

/* Resolve the identity into the cache once. Precedence:
   usergroup/MuFS (when configured) -> ENV: overrides -> setup profile
   (ENVARC: tier of the same GetVar namespace) -> non-synthetic defaults.
   INVARIANT: on return every ai_* STRPTR is non-NULL. */
VOID cred_ResolveIdentity(struct CredentialsBase *CredentialsBase)
{
    struct ArosIdentity *id = &CredentialsBase->cb_Identity;

    ObtainSemaphore(&CredentialsBase->cb_Lock);

    if (!CredentialsBase->cb_IdentityValid)
    {
        if (!cred_try_usergroup(CredentialsBase))
        {
            cred_from_env(CredentialsBase->cb_UserBuf,  sizeof(CredentialsBase->cb_UserBuf),  "USER",     CRED_DEFAULT_USER);
            cred_from_env(CredentialsBase->cb_FullBuf,  sizeof(CredentialsBase->cb_FullBuf),  "REALNAME", "");
            cred_from_env(CredentialsBase->cb_HomeBuf,  sizeof(CredentialsBase->cb_HomeBuf),  "HOME",     CRED_DEFAULT_HOME);
            cred_from_env(CredentialsBase->cb_ShellBuf, sizeof(CredentialsBase->cb_ShellBuf), "SHELL",    CRED_DEFAULT_SHELL);

            id->ai_UID = CRED_DEFAULT_UID;
            id->ai_GID = CRED_DEFAULT_GID;
        }

        /* Point the record at the backing buffers and enforce non-NULL. */
        id->ai_UserName = CredentialsBase->cb_UserBuf;
        id->ai_FullName = CredentialsBase->cb_FullBuf;
        id->ai_HomeDir  = CredentialsBase->cb_HomeBuf;
        id->ai_Shell    = CredentialsBase->cb_ShellBuf;

        CredentialsBase->cb_IdentityValid = TRUE;
    }

    ReleaseSemaphore(&CredentialsBase->cb_Lock);
}

/*****************************************************************************

    NAME */
        AROS_LH1(LONG, CredGetIdentity,

/*  SYNOPSIS */
        AROS_LHA(struct ArosIdentity *, id, A0),

/*  LOCATION */
        struct CredentialsBase *, CredentialsBase, 5, Credentials)

/*  FUNCTION
        Fill *id with the current AROS identity. Every string field is
        guaranteed non-NULL (empty "" for absent optionals). Backs
        uv_os_get_passwd, Node os.userInfo and posixc getpwuid.

    INPUTS
        id - caller-supplied struct ArosIdentity to populate.

    RESULT
        CRED_OK, or CRED_ERR_BADARG if id is NULL.

    NOTES
        The returned string pointers reference library-owned storage that
        stays valid for the life of the library; callers must copy strings
        they intend to keep across an Expunge.

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    if (!id)
        return CRED_ERR_BADARG;

    cred_ResolveIdentity(CredentialsBase);

    ObtainSemaphoreShared(&CredentialsBase->cb_Lock);
    *id = CredentialsBase->cb_Identity;
    ReleaseSemaphore(&CredentialsBase->cb_Lock);

    return CRED_OK;

    AROS_LIBFUNC_EXIT
}
