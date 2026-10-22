/*
 * Big numbers and elliptic curves on mbed TLS for the internal crypto backend
 * Copyright (c) 2026, Neil Cafferkey
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 *
 * The internal crypto implementation (CONFIG_TLS=internal) has no big number
 * or elliptic curve arithmetic, which SAE needs. This file provides the
 * crypto_bignum_* and crypto_ec_* parts of crypto.h on libmbedcrypto; the
 * hashes, ciphers and the TLS client stay internal. Random numbers come from
 * wpa_supplicant's own pool, not mbed TLS's DRBG.
 */

#include "includes.h"

#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <mbedtls/bignum.h>
#include <mbedtls/ecp.h>

#include "common.h"
#include "random.h"
#include "crypto.h"


struct crypto_ec {
	mbedtls_ecp_group grp;
	mbedtls_mpi a;		/* A as a value; mbed TLS leaves -3 implicit */
	int group;
};


static int crypto_mbedtls_rng(void *ctx, unsigned char *buf, size_t len)
{
	if (random_get_bytes(buf, len) < 0)
		return MBEDTLS_ERR_ECP_RANDOM_FAILED;
	return 0;
}


/* Modular exponentiation and Diffie-Hellman (dh_groups.c), which the
 * internal backend only provides together with its TLS client */

int crypto_mod_exp(const u8 *base, size_t base_len,
		   const u8 *power, size_t power_len,
		   const u8 *modulus, size_t modulus_len,
		   u8 *result, size_t *result_len)
{
	mbedtls_mpi b, e, m, r;
	size_t len;
	int ret = -1;

	mbedtls_mpi_init(&b);
	mbedtls_mpi_init(&e);
	mbedtls_mpi_init(&m);
	mbedtls_mpi_init(&r);

	if (mbedtls_mpi_read_binary(&b, base, base_len) != 0 ||
	    mbedtls_mpi_read_binary(&e, power, power_len) != 0 ||
	    mbedtls_mpi_read_binary(&m, modulus, modulus_len) != 0 ||
	    mbedtls_mpi_exp_mod(&r, &b, &e, &m, NULL) != 0)
		goto fail;

	len = mbedtls_mpi_size(&r);
	if (len > *result_len)
		goto fail;
	if (mbedtls_mpi_write_binary(&r, result, len) != 0)
		goto fail;
	*result_len = len;
	ret = 0;

fail:
	mbedtls_mpi_free(&r);
	mbedtls_mpi_free(&m);
	mbedtls_mpi_free(&e);
	mbedtls_mpi_free(&b);
	return ret;
}


int crypto_dh_init(u8 generator, const u8 *prime, size_t prime_len, u8 *privkey,
		   u8 *pubkey)
{
	size_t pubkey_len, pad;

	if (random_get_bytes(privkey, prime_len) < 0)
		return -1;
	if (os_memcmp(privkey, prime, prime_len) > 0) {
		/* Make sure private value is smaller than prime */
		privkey[0] = 0;
	}

	pubkey_len = prime_len;
	if (crypto_mod_exp(&generator, 1, privkey, prime_len, prime, prime_len,
			   pubkey, &pubkey_len) < 0)
		return -1;
	if (pubkey_len < prime_len) {
		pad = prime_len - pubkey_len;
		os_memmove(pubkey + pad, pubkey, pubkey_len);
		os_memset(pubkey, 0, pad);
	}

	return 0;
}


