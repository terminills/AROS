/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Embedded, self-contained crypto for credentials.library. NO external crypto
    library (no OpenSSL/libcrypto) and NO AROS headers -- only <string.h> and
    <stdint.h> -- so it links clean into a bare AROS library module (no
    C-runtime/relbase/socket baggage). Behind the credentials_crypto.h interface,
    so an AmiSSL/libcrypto backend can replace this file later with zero change
    to the keyring layer.

    Primitives are canonical, widely-verified reference implementations:
      - SHA-256                (FIPS 180-4)
      - HMAC-SHA256 -> HKDF    (RFC 2104 / RFC 5869)
      - ChaCha20 + Poly1305    (RFC 8439) AEAD for seal/unseal
      - CSPRNG                 x86_64 RDRAND (hardware RNG)
    Poly1305 is the public-domain poly1305-donna (Andrew Moon) 32-bit variant.

    NOTE: RFC-8439 / RFC-5869 test-vector self-checks belong in a host unit test
    (build target) -- tracked as a JOB-0081 follow-up.
*/

#include <string.h>
#include <stdint.h>

#include "credentials_crypto.h"

/* ============================ SHA-256 (FIPS 180-4) ============================ */

typedef struct { uint32_t s[8]; uint64_t bitlen; uint8_t buf[64]; uint32_t n; } sha256;

static const uint32_t K256[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define ROR32(x,n) (((x)>>(n))|((x)<<(32-(n))))

static void sha256_compress(sha256 *c, const uint8_t *p)
{
    uint32_t w[64], a,b,cc,d,e,f,g,h,t1,t2; int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
    for (; i < 64; i++) {
        uint32_t s0 = ROR32(w[i-15],7)^ROR32(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1 = ROR32(w[i-2],17)^ROR32(w[i-2],19)^(w[i-2]>>10);
        w[i] = w[i-16]+s0+w[i-7]+s1;
    }
    a=c->s[0];b=c->s[1];cc=c->s[2];d=c->s[3];e=c->s[4];f=c->s[5];g=c->s[6];h=c->s[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROR32(e,6)^ROR32(e,11)^ROR32(e,25);
        uint32_t ch = (e&f)^((~e)&g);
        t1 = h+S1+ch+K256[i]+w[i];
        uint32_t S0 = ROR32(a,2)^ROR32(a,13)^ROR32(a,22);
        uint32_t maj = (a&b)^(a&cc)^(b&cc);
        t2 = S0+maj;
        h=g;g=f;f=e;e=d+t1;d=cc;cc=b;b=a;a=t1+t2;
    }
    c->s[0]+=a;c->s[1]+=b;c->s[2]+=cc;c->s[3]+=d;c->s[4]+=e;c->s[5]+=f;c->s[6]+=g;c->s[7]+=h;
}

static void sha256_init(sha256 *c)
{
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->s, iv, sizeof iv); c->bitlen = 0; c->n = 0;
}

static void sha256_update(sha256 *c, const uint8_t *p, size_t len)
{
    c->bitlen += (uint64_t)len * 8;
    while (len) {
        uint32_t take = 64 - c->n; if (take > len) take = (uint32_t)len;
        memcpy(c->buf + c->n, p, take); c->n += take; p += take; len -= take;
        if (c->n == 64) { sha256_compress(c, c->buf); c->n = 0; }
    }
}

static void sha256_final(sha256 *c, uint8_t out[32])
{
    uint64_t bl = c->bitlen; int i;
    uint8_t pad = 0x80; sha256_update(c, &pad, 1);
    pad = 0; while (c->n != 56) sha256_update(c, &pad, 1);
    for (i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bl >> (i*8)); sha256_update(c, &b, 1); }
    for (i = 0; i < 8; i++) { out[i*4]=(uint8_t)(c->s[i]>>24);out[i*4+1]=(uint8_t)(c->s[i]>>16);
                              out[i*4+2]=(uint8_t)(c->s[i]>>8);out[i*4+3]=(uint8_t)c->s[i]; }
}

/* ============================ HMAC-SHA256 / HKDF ============================ */

