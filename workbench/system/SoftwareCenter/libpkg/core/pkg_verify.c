/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Hashing, hex and Ed25519 verification (PROTOCOL.md §5).
          SHA-256 is the credentials.library reference implementation
          (credentials_crypto.c, vendored under libpkg/vendor and compiled
          into libpkg); Ed25519 is vendored TweetNaCl.

    AI_PHASE: SOFTWARE_CENTER_LIBPKG
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: LOW
    AI_DEPENDENCIES: credentials_crypto.c, vendor/tweetnacl.c
*/

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "pkg_core.h"
#include "../vendor/credentials_crypto.h"
#include "../vendor/tweetnacl.h"

/* TweetNaCl wants this symbol for keypair/sign; we only ever verify.  Make
   any accidental use loud rather than silently unrandom. */
void randombytes(unsigned char *x, unsigned long long n)
{
    memset(x, 0, (size_t)n);
    abort();
}

void pkg_sha256(const void *data, size_t len, uint8_t md[32])
{
    cred_sha256_ctx c;
    cred_crypto_sha256_init(&c);
    cred_crypto_sha256_update(&c, data, len);
    cred_crypto_sha256_final(&c, md);
}

void pkg_sha256_init(pkg_sha256_ctx *c)
{
    cred_crypto_sha256_init((cred_sha256_ctx *)c->opaque);
}

void pkg_sha256_update(pkg_sha256_ctx *c, const void *p, size_t n)
{
    cred_crypto_sha256_update((cred_sha256_ctx *)c->opaque, p, n);
}

void pkg_sha256_final(pkg_sha256_ctx *c, uint8_t md[32])
{
    cred_crypto_sha256_final((cred_sha256_ctx *)c->opaque, md);
}

void pkg_hex(const uint8_t *in, size_t len, char *out)
{
    static const char hx[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; i++)
    {
        out[i * 2]     = hx[in[i] >> 4];
        out[i * 2 + 1] = hx[in[i] & 15];
    }
    out[len * 2] = 0;
}

static int nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int pkg_unhex(const char *in, uint8_t *out, size_t outlen)
{
    size_t i;
    for (i = 0; i < outlen; i++)
    {
        int h = nib(in[i * 2]), l;
        if (h < 0) return -1;
        l = nib(in[i * 2 + 1]);
        if (l < 0) return -1;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return in[outlen * 2] == 0 ? 0 : -1;
}

int pkg_hex_equal(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b)
    {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

int pkg_ed25519_verify(const uint8_t *msg, size_t len, const uint8_t sig[64],
                       const uint8_t pub[32])
{
    unsigned char *sm, *m;
    unsigned long long mlen = 0;
    int ok;

    sm = malloc(len + 64);
    m  = malloc(len + 64);
    if (!sm || !m)
    {
        free(sm); free(m);
        return 0;
    }
    memcpy(sm, sig, 64);
    memcpy(sm + 64, msg, len);
    ok = crypto_sign_open(m, &mlen, sm, len + 64, pub) == 0 && mlen == len;
    free(sm);
    free(m);
    return ok;
}

void pkg_keyid(const uint8_t pub[32], char out[33])
{
    uint8_t md[32];
    pkg_sha256(pub, 32, md);
    pkg_hex(md, 16, out);
}