int crypto_dh_derive_secret(u8 generator, const u8 *prime, size_t prime_len,
			    const u8 *order, size_t order_len,
			    const u8 *privkey, size_t privkey_len,
			    const u8 *pubkey, size_t pubkey_len,
			    u8 *secret, size_t *len)
{
	mbedtls_mpi pub, p, q, tmp;
	int res = -1;

	if (pubkey_len > prime_len ||
	    (pubkey_len == prime_len &&
	     os_memcmp(pubkey, prime, prime_len) >= 0))
		return -1;

	mbedtls_mpi_init(&pub);
	mbedtls_mpi_init(&p);
	mbedtls_mpi_init(&q);
	mbedtls_mpi_init(&tmp);

	if (mbedtls_mpi_read_binary(&pub, pubkey, pubkey_len) != 0 ||
	    mbedtls_mpi_cmp_int(&pub, 1) <= 0)
		goto fail;

	if (order) {
		/* verify: pubkey^q == 1 mod p */
		if (mbedtls_mpi_read_binary(&p, prime, prime_len) != 0 ||
		    mbedtls_mpi_read_binary(&q, order, order_len) != 0 ||
		    mbedtls_mpi_exp_mod(&tmp, &pub, &q, &p, NULL) != 0 ||
		    mbedtls_mpi_cmp_int(&tmp, 1) != 0)
			goto fail;
	}

	res = crypto_mod_exp(pubkey, pubkey_len, privkey, privkey_len,
			     prime, prime_len, secret, len);
fail:
	mbedtls_mpi_free(&tmp);
	mbedtls_mpi_free(&q);
	mbedtls_mpi_free(&p);
	mbedtls_mpi_free(&pub);
	return res;
}


/* Big numbers */

struct crypto_bignum * crypto_bignum_init(void)
{
	mbedtls_mpi *n = os_malloc(sizeof(*n));

	if (n == NULL)
		return NULL;
	mbedtls_mpi_init(n);
	return (struct crypto_bignum *) n;
}


struct crypto_bignum * crypto_bignum_init_set(const u8 *buf, size_t len)
{
	mbedtls_mpi *n = (mbedtls_mpi *) crypto_bignum_init();

	if (n == NULL)
		return NULL;
	if (mbedtls_mpi_read_binary(n, buf, len) != 0) {
		crypto_bignum_deinit((struct crypto_bignum *) n, 0);
		return NULL;
	}
	return (struct crypto_bignum *) n;
}


struct crypto_bignum * crypto_bignum_init_uint(unsigned int val)
{
	mbedtls_mpi *n = (mbedtls_mpi *) crypto_bignum_init();

	if (n == NULL)
		return NULL;
	if (mbedtls_mpi_lset(n, val) != 0) {
		crypto_bignum_deinit((struct crypto_bignum *) n, 0);
		return NULL;
	}
	return (struct crypto_bignum *) n;
}


void crypto_bignum_deinit(struct crypto_bignum *n, int clear)
{
	if (n == NULL)
		return;
	/* mbedtls_mpi_free zeroises the limbs */
	mbedtls_mpi_free((mbedtls_mpi *) n);
	os_free(n);
}


int crypto_bignum_to_bin(const struct crypto_bignum *a,
			 u8 *buf, size_t buflen, size_t padlen)
{
	size_t num_bytes = mbedtls_mpi_size((const mbedtls_mpi *) a);
	size_t len = num_bytes;

	if (padlen > buflen)
		return -1;
	if (padlen > len)
		len = padlen;
	if (len > buflen)
		return -1;

	if (mbedtls_mpi_write_binary((const mbedtls_mpi *) a, buf, len) != 0)
		return -1;
	return (int) len;
}


int crypto_bignum_rand(struct crypto_bignum *r, const struct crypto_bignum *m)
{
	return mbedtls_mpi_random((mbedtls_mpi *) r, 0, (const mbedtls_mpi *) m,
				  crypto_mbedtls_rng, NULL) == 0 ? 0 : -1;
}


int crypto_bignum_add(const struct crypto_bignum *a,
		      const struct crypto_bignum *b,
		      struct crypto_bignum *c)
{
	return mbedtls_mpi_add_mpi((mbedtls_mpi *) c, (const mbedtls_mpi *) a,
				   (const mbedtls_mpi *) b) == 0 ? 0 : -1;
}


int crypto_bignum_mod(const struct crypto_bignum *a,
		      const struct crypto_bignum *b,
		      struct crypto_bignum *c)
{
	return mbedtls_mpi_mod_mpi((mbedtls_mpi *) c, (const mbedtls_mpi *) a,
				   (const mbedtls_mpi *) b) == 0 ? 0 : -1;
}