static void hmac_sha256(const uint8_t *key, size_t klen,
                        const uint8_t *msg, size_t mlen, uint8_t out[32])
{
    uint8_t k[64], ipad[64], opad[64], ihash[32]; sha256 c; int i;
    memset(k, 0, 64);
    if (klen > 64) { sha256_init(&c); sha256_update(&c, key, klen); sha256_final(&c, k); }
    else memcpy(k, key, klen);
    for (i = 0; i < 64; i++) { ipad[i] = k[i]^0x36; opad[i] = k[i]^0x5c; }
    sha256_init(&c); sha256_update(&c, ipad, 64); sha256_update(&c, msg, mlen); sha256_final(&c, ihash);
    sha256_init(&c); sha256_update(&c, opad, 64); sha256_update(&c, ihash, 32); sha256_final(&c, out);
}

/* HKDF-SHA256 (RFC 5869), fixed salt from the interface. */
int cred_crypto_hkdf(const unsigned char *seed, int seedlen,
                     const unsigned char *info, int infolen,
                     unsigned char *out, int outlen)
{
    static const uint8_t salt[] = "credentials.library:hkdf:v1";
    uint8_t prk[32], t[32]; int done = 0; uint8_t counter = 1; uint8_t prev_len = 0;

    if (seedlen < 0 || infolen < 0 || outlen < 0 || outlen > 32*255) return 0;
    /* Extract */
    hmac_sha256(salt, sizeof(salt)-1, seed, seedlen, prk);
    /* Expand */
    while (done < outlen) {
        sha256 dummy; (void)dummy;
        /* T(n) = HMAC(PRK, T(n-1) | info | counter) */
        uint8_t blk[32 + 512 + 1]; int bl = 0;
        if (prev_len) { memcpy(blk, t, prev_len); bl = prev_len; }
        if (infolen > 512) return 0;
        memcpy(blk + bl, info, infolen); bl += infolen;
        blk[bl++] = counter;
        hmac_sha256(prk, 32, blk, bl, t);
        { int take = outlen - done; if (take > 32) take = 32; memcpy(out + done, t, take); done += take; }
        prev_len = 32; counter++;
    }
    memset(prk, 0, sizeof prk); memset(t, 0, sizeof t);
    return 1;
}

int cred_crypto_sha256(const unsigned char *data, int len, unsigned char *md32)
{
    sha256 c;
    if (len < 0) return 0;
    sha256_init(&c); sha256_update(&c, data, len); sha256_final(&c, md32);
    return 1;
}

/*
 * Streaming variant for callers hashing data that does not fit in memory
 * (e.g. the Software Center verifying a multi-GB download as it lands on
 * disk).  cred_sha256_ctx is layout-identical to the private sha256 type.
 */
void cred_crypto_sha256_init(cred_sha256_ctx *ctx)
{
    sha256_init((sha256 *)ctx);
}

void cred_crypto_sha256_update(cred_sha256_ctx *ctx, const unsigned char *data, size_t len)
{
    sha256_update((sha256 *)ctx, data, len);
}

void cred_crypto_sha256_final(cred_sha256_ctx *ctx, unsigned char *md32)
{
    sha256_final((sha256 *)ctx, md32);
}

/* ============================ ChaCha20 (RFC 8439) ============================ */

#define ROTL32(x,n) (((x)<<(n))|((x)>>(32-(n))))
#define QR(a,b,c,d) a+=b; d^=a; d=ROTL32(d,16); c+=d; b^=c; b=ROTL32(b,12); \
                    a+=b; d^=a; d=ROTL32(d,8);  c+=d; b^=c; b=ROTL32(b,7)

static void chacha20_block(const uint32_t key[8], uint32_t counter,
                           const uint32_t nonce[3], uint8_t out[64])
{
    uint32_t s[16], x[16]; int i;
    s[0]=0x61707865;s[1]=0x3320646e;s[2]=0x79622d32;s[3]=0x6b206574;
    for (i=0;i<8;i++) s[4+i]=key[i];
    s[12]=counter; s[13]=nonce[0]; s[14]=nonce[1]; s[15]=nonce[2];
    memcpy(x, s, sizeof s);
    for (i=0;i<10;i++) {
        QR(x[0],x[4],x[8], x[12]); QR(x[1],x[5],x[9], x[13]);
        QR(x[2],x[6],x[10],x[14]); QR(x[3],x[7],x[11],x[15]);
        QR(x[0],x[5],x[10],x[15]); QR(x[1],x[6],x[11],x[12]);
        QR(x[2],x[7],x[8], x[13]); QR(x[3],x[4],x[9], x[14]);
    }
    for (i=0;i<16;i++) { uint32_t v = x[i]+s[i];
        out[i*4]=(uint8_t)v;out[i*4+1]=(uint8_t)(v>>8);out[i*4+2]=(uint8_t)(v>>16);out[i*4+3]=(uint8_t)(v>>24); }
}

