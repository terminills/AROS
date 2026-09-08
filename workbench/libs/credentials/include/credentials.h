/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    credentials.library -- AROS identity + keyring substrate.

    ONE library, TWO layers:
      * IDENTITY  -- the passwd-equivalent every port needs (uv_os_get_passwd,
                     Node os.userInfo, posixc getpwuid). CredGetIdentity() is the
                     single source of truth; every string field is GUARANTEED
                     non-NULL (empty "" for absent optionals, NEVER NULL).
      * KEYRING   -- libsecret-API-compatible secret store keyed by
                     (service, account). Derive-don't-store: BIP39 seed -> HKDF
                     per-entry key -> AmiSSL AES-256-GCM seal; only ciphertext is
                     persisted (ENVARC:Keyring/). uuid.library for handles/device-id.
*/
#ifndef LIBRARIES_CREDENTIALS_H
#define LIBRARIES_CREDENTIALS_H

#include <exec/types.h>

/* -------- Identity layer -------- */

/* The native AROS identity record. Resolution precedence (see
   CredGetIdentity): (1) usergroup/MuFS current user when configured;
   (2) ENV:USER / ENV:HOME / ENV:SHELL overrides; (3) the setup-created
   native profile at ENVARC:Sys/identity; (4) non-synthetic defaults
   (username "AROS", home "SYS:Home", shell "", uid/gid 1000).
   INVARIANT: no ai_* STRPTR is ever NULL. */
struct ArosIdentity
{
    STRPTR ai_UserName;     /* login name; never NULL                    */
    STRPTR ai_FullName;     /* real name / gecos; never NULL ("" ok)     */
    STRPTR ai_HomeDir;      /* real AROS path (e.g. SYS:Home); never NULL */
    STRPTR ai_Shell;        /* default shell; never NULL ("" ok)         */
    ULONG  ai_UID;          /* single-user default 1000 (NOT 0)          */
    ULONG  ai_GID;          /* single-user default 1000                  */
};

/* -------- Keyring layer -------- */

/* CredOpenKeyring() flags. */
#define CRKF_CREATE     (1UL << 0)  /* create the collection if absent   */
#define CRKF_READONLY   (1UL << 1)  /* open without unlock-for-write     */

/* -------- Result codes (all functions returning LONG) -------- */
#define CRED_OK          0
#define CRED_ERR_NOTFOUND 1
#define CRED_ERR_LOCKED   2
#define CRED_ERR_IO       3
#define CRED_ERR_CRYPTO   4
#define CRED_ERR_NOMEM    5
#define CRED_ERR_BADARG   6
#define CRED_ERR_NOTIMPL  7   /* keyring layer stub, pre-AmiSSL wiring   */

#endif /* LIBRARIES_CREDENTIALS_H */