int crypto_bignum_exptmod(const struct crypto_bignum *a,
			  const struct crypto_bignum *b,
			  const struct crypto_bignum *c,
			  struct crypto_bignum *d)
{
	return mbedtls_mpi_exp_mod((mbedtls_mpi *) d, (const mbedtls_mpi *) a,
				   (const mbedtls_mpi *) b,
				   (const mbedtls_mpi *) c, NULL) == 0 ? 0 : -1;
}


int crypto_bignum_inverse(const struct crypto_bignum *a,
			  const struct crypto_bignum *b,
			  struct crypto_bignum *c)
{
	return mbedtls_mpi_inv_mod((mbedtls_mpi *) c, (const mbedtls_mpi *) a,
				   (const mbedtls_mpi *) b) == 0 ? 0 : -1;
}


int crypto_bignum_sub(const struct crypto_bignum *a,
		      const struct crypto_bignum *b,
		      struct crypto_bignum *c)
{
	return mbedtls_mpi_sub_mpi((mbedtls_mpi *) c, (const mbedtls_mpi *) a,
				   (const mbedtls_mpi *) b) == 0 ? 0 : -1;
}


int crypto_bignum_div(const struct crypto_bignum *a,
		      const struct crypto_bignum *b,
		      struct crypto_bignum *c)
{
	return mbedtls_mpi_div_mpi((mbedtls_mpi *) c, NULL,
				   (const mbedtls_mpi *) a,
				   (const mbedtls_mpi *) b) == 0 ? 0 : -1;
}


int crypto_bignum_addmod(const struct crypto_bignum *a,
			 const struct crypto_bignum *b,
			 const struct crypto_bignum *c,
			 struct crypto_bignum *d)
{
	mbedtls_mpi tmp;
	int ret;

	mbedtls_mpi_init(&tmp);
	ret = mbedtls_mpi_add_mpi(&tmp, (const mbedtls_mpi *) a,
				  (const mbedtls_mpi *) b);
	if (ret == 0)
		ret = mbedtls_mpi_mod_mpi((mbedtls_mpi *) d, &tmp,
					  (const mbedtls_mpi *) c);
	mbedtls_mpi_free(&tmp);
	return ret == 0 ? 0 : -1;
}


int crypto_bignum_mulmod(const struct crypto_bignum *a,
			 const struct crypto_bignum *b,
			 const struct crypto_bignum *c,
			 struct crypto_bignum *d)
{
	mbedtls_mpi tmp;
	int ret;

	mbedtls_mpi_init(&tmp);
	ret = mbedtls_mpi_mul_mpi(&tmp, (const mbedtls_mpi *) a,
				  (const mbedtls_mpi *) b);
	if (ret == 0)
		ret = mbedtls_mpi_mod_mpi((mbedtls_mpi *) d, &tmp,
					  (const mbedtls_mpi *) c);
	mbedtls_mpi_free(&tmp);
	return ret == 0 ? 0 : -1;
}


int crypto_bignum_sqrmod(const struct crypto_bignum *a,
			 const struct crypto_bignum *b,
			 struct crypto_bignum *c)
{
	return crypto_bignum_mulmod(a, a, b, c);
}


int crypto_bignum_rshift(const struct crypto_bignum *a, int n,
			 struct crypto_bignum *r)
{
	if (mbedtls_mpi_copy((mbedtls_mpi *) r, (const mbedtls_mpi *) a) != 0)
		return -1;
	return mbedtls_mpi_shift_r((mbedtls_mpi *) r, n) == 0 ? 0 : -1;
}


int crypto_bignum_cmp(const struct crypto_bignum *a,
		      const struct crypto_bignum *b)
{
	return mbedtls_mpi_cmp_mpi((const mbedtls_mpi *) a,
				   (const mbedtls_mpi *) b);
}


