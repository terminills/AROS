/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Keyring layer: derive-don't-store secret storage.
      * master seed  : machine-transparent MVP -- generated once, persisted at
                       ENVARC:Keyring/.seed (as safe as the disk; a later pass
                       seals it under a machine-derived key / adds passphrase mode).
      * per-entry key: HKDF-SHA256(seed, "cred:v1:" service \0 account) -- derived
                       on demand, never stored.
      * sealing      : ChaCha20-Poly1305 (embedded reference crypto via
                       credentials_crypto.c -- no external crypto library).
      * backing store: one ciphertext blob per entry at ENVARC:Keyring/<sha256hex>;
                       only ciphertext + nonce + tag touch the disk.
    This is the AROS LVO + DOS-store half; the crypto lives entirely behind the
    plain-C credentials_crypto.h interface so the type worlds never collide.
*/

#include <aros/symbolsets.h>
#include <aros/libcall.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <string.h>

#include "credentials_private.h"
#include "credentials_crypto.h"
#include LC_LIBDEFS_FILE

extern struct DosLibrary *DOSBase;   /* opened in credentials_init.c */

#define CRED_SEED_FILE     "ENVARC:Keyring/.seed"
#define CRED_DEVICEID_FILE "ENVARC:Keyring/.deviceid"
#define CRED_MAX_SECRET    65536

/* ---- DOS store helpers ---- */

static void cred_ensure_dir(void)
{
    BPTR l = CreateDir((CONST_STRPTR)CRED_KEYRING_DIR);
    if (l)
        UnLock(l);   /* if the dir already exists CreateDir just fails, harmless */
}

static LONG cred_write_file(CONST_STRPTR path, const void *data, LONG len)
{
    BPTR fh = Open(path, MODE_NEWFILE);
    LONG w;
    if (!fh)
        return -1;
    w = Write(fh, (APTR)data, len);
    Close(fh);
    return (w == len) ? 0 : -1;
}

static LONG cred_read_file(CONST_STRPTR path, void *buf, LONG cap)
{
    BPTR fh = Open(path, MODE_OLDFILE);
    LONG r;
    if (!fh)
        return -1;
    r = Read(fh, buf, cap);
    Close(fh);
    return r;   /* bytes read, or -1 */
}

/* SHA-256(service \0 account) -> "ENVARC:Keyring/<64 hex>" into out. */
static void cred_entry_path(CONST_STRPTR service, CONST_STRPTR account,
                            STRPTR out, ULONG outsize)
{
    static const char hexd[] = "0123456789abcdef";
    UBYTE tmp[512], md[32];
    TEXT  hex[65];
    int n = 0, i;

    for (i = 0; service && service[i] && n < (int)sizeof(tmp); i++) tmp[n++] = service[i];
    if (n < (int)sizeof(tmp)) tmp[n++] = 0;
    for (i = 0; account && account[i] && n < (int)sizeof(tmp); i++) tmp[n++] = account[i];

    cred_crypto_sha256(tmp, n, md);
    for (i = 0; i < 32; i++) { hex[i * 2] = hexd[md[i] >> 4]; hex[i * 2 + 1] = hexd[md[i] & 0x0f]; }
    hex[64] = 0;

    out[0] = 0;
    if (outsize > sizeof(CRED_KEYRING_DIR) + 66)
    {
        strcpy(out, CRED_KEYRING_DIR);
        strcat(out, "/");
        strcat(out, hex);
    }
}

/* HKDF info = "cred:v1:" service \0 account. Returns length. */
static int cred_make_info(CONST_STRPTR service, CONST_STRPTR account,
                          UBYTE *info, int cap)
{
    static const char pfx[] = "cred:v1:";
    int n = 0, i;
    for (i = 0; pfx[i] && n < cap; i++) info[n++] = pfx[i];
    for (i = 0; service && service[i] && n < cap; i++) info[n++] = service[i];
    if (n < cap) info[n++] = 0;
    for (i = 0; account && account[i] && n < cap; i++) info[n++] = account[i];
    return n;
}

/* Load or (first use) create the master seed; unlock the handle. */
static LONG cred_ensure_unlocked(struct CredKeyring *k)
{
    UBYTE seed[CRED_SEED_LEN];
    LONG r;

    if (k->ck_Unlocked)
        return CRED_OK;

    cred_ensure_dir();
    r = cred_read_file((CONST_STRPTR)CRED_SEED_FILE, seed, sizeof(seed));
    if (r != CRED_SEED_LEN)
    {
        if (!cred_crypto_random(seed, sizeof(seed)))
            return CRED_ERR_CRYPTO;
        if (cred_write_file((CONST_STRPTR)CRED_SEED_FILE, seed, sizeof(seed)) != 0)
        {
            memset(seed, 0, sizeof(seed));
            return CRED_ERR_IO;
        }
    }
    memcpy(k->ck_Seed, seed, sizeof(seed));
    memset(seed, 0, sizeof(seed));
    k->ck_Unlocked = TRUE;
    return CRED_OK;
}

