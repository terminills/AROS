/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Private state for credentials.library.
*/
#ifndef CREDENTIALS_PRIVATE_H
#define CREDENTIALS_PRIVATE_H

#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <libraries/credentials.h>

/* AROS-native locations (no Unix dot-directory hiding dependency). */
#define CRED_IDENTITY_VAR   "Sys/identity"   /* under ENVARC: via GetVar   */
#define CRED_KEYRING_DIR     "ENVARC:Keyring" /* ciphertext-only blobs      */

/* Non-synthetic single-user defaults (used only as the final floor). */
#define CRED_DEFAULT_USER    "AROS"
#define CRED_DEFAULT_HOME    "SYS:Home"
#define CRED_DEFAULT_SHELL   ""
#define CRED_DEFAULT_UID     1000            /* NOT 0: ports gate on uid!=0 */
#define CRED_DEFAULT_GID     1000

struct CredentialsBase
{
    struct Library         cb_Lib;
    struct SignalSemaphore cb_Lock;          /* guards the cached identity  */

    /* Cached identity (resolved once, refreshed on env/profile change).
       The strings point into cb_IdBuf so the record survives past the
       resolver call and satisfies the non-NULL invariant. */
    struct ArosIdentity    cb_Identity;
    BOOL                   cb_IdentityValid;
    /* Backing storage for the identity strings (single-user is tiny). */
    TEXT                   cb_UserBuf[64];
    TEXT                   cb_FullBuf[128];
    TEXT                   cb_HomeBuf[256];
    TEXT                   cb_ShellBuf[256];
};

/* internal identity resolver (credentials_identity.c) */
VOID cred_ResolveIdentity(struct CredentialsBase *CredentialsBase);

/* Keyring handle returned by CredOpenKeyring(). Holds the (machine-transparent
   MVP) master seed after unlock; per-entry keys are HKDF-derived on demand and
   never stored -- only ciphertext blobs live under ENVARC:Keyring/. */
struct CredKeyring
{
    struct CredentialsBase *ck_Base;
    UBYTE                   ck_Seed[32];    /* master seed; valid iff ck_Unlocked */
    BOOL                    ck_Unlocked;
    ULONG                   ck_Flags;       /* CRKF_*                             */
    TEXT                    ck_Collection[64];
};

#endif /* CREDENTIALS_PRIVATE_H */