int crypto_bignum_is_zero(const struct crypto_bignum *a)
{
	return mbedtls_mpi_cmp_int((const mbedtls_mpi *) a, 0) == 0;
}


int crypto_bignum_is_one(const struct crypto_bignum *a)
{
	return mbedtls_mpi_cmp_int((const mbedtls_mpi *) a, 1) == 0;
}


int crypto_bignum_is_odd(const struct crypto_bignum *a)
{
	return mbedtls_mpi_get_bit((const mbedtls_mpi *) a, 0) == 1;
}


int crypto_bignum_legendre(const struct crypto_bignum *a,
			   const struct crypto_bignum *p)
{
	/* Euler's criterion: a^((p-1)/2) mod p */
	mbedtls_mpi exp, tmp;
	int res = -2;

	mbedtls_mpi_init(&exp);
	mbedtls_mpi_init(&tmp);

	if (mbedtls_mpi_sub_int(&exp, (const mbedtls_mpi *) p, 1) != 0 ||
	    mbedtls_mpi_shift_r(&exp, 1) != 0 ||
	    mbedtls_mpi_exp_mod(&tmp, (const mbedtls_mpi *) a, &exp,
				(const mbedtls_mpi *) p, NULL) != 0)
		goto fail;

	if (mbedtls_mpi_cmp_int(&tmp, 1) == 0)
		res = 1;
	else if (mbedtls_mpi_cmp_int(&tmp, 0) == 0)
		res = 0;
	else
		res = -1;

fail:
	mbedtls_mpi_free(&tmp);
	mbedtls_mpi_free(&exp);
	return res;
}


/* Elliptic curves */

static mbedtls_ecp_group_id crypto_mbedtls_group_id(int group)
{
	/* IANA "Group Description" numbers, as used by SAE */
	switch (group) {
	case 19:
		return MBEDTLS_ECP_DP_SECP256R1;
	case 20:
		return MBEDTLS_ECP_DP_SECP384R1;
	case 21:
		return MBEDTLS_ECP_DP_SECP521R1;
#ifdef MBEDTLS_ECP_DP_SECP192R1_ENABLED
	case 25:
		return MBEDTLS_ECP_DP_SECP192R1;
#endif
#ifdef MBEDTLS_ECP_DP_SECP224R1_ENABLED
	case 26:
		return MBEDTLS_ECP_DP_SECP224R1;
#endif
#ifdef MBEDTLS_ECP_DP_BP256R1_ENABLED
	case 28:
		return MBEDTLS_ECP_DP_BP256R1;
#endif
#ifdef MBEDTLS_ECP_DP_BP384R1_ENABLED
	case 29:
		return MBEDTLS_ECP_DP_BP384R1;
#endif
#ifdef MBEDTLS_ECP_DP_BP512R1_ENABLED
	case 30:
		return MBEDTLS_ECP_DP_BP512R1;
#endif
	default:
		return MBEDTLS_ECP_DP_NONE;
	}
}


struct crypto_ec * crypto_ec_init(int group)
{
	struct crypto_ec *e;
	mbedtls_ecp_group_id id = crypto_mbedtls_group_id(group);

	if (id == MBEDTLS_ECP_DP_NONE)
		return NULL;

	e = os_zalloc(sizeof(*e));
	if (e == NULL)
		return NULL;
	e->group = group;
	mbedtls_ecp_group_init(&e->grp);
	mbedtls_mpi_init(&e->a);

	if (mbedtls_ecp_group_load(&e->grp, id) != 0 ||
	    mbedtls_ecp_get_type(&e->grp) != MBEDTLS_ECP_TYPE_SHORT_WEIERSTRASS)
		goto fail;

	/* mbed TLS keeps A empty for the NIST curves, where A = -3 */
	if (e->grp.A.p == NULL) {
		if (mbedtls_mpi_sub_int(&e->a, &e->grp.P, 3) != 0)
			goto fail;
	} else if (mbedtls_mpi_copy(&e->a, &e->grp.A) != 0)
		goto fail;

