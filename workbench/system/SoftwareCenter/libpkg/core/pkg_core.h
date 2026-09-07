/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: libpkg portable core — stanza format, version compare, gzip+ustar
          reader, hashing and signature verification.  NO AROS headers so
          these modules compile and unit-test on the build host (tests/).

    AI_PHASE: SOFTWARE_CENTER_LIBPKG
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: MEDIUM
*/

#ifndef LIBPKG_PKG_CORE_H
#define LIBPKG_PKG_CORE_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------ */
/* Stanza format (PROTOCOL.md §2)                                            */

struct pkg_field
{
    struct pkg_field *next;
    char             *key;
    char             *value;
};

struct pkg_stanza
{
    struct pkg_stanza *next;
    struct pkg_field  *fields;     /* in file order                          */
    struct pkg_field  *last;
};

/* Parse a whole file.  Returns the head of a record list (NULL for empty
   input or on OOM — *count is -1 on OOM). */
struct pkg_stanza *pkg_stanza_parse(const char *buf, size_t len, int *count);
void               pkg_stanza_free(struct pkg_stanza *list);

/* Case-insensitive key lookup; NULL if absent. */
const char        *pkg_stanza_get(const struct pkg_stanza *s, const char *key);
/* Set (replace) or append a field.  0 on success, -1 on OOM. */
int                pkg_stanza_set(struct pkg_stanza *s, const char *key, const char *value);
struct pkg_stanza *pkg_stanza_new(void);
/* Unlink+free one record from a list; returns new head. */
struct pkg_stanza *pkg_stanza_unlink(struct pkg_stanza *head, struct pkg_stanza *s);

/* Serialise a record list.  Returns a malloc'd NUL-terminated buffer with
   the byte length in *len; NULL on OOM. */
char              *pkg_stanza_write(const struct pkg_stanza *list, size_t *len);

/* Helpers on values */
uint64_t           pkg_parse_u64(const char *s);       /* 0 on garbage      */
int                pkg_parse_bool(const char *s);      /* yes/true/1 → 1    */
/* "SHA256 <hex> <size>" → fills hex[65] and *size; 0 on success */
int                pkg_parse_digest_line(const char *v, char hex[65], uint64_t *size);

/* ------------------------------------------------------------------------ */
/* Version compare (PROTOCOL.md §7)                                          */

int pkg_vercmp(const char *a, const char *b);

/* ------------------------------------------------------------------------ */
/* Hashing / hex                                                             */

void pkg_sha256(const void *data, size_t len, uint8_t md[32]);
void pkg_hex(const uint8_t *in, size_t len, char *out);      /* lowercase, NUL */
int  pkg_unhex(const char *in, uint8_t *out, size_t outlen);  /* 0 ok        */
int  pkg_hex_equal(const char *a, const char *b);             /* ci compare */

/* Streaming SHA-256 (thin wrapper over credentials_crypto.c) */
typedef struct pkg_sha256_ctx { uint8_t opaque[112]; } pkg_sha256_ctx;
void pkg_sha256_init(pkg_sha256_ctx *c);
void pkg_sha256_update(pkg_sha256_ctx *c, const void *p, size_t n);
void pkg_sha256_final(pkg_sha256_ctx *c, uint8_t md[32]);

/* ------------------------------------------------------------------------ */
/* Ed25519 (PROTOCOL.md §5)                                                  */

/* 1 = signature valid, 0 = invalid */
int  pkg_ed25519_verify(const uint8_t *msg, size_t len, const uint8_t sig[64],
                        const uint8_t pub[32]);
/* KeyId = hex of first 16 bytes of SHA-256(pub) → out[33] */
void pkg_keyid(const uint8_t pub[32], char out[33]);

/* ------------------------------------------------------------------------ */
/* gzip + ustar reader (PROTOCOL.md §4)                                      */

#define PKG_TAR_REG  '0'
#define PKG_TAR_DIR  '5'

struct pkg_tar_entry
{
    char     name[256];            /* prefix/name joined, no leading "./"   */
    char     type;                 /* PKG_TAR_REG or PKG_TAR_DIR (others rejected) */
    uint32_t mode;
    uint64_t size;
};

struct pkg_tar;                    /* opaque reader                         */

/* The reader pulls compressed bytes from a caller callback so the archive
   never has to be fully in memory.  read(ctx, buf, n) returns bytes read,
   0 at EOF, <0 on error. */
typedef long (*pkg_tar_readfn)(void *ctx, void *buf, size_t n);

struct pkg_tar *pkg_tar_open(pkg_tar_readfn read, void *ctx);
/* 1 = entry filled, 0 = end of archive, <0 = error (PKG_TARERR_#?) */
int             pkg_tar_next(struct pkg_tar *t, struct pkg_tar_entry *e);
/* Read up to n bytes of the current entry's payload; 0 at entry end, <0 err */
long            pkg_tar_read(struct pkg_tar *t, void *buf, size_t n);
/* Skip whatever is left of the current entry. */
int             pkg_tar_skip(struct pkg_tar *t);
void            pkg_tar_close(struct pkg_tar *t);
const char     *pkg_tar_error(struct pkg_tar *t);

#define PKG_TARERR_IO        -1
#define PKG_TARERR_GZIP      -2
#define PKG_TARERR_FORMAT    -3   /* bad header / checksum / not ustar     */
#define PKG_TARERR_UNSUPPORTED -4 /* symlink, device, pax, gnu long name   */
#define PKG_TARERR_NOMEM     -5

#endif /* LIBPKG_PKG_CORE_H */