static void chacha20_xor(const uint32_t key[8], uint32_t counter, const uint32_t nonce[3],
                         const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t ks[64]; size_t i = 0;
    while (i < len) {
        chacha20_block(key, counter++, nonce, ks);
        size_t take = len - i; if (take > 64) take = 64;
        for (size_t j = 0; j < take; j++) out[i+j] = in[i+j] ^ ks[j];
        i += take;
    }
    memset(ks, 0, sizeof ks);
}

/* ============================ Poly1305 (poly1305-donna, PD) ============================ */

typedef struct { uint32_t r[5], h[5], pad[4]; size_t leftover; uint8_t buffer[16]; uint8_t final; } poly1305;

static uint32_t p_load32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}

static void poly1305_init(poly1305 *st, const uint8_t key[32])
{
    st->r[0]=(p_load32(&key[0]))&0x3ffffff;
    st->r[1]=(p_load32(&key[3])>>2)&0x3ffff03;
    st->r[2]=(p_load32(&key[6])>>4)&0x3ffc0ff;
    st->r[3]=(p_load32(&key[9])>>6)&0x3f03fff;
    st->r[4]=(p_load32(&key[12])>>8)&0x00fffff;
    st->h[0]=st->h[1]=st->h[2]=st->h[3]=st->h[4]=0;
    st->pad[0]=p_load32(&key[16]);st->pad[1]=p_load32(&key[20]);
    st->pad[2]=p_load32(&key[24]);st->pad[3]=p_load32(&key[28]);
    st->leftover=0; st->final=0;
}

static void poly1305_blocks(poly1305 *st, const uint8_t *m, size_t bytes)
{
    const uint32_t hibit = st->final ? 0 : (1UL<<24);
    uint32_t r0=st->r[0],r1=st->r[1],r2=st->r[2],r3=st->r[3],r4=st->r[4];
    uint32_t s1=r1*5,s2=r2*5,s3=r3*5,s4=r4*5;
    uint32_t h0=st->h[0],h1=st->h[1],h2=st->h[2],h3=st->h[3],h4=st->h[4];
    while (bytes >= 16) {
        uint64_t d0,d1,d2,d3,d4; uint32_t c;
        h0+=(p_load32(m))&0x3ffffff;
        h1+=(p_load32(m+3)>>2)&0x3ffffff;
        h2+=(p_load32(m+6)>>4)&0x3ffffff;
        h3+=(p_load32(m+9)>>6)&0x3ffffff;
        h4+=(p_load32(m+12)>>8)|hibit;
        d0=(uint64_t)h0*r0+(uint64_t)h1*s4+(uint64_t)h2*s3+(uint64_t)h3*s2+(uint64_t)h4*s1;
        d1=(uint64_t)h0*r1+(uint64_t)h1*r0+(uint64_t)h2*s4+(uint64_t)h3*s3+(uint64_t)h4*s2;
        d2=(uint64_t)h0*r2+(uint64_t)h1*r1+(uint64_t)h2*r0+(uint64_t)h3*s4+(uint64_t)h4*s3;
        d3=(uint64_t)h0*r3+(uint64_t)h1*r2+(uint64_t)h2*r1+(uint64_t)h3*r0+(uint64_t)h4*s4;
        d4=(uint64_t)h0*r4+(uint64_t)h1*r3+(uint64_t)h2*r2+(uint64_t)h3*r1+(uint64_t)h4*r0;
        c=(uint32_t)(d0>>26);h0=(uint32_t)d0&0x3ffffff;d1+=c;
        c=(uint32_t)(d1>>26);h1=(uint32_t)d1&0x3ffffff;d2+=c;
        c=(uint32_t)(d2>>26);h2=(uint32_t)d2&0x3ffffff;d3+=c;
        c=(uint32_t)(d3>>26);h3=(uint32_t)d3&0x3ffffff;d4+=c;
        c=(uint32_t)(d4>>26);h4=(uint32_t)d4&0x3ffffff;h0+=c*5;
        c=h0>>26;h0&=0x3ffffff;h1+=c;
        m+=16; bytes-=16;
    }
    st->h[0]=h0;st->h[1]=h1;st->h[2]=h2;st->h[3]=h3;st->h[4]=h4;
}