	return e;

fail:
	crypto_ec_deinit(e);
	return NULL;
}


void crypto_ec_deinit(struct crypto_ec *e)
{
	if (e == NULL)
		return;
	mbedtls_mpi_free(&e->a);
	mbedtls_ecp_group_free(&e->grp);
	os_free(e);
}


size_t crypto_ec_prime_len(struct crypto_ec *e)
{
	return mbedtls_mpi_size(&e->grp.P);
}


size_t crypto_ec_prime_len_bits(struct crypto_ec *e)
{
	return e->grp.pbits;
}


size_t crypto_ec_order_len(struct crypto_ec *e)
{
	return mbedtls_mpi_size(&e->grp.N);
}


const struct crypto_bignum * crypto_ec_get_prime(struct crypto_ec *e)
{
	return (const struct crypto_bignum *) &e->grp.P;
}


const struct crypto_bignum * crypto_ec_get_order(struct crypto_ec *e)
{
	return (const struct crypto_bignum *) &e->grp.N;
}


const struct crypto_bignum * crypto_ec_get_a(struct crypto_ec *e)
{
	return (const struct crypto_bignum *) &e->a;
}


const struct crypto_bignum * crypto_ec_get_b(struct crypto_ec *e)
{
	return (const struct crypto_bignum *) &e->grp.B;
}


const struct crypto_ec_point * crypto_ec_get_generator(struct crypto_ec *e)
{
	return (const struct crypto_ec_point *) &e->grp.G;
}


struct crypto_ec_point * crypto_ec_point_init(struct crypto_ec *e)
{
	mbedtls_ecp_point *p = os_malloc(sizeof(*p));

	if (p == NULL)
		return NULL;
	mbedtls_ecp_point_init(p);
	return (struct crypto_ec_point *) p;
}


void crypto_ec_point_deinit(struct crypto_ec_point *p, int clear)
{
	if (p == NULL)
		return;
	mbedtls_ecp_point_free((mbedtls_ecp_point *) p);
	os_free(p);
}


int crypto_ec_point_x(struct crypto_ec *e, const struct crypto_ec_point *p,
		      struct crypto_bignum *x)
{
	const mbedtls_ecp_point *pt = (const mbedtls_ecp_point *) p;

	return mbedtls_mpi_copy((mbedtls_mpi *) x, &pt->X) == 0 ? 0 : -1;
}


int crypto_ec_point_to_bin(struct crypto_ec *e,
			   const struct crypto_ec_point *point, u8 *x, u8 *y)
{
	const mbedtls_ecp_point *pt = (const mbedtls_ecp_point *) point;
	size_t len = crypto_ec_prime_len(e);

	if (x && mbedtls_mpi_write_binary(&pt->X, x, len) != 0)
		return -1;
	if (y && mbedtls_mpi_write_binary(&pt->Y, y, len) != 0)
		return -1;
	return 0;
}


struct crypto_ec_point * crypto_ec_point_from_bin(struct crypto_ec *e,
						  const u8 *val)
{
	mbedtls_ecp_point *pt;
	size_t len = crypto_ec_prime_len(e);
	u8 *buf;
	int ret;

	pt = (mbedtls_ecp_point *) crypto_ec_point_init(e);
	if (pt == NULL)
		return NULL;

	buf = os_malloc(1 + 2 * len);
	if (buf == NULL) {
		crypto_ec_point_deinit((struct crypto_ec_point *) pt, 0);
		return NULL;
	}
	buf[0] = 0x04;	/* uncompressed */
	os_memcpy(buf + 1, val, 2 * len);
	ret = mbedtls_ecp_point_read_binary(&e->grp, pt, buf, 1 + 2 * len);
	forced_memzero(buf, 1 + 2 * len);
	os_free(buf);

	if (ret != 0) {
		crypto_ec_point_deinit((struct crypto_ec_point *) pt, 0);
		return NULL;
	}
	return (struct crypto_ec_point *) pt;
}