/* ---- LVOs ---- */

AROS_LH2(APTR, CredOpenKeyring,
    AROS_LHA(CONST_STRPTR, collection, A0),
    AROS_LHA(ULONG, flags, D0),
    struct CredentialsBase *, CredentialsBase, 6, Credentials)
{
    AROS_LIBFUNC_INIT

    struct CredKeyring *k = AllocVec(sizeof(*k), MEMF_PUBLIC | MEMF_CLEAR);
    CONST_STRPTR c = collection ? collection : (CONST_STRPTR)"login";
    int i;

    if (!k)
        return NULL;
    k->ck_Base = CredentialsBase;
    k->ck_Flags = flags;
    for (i = 0; c[i] && i < (int)sizeof(k->ck_Collection) - 1; i++)
        k->ck_Collection[i] = c[i];
    k->ck_Collection[i] = 0;
    k->ck_Unlocked = FALSE;
    return k;

    AROS_LIBFUNC_EXIT
}

AROS_LH6(LONG, CredKeyringStore,
    AROS_LHA(APTR, kr, A0),
    AROS_LHA(CONST_STRPTR, service, A1),
    AROS_LHA(CONST_STRPTR, account, A2),
    AROS_LHA(CONST APTR, secret, A3),
    AROS_LHA(ULONG, len, D0),
    AROS_LHA(CONST_STRPTR, label, D1),
    struct CredentialsBase *, CredentialsBase, 7, Credentials)
{
    AROS_LIBFUNC_INIT

    struct CredKeyring *k = (struct CredKeyring *)kr;
    UBYTE key[CRED_KEY_LEN], info[600];
    UBYTE *blob;
    TEXT  path[128];
    int   ilen, blen, cap;
    LONG  rc;

    (void)CredentialsBase; (void)label;
    if (!k || !service || !account || !secret || len > CRED_MAX_SECRET)
        return CRED_ERR_BADARG;
    if ((rc = cred_ensure_unlocked(k)) != CRED_OK)
        return rc;

    ilen = cred_make_info(service, account, info, sizeof(info));
    if (!cred_crypto_hkdf(k->ck_Seed, CRED_SEED_LEN, info, ilen, key, CRED_KEY_LEN))
        return CRED_ERR_CRYPTO;

    cap = (int)len + CRED_SEAL_OVERHEAD;
    blob = AllocVec(cap, MEMF_PUBLIC);
    if (!blob) { memset(key, 0, sizeof(key)); return CRED_ERR_NOMEM; }

    blen = cred_crypto_seal(key, secret, (int)len, blob, cap);
    memset(key, 0, sizeof(key));
    if (blen < 0) { FreeVec(blob); return CRED_ERR_CRYPTO; }

    cred_entry_path(service, account, path, sizeof(path));
    rc = (cred_write_file(path, blob, blen) == 0) ? CRED_OK : CRED_ERR_IO;
    memset(blob, 0, cap);
    FreeVec(blob);
    return rc;

    AROS_LIBFUNC_EXIT
}

AROS_LH5(LONG, CredKeyringLookup,
    AROS_LHA(APTR, kr, A0),
    AROS_LHA(CONST_STRPTR, service, A1),
    AROS_LHA(CONST_STRPTR, account, A2),
    AROS_LHA(APTR *, secretOut, A3),
    AROS_LHA(ULONG *, lenOut, D0),
    struct CredentialsBase *, CredentialsBase, 8, Credentials)
{
    AROS_LIBFUNC_INIT

    struct CredKeyring *k = (struct CredKeyring *)kr;
    UBYTE key[CRED_KEY_LEN], info[600];
    UBYTE *blob, *pt;
    TEXT  path[128];
    int   ilen, ptlen, cap;
    LONG  blen, rc;

    (void)CredentialsBase;
    if (secretOut) *secretOut = NULL;
    if (lenOut)    *lenOut = 0;
    if (!k || !service || !account || !secretOut || !lenOut)
        return CRED_ERR_BADARG;
    if ((rc = cred_ensure_unlocked(k)) != CRED_OK)
        return rc;

    cap = CRED_MAX_SECRET + CRED_SEAL_OVERHEAD;
    blob = AllocVec(cap, MEMF_PUBLIC);
    if (!blob)
        return CRED_ERR_NOMEM;

    cred_entry_path(service, account, path, sizeof(path));
    blen = cred_read_file(path, blob, cap);
    if (blen < CRED_SEAL_OVERHEAD) { FreeVec(blob); return CRED_ERR_NOTFOUND; }

    ilen = cred_make_info(service, account, info, sizeof(info));
    if (!cred_crypto_hkdf(k->ck_Seed, CRED_SEED_LEN, info, ilen, key, CRED_KEY_LEN))
    { FreeVec(blob); return CRED_ERR_CRYPTO; }