static void poly1305_update(poly1305 *st, const uint8_t *m, size_t bytes)
{
    if (st->leftover) {
        size_t want = 16 - st->leftover; if (want > bytes) want = bytes;
        memcpy(st->buffer + st->leftover, m, want); bytes-=want; m+=want; st->leftover+=want;
        if (st->leftover < 16) return;
        poly1305_blocks(st, st->buffer, 16); st->leftover = 0;
    }
    if (bytes >= 16) { size_t want = bytes & ~(size_t)15; poly1305_blocks(st, m, want); m+=want; bytes-=want; }
    if (bytes) { memcpy(st->buffer + st->leftover, m, bytes); st->leftover += bytes; }
}

static void poly1305_finish(poly1305 *st, uint8_t mac[16])
{
    uint32_t h0,h1,h2,h3,h4,c,g0,g1,g2,g3,g4; uint64_t f; uint32_t mask;
    if (st->leftover) {
        size_t i = st->leftover; st->buffer[i++]=1; for (;i<16;i++) st->buffer[i]=0;
        st->final=1; poly1305_blocks(st, st->buffer, 16);
    }
    h0=st->h[0];h1=st->h[1];h2=st->h[2];h3=st->h[3];h4=st->h[4];
    c=h1>>26;h1&=0x3ffffff;h2+=c;c=h2>>26;h2&=0x3ffffff;h3+=c;
    c=h3>>26;h3&=0x3ffffff;h4+=c;c=h4>>26;h4&=0x3ffffff;h0+=c*5;c=h0>>26;h0&=0x3ffffff;h1+=c;
    g0=h0+5;c=g0>>26;g0&=0x3ffffff;g1=h1+c;c=g1>>26;g1&=0x3ffffff;g2=h2+c;c=g2>>26;g2&=0x3ffffff;
    g3=h3+c;c=g3>>26;g3&=0x3ffffff;g4=h4+c-(1UL<<26);
    mask=(g4>>((sizeof(uint32_t)*8)-1))-1;
    g0&=mask;g1&=mask;g2&=mask;g3&=mask;g4&=mask;mask=~mask;
    h0=(h0&mask)|g0;h1=(h1&mask)|g1;h2=(h2&mask)|g2;h3=(h3&mask)|g3;h4=(h4&mask)|g4;
    h0=((h0)|(h1<<26))&0xffffffff;h1=((h1>>6)|(h2<<20))&0xffffffff;
    h2=((h2>>12)|(h3<<14))&0xffffffff;h3=((h3>>18)|(h4<<8))&0xffffffff;
    f=(uint64_t)h0+st->pad[0];h0=(uint32_t)f;
    f=(uint64_t)h1+st->pad[1]+(f>>32);h1=(uint32_t)f;
    f=(uint64_t)h2+st->pad[2]+(f>>32);h2=(uint32_t)f;
    f=(uint64_t)h3+st->pad[3]+(f>>32);h3=(uint32_t)f;
    mac[0]=(uint8_t)h0;mac[1]=(uint8_t)(h0>>8);mac[2]=(uint8_t)(h0>>16);mac[3]=(uint8_t)(h0>>24);
    mac[4]=(uint8_t)h1;mac[5]=(uint8_t)(h1>>8);mac[6]=(uint8_t)(h1>>16);mac[7]=(uint8_t)(h1>>24);
    mac[8]=(uint8_t)h2;mac[9]=(uint8_t)(h2>>8);mac[10]=(uint8_t)(h2>>16);mac[11]=(uint8_t)(h2>>24);
    mac[12]=(uint8_t)h3;mac[13]=(uint8_t)(h3>>8);mac[14]=(uint8_t)(h3>>16);mac[15]=(uint8_t)(h3>>24);
}

/* ============================ ChaCha20-Poly1305 AEAD (RFC 8439) ============================ */