int crypto_ec_point_add(struct crypto_ec *e, const struct crypto_ec_point *a,
			const struct crypto_ec_point *b,
			struct crypto_ec_point *c)
{
	mbedtls_mpi one;
	int ret;

	mbedtls_mpi_init(&one);
	ret = mbedtls_mpi_lset(&one, 1);
	if (ret == 0)
		ret = mbedtls_ecp_muladd(&e->grp, (mbedtls_ecp_point *) c,
					 &one, (const mbedtls_ecp_point *) a,
					 &one, (const mbedtls_ecp_point *) b);
	mbedtls_mpi_free(&one);
	return ret == 0 ? 0 : -1;
}


int crypto_ec_point_mul(struct crypto_ec *e, const struct crypto_ec_point *p,
			const struct crypto_bignum *b,
			struct crypto_ec_point *res)
{
	return mbedtls_ecp_mul(&e->grp, (mbedtls_ecp_point *) res,
			       (const mbedtls_mpi *) b,
			       (const mbedtls_ecp_point *) p,
			       crypto_mbedtls_rng, NULL) == 0 ? 0 : -1;
}


int crypto_ec_point_invert(struct crypto_ec *e, struct crypto_ec_point *p)
{
	mbedtls_ecp_point *pt = (mbedtls_ecp_point *) p;

	if (mbedtls_ecp_is_zero(pt))
		return 0;
	if (mbedtls_mpi_cmp_int(&pt->Y, 0) == 0)
		return 0;
	/* -P = (x, p - y) */
	return mbedtls_mpi_sub_mpi(&pt->Y, &e->grp.P, &pt->Y) == 0 ? 0 : -1;
}


struct crypto_bignum *
crypto_ec_point_compute_y_sqr(struct crypto_ec *e,
			      const struct crypto_bignum *x)
{
	/* y^2 = x^3 + ax + b mod p */
	mbedtls_mpi *y_sqr = (mbedtls_mpi *) crypto_bignum_init();
	mbedtls_mpi tmp;
	int ret;

	if (y_sqr == NULL)
		return NULL;
	mbedtls_mpi_init(&tmp);

	ret = mbedtls_mpi_mul_mpi(&tmp, (const mbedtls_mpi *) x,
				  (const mbedtls_mpi *) x);
	if (ret == 0)
		ret = mbedtls_mpi_mod_mpi(&tmp, &tmp, &e->grp.P);
	if (ret == 0)
		ret = mbedtls_mpi_add_mpi(&tmp, &tmp, &e->a);	/* x^2 + a */
	if (ret == 0)
		ret = mbedtls_mpi_mul_mpi(&tmp, &tmp,
					  (const mbedtls_mpi *) x);
	if (ret == 0)
		ret = mbedtls_mpi_add_mpi(&tmp, &tmp, &e->grp.B);
	if (ret == 0)
		ret = mbedtls_mpi_mod_mpi(y_sqr, &tmp, &e->grp.P);

	mbedtls_mpi_free(&tmp);
	if (ret != 0) {
		crypto_bignum_deinit((struct crypto_bignum *) y_sqr, 1);
		return NULL;
	}
	return (struct crypto_bignum *) y_sqr;
}


int crypto_ec_point_is_at_infinity(struct crypto_ec *e,
				   const struct crypto_ec_point *p)
{
	return mbedtls_ecp_is_zero((mbedtls_ecp_point *) p);
}


int crypto_ec_point_is_on_curve(struct crypto_ec *e,
				const struct crypto_ec_point *p)
{
	return mbedtls_ecp_check_pubkey(&e->grp,
					(const mbedtls_ecp_point *) p) == 0;
}


int crypto_ec_point_cmp(const struct crypto_ec *e,
			const struct crypto_ec_point *a,
			const struct crypto_ec_point *b)
{
	return mbedtls_ecp_point_cmp((const mbedtls_ecp_point *) a,
				     (const mbedtls_ecp_point *) b) == 0 ?
		0 : 1;
}