    pt = AllocVec(blen, MEMF_PUBLIC | MEMF_CLEAR);   /* >= plaintext length */
    if (!pt) { memset(key, 0, sizeof(key)); FreeVec(blob); return CRED_ERR_NOMEM; }

    ptlen = cred_crypto_unseal(key, blob, (int)blen, pt, (int)blen);
    memset(key, 0, sizeof(key));
    memset(blob, 0, cap);
    FreeVec(blob);
    if (ptlen < 0) { memset(pt, 0, blen); FreeVec(pt); return CRED_ERR_CRYPTO; }

    *secretOut = pt;
    *lenOut = (ULONG)ptlen;
    return CRED_OK;

    AROS_LIBFUNC_EXIT
}

AROS_LH3(LONG, CredKeyringDelete,
    AROS_LHA(APTR, kr, A0),
    AROS_LHA(CONST_STRPTR, service, A1),
    AROS_LHA(CONST_STRPTR, account, A2),
    struct CredentialsBase *, CredentialsBase, 9, Credentials)
{
    AROS_LIBFUNC_INIT

    TEXT path[128];
    (void)CredentialsBase; (void)kr;
    if (!service || !account)
        return CRED_ERR_BADARG;
    cred_entry_path(service, account, path, sizeof(path));
    return DeleteFile((CONST_STRPTR)path) ? CRED_OK : CRED_ERR_NOTFOUND;

    AROS_LIBFUNC_EXIT
}

AROS_LH2(VOID, CredKeyringFree,
    AROS_LHA(APTR, secret, A0),
    AROS_LHA(ULONG, len, D0),
    struct CredentialsBase *, CredentialsBase, 10, Credentials)
{
    AROS_LIBFUNC_INIT
    (void)CredentialsBase;
    if (secret)
    {
        if (len)
            memset(secret, 0, len);   /* zeroize before release */
        FreeVec(secret);
    }
    AROS_LIBFUNC_EXIT
}

AROS_LH2(LONG, CredKeyringUnlock,
    AROS_LHA(APTR, kr, A0),
    AROS_LHA(CONST_STRPTR, passphrase, A1),
    struct CredentialsBase *, CredentialsBase, 11, Credentials)
{
    AROS_LIBFUNC_INIT
    struct CredKeyring *k = (struct CredKeyring *)kr;
    (void)CredentialsBase;
    if (!k)
        return CRED_ERR_BADARG;
    if (passphrase)
        return CRED_ERR_NOTIMPL;      /* passphrase mode: follow-up */
    return cred_ensure_unlocked(k);   /* machine-transparent MVP */
    AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, CredKeyringLock,
    AROS_LHA(APTR, kr, A0),
    struct CredentialsBase *, CredentialsBase, 12, Credentials)
{
    AROS_LIBFUNC_INIT
    struct CredKeyring *k = (struct CredKeyring *)kr;
    (void)CredentialsBase;
    if (!k)
        return CRED_ERR_BADARG;
    memset(k->ck_Seed, 0, sizeof(k->ck_Seed));
    k->ck_Unlocked = FALSE;
    return CRED_OK;
    AROS_LIBFUNC_EXIT
}

AROS_LH1(VOID, CredCloseKeyring,
    AROS_LHA(APTR, kr, A0),
    struct CredentialsBase *, CredentialsBase, 13, Credentials)
{
    AROS_LIBFUNC_INIT
    struct CredKeyring *k = (struct CredKeyring *)kr;
    (void)CredentialsBase;
    if (k)
    {
        memset(k->ck_Seed, 0, sizeof(k->ck_Seed));
        FreeVec(k);
    }
    AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, CredDeviceID,
    AROS_LHA(UBYTE *, uuidOut, A0),
    struct CredentialsBase *, CredentialsBase, 14, Credentials)
{
    AROS_LIBFUNC_INIT

    UBYTE id[16];
    LONG  r;
    (void)CredentialsBase;
    if (!uuidOut)
        return CRED_ERR_BADARG;

    cred_ensure_dir();
    r = cred_read_file((CONST_STRPTR)CRED_DEVICEID_FILE, id, sizeof(id));
    if (r != 16)
    {
        if (!cred_crypto_random(id, sizeof(id)))
            return CRED_ERR_CRYPTO;
        id[6] = (id[6] & 0x0f) | 0x40;   /* RFC4122 version 4 */
        id[8] = (id[8] & 0x3f) | 0x80;   /* variant           */
        if (cred_write_file((CONST_STRPTR)CRED_DEVICEID_FILE, id, sizeof(id)) != 0)
            return CRED_ERR_IO;
    }
    memcpy(uuidOut, id, 16);
    return CRED_OK;

    AROS_LIBFUNC_EXIT
}