static void aead_tag(const uint32_t key[8], const uint32_t nonce[3],
                     const uint8_t *ct, size_t ctlen, uint8_t tag[16])
{
    uint8_t otk[64]; poly1305 st; uint8_t pad[16]; uint8_t lenblk[16]; size_t rem;
    chacha20_block(key, 0, nonce, otk);         /* one-time Poly1305 key = block 0 */
    poly1305_init(&st, otk);
    poly1305_update(&st, ct, ctlen);            /* empty AAD */
    rem = ctlen % 16;
    if (rem) { memset(pad, 0, 16); poly1305_update(&st, pad, 16 - rem); }
    memset(lenblk, 0, 16);                        /* aad_len=0 (8 bytes) | ct_len (8 bytes LE) */
    { uint64_t L = (uint64_t)ctlen; for (int i=0;i<8;i++) lenblk[8+i]=(uint8_t)(L>>(8*i)); }
    poly1305_update(&st, lenblk, 16);
    poly1305_finish(&st, tag);
    memset(otk, 0, sizeof otk);
}

static void load_key_nonce(const unsigned char *key32, uint32_t key[8],
                           const uint8_t *nonce12, uint32_t nonce[3])
{
    for (int i=0;i<8;i++) key[i]=p_load32(key32+i*4);
    for (int i=0;i<3;i++) nonce[i]=p_load32(nonce12+i*4);
}

int cred_crypto_seal(const unsigned char key[CRED_KEY_LEN],
                     const unsigned char *pt, int ptlen,
                     unsigned char *blob, int blobcap)
{
    uint32_t k[8], n[3];
    unsigned char *nonce = blob, *ct = blob + CRED_NONCE_LEN, *tag;
    if (ptlen < 0 || blobcap < ptlen + CRED_SEAL_OVERHEAD) return -1;
    if (!cred_crypto_random(nonce, CRED_NONCE_LEN)) return -1;
    load_key_nonce(key, k, nonce, n);
    chacha20_xor(k, 1, n, pt, ct, (size_t)ptlen);     /* counter starts at 1 */
    tag = ct + ptlen;
    aead_tag(k, n, ct, (size_t)ptlen, tag);
    memset(k, 0, sizeof k);
    return CRED_NONCE_LEN + ptlen + CRED_TAG_LEN;
}

int cred_crypto_unseal(const unsigned char key[CRED_KEY_LEN],
                       const unsigned char *blob, int bloblen,
                       unsigned char *pt, int ptcap)
{
    uint32_t k[8], n[3];
    const unsigned char *nonce = blob, *ct = blob + CRED_NONCE_LEN;
    int ctlen = bloblen - CRED_SEAL_OVERHEAD;
    const unsigned char *tag = blob + CRED_NONCE_LEN + ctlen;
    uint8_t want[16]; int diff = 0, i;
    if (bloblen < CRED_SEAL_OVERHEAD || ctlen < 0 || ptcap < ctlen) return -1;
    load_key_nonce(key, k, nonce, n);
    aead_tag(k, n, ct, (size_t)ctlen, want);
    for (i = 0; i < 16; i++) diff |= want[i] ^ tag[i];   /* constant-time compare */
    if (diff) { memset(k, 0, sizeof k); return -1; }      /* inauthentic */
    chacha20_xor(k, 1, n, ct, pt, (size_t)ctlen);
    memset(k, 0, sizeof k);
    return ctlen;
}

/* ============================ CSPRNG (x86_64 RDRAND) ============================ */

#if defined(__x86_64__) || defined(__i386__)
static int rdrand32(uint32_t *v)
{
    unsigned char ok; int tries = 32;
    while (tries--) {
        __asm__ volatile ("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok) :: "cc");
        if (ok) return 1;
    }
    return 0;
}
#endif

int cred_crypto_random(unsigned char *out, int len)
{
    if (len < 0) return 0;
#if defined(__x86_64__) || defined(__i386__)
    {
        int i = 0;
        while (i < len) {
            uint32_t v; int take = len - i; if (take > 4) take = 4;
            if (!rdrand32(&v)) return 0;
            memcpy(out + i, &v, take); i += take;
        }
        return 1;
    }
#else
    /* Non-x86 target: no hardware RNG wired yet. The keyring layer must supply
       an entropy source (exec E-Clock + addresses, or a device) -- JOB-0081. */
    (void)out;
    return 0;
#endif
}
