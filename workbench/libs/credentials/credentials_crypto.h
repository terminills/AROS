/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Pure-C crypto interface for credentials.library. The default backend
    (credentials_crypto.c) is self-contained embedded reference crypto --
    ChaCha20-Poly1305 (RFC 8439) + SHA-256/HMAC/HKDF + RDRAND -- with NO
    external crypto library, so it links clean into a bare AROS library module.
    Kept free of AROS headers so the crypto and AROS type worlds never meet in
    one translation unit; the AROS LVO layer (credentials_keyring.c) calls
    these plain-C primitives.

    Swap target: an AmiSSL / libcrypto backend can replace credentials_crypto.c
    behind this same interface with zero change to the keyring layer.
*/
#ifndef CREDENTIALS_CRYPTO_H
#define CREDENTIALS_CRYPTO_H

#include <stddef.h>

#define CRED_SEED_LEN   32
#define CRED_KEY_LEN    32
#define CRED_NONCE_LEN  12
#define CRED_TAG_LEN    16
/* sealed blob overhead added around the plaintext */
#define CRED_SEAL_OVERHEAD (CRED_NONCE_LEN + CRED_TAG_LEN)

/* Fill out[0..len) with CSPRNG bytes. Returns 1 on success, 0 on failure. */
int cred_crypto_random(unsigned char *out, int len);

/* SHA-256 one-shot: md must hold 32 bytes. Returns 1/0. */
int cred_crypto_sha256(const unsigned char *data, int len, unsigned char *md32);

/* HKDF-SHA256: derive outlen bytes from (seed, info) into out. Returns 1/0. */
int cred_crypto_hkdf(const unsigned char *seed, int seedlen,
                     const unsigned char *info, int infolen,
                     unsigned char *out, int outlen);

/* ChaCha20-Poly1305 seal. blob layout: [nonce(12)][ciphertext(ptlen)][tag(16)].
   blob must have room for ptlen + CRED_SEAL_OVERHEAD. Returns total blob
   length, or -1 on failure. */
int cred_crypto_seal(const unsigned char key[CRED_KEY_LEN],
                     const unsigned char *pt, int ptlen,
                     unsigned char *blob, int blobcap);

/* ChaCha20-Poly1305 unseal (verifies the tag). Writes plaintext into pt; ptcap
   must be >= bloblen - CRED_SEAL_OVERHEAD. Returns plaintext length, or -1
   on failure / tag mismatch. */
int cred_crypto_unseal(const unsigned char key[CRED_KEY_LEN],
                       const unsigned char *blob, int bloblen,
                       unsigned char *pt, int ptcap);

#endif /* CREDENTIALS_CRYPTO_H */
