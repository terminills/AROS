/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Host-side unit tests for the libpkg portable core.
          Build/run: make -C workbench/system/SoftwareCenter/tests

    AI_PHASE: SOFTWARE_CENTER_LIBPKG
    AI_STATUS: IMPLEMENTED
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pkg_core.h"

static int fails = 0, passes = 0;
#define CHECK(cond, ...) do { if (cond) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_stanza(void)
{
    const char *text =
        "# comment\n"
        "Package: Foo\n"
        "Version: 1.2.3\n"
        "Description: First line\n"
        " second line\n"
        " .\n"
        " after blank\n"
        "Weird-Key:   spaced value   \n"
        "\n"
        "\n"
        "Package: Bar\r\n"
        "Size: 12345678901234\n";
    int n = 0;
    struct pkg_stanza *l = pkg_stanza_parse(text, strlen(text), &n);
    char *out;
    size_t ol;

    CHECK(n == 2, "record count %d", n);
    CHECK(l && strcmp(pkg_stanza_get(l, "package"), "Foo") == 0, "ci key lookup");
    CHECK(strcmp(pkg_stanza_get(l, "Description"), "First line\nsecond line\n\nafter blank") == 0,
          "continuation: '%s'", pkg_stanza_get(l, "Description"));
    CHECK(strcmp(pkg_stanza_get(l, "weird-key"), "spaced value") == 0, "trim");
    CHECK(l->next && strcmp(pkg_stanza_get(l->next, "Package"), "Bar") == 0, "second record / CR strip");
    CHECK(pkg_parse_u64(pkg_stanza_get(l->next, "Size")) == 12345678901234ULL, "u64");
    CHECK(pkg_stanza_get(l, "Nope") == NULL, "absent key");

    out = pkg_stanza_write(l, &ol);
    CHECK(out != NULL, "write");
    if (out)
    {
        int n2 = 0;
        struct pkg_stanza *l2 = pkg_stanza_parse(out, ol, &n2);
        CHECK(n2 == 2, "round-trip count");
        CHECK(l2 && strcmp(pkg_stanza_get(l2, "Description"), pkg_stanza_get(l, "Description")) == 0, "round-trip continuation");
        CHECK(strstr(out, "\n .\n") != NULL, "empty paragraph emitted as ' .'");
        pkg_stanza_free(l2);
        free(out);
    }
    CHECK(pkg_stanza_set(l, "Version", "9.9") == 0 && strcmp(pkg_stanza_get(l, "version"), "9.9") == 0, "set replace");
    CHECK(pkg_stanza_set(l, "New", "x") == 0 && strcmp(pkg_stanza_get(l, "New"), "x") == 0, "set append");
    pkg_stanza_free(l);

    {
        char hex[65]; uint64_t sz = 0;
        CHECK(pkg_parse_digest_line("SHA256 ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789 4242", hex, &sz) == 0
              && sz == 4242 && hex[0] == 'a', "digest line");
        CHECK(pkg_parse_digest_line("SHA256 zz", hex, &sz) != 0, "bad digest line");
    }
}

static void test_vercmp(void)
{
    CHECK(pkg_vercmp("1.0.10", "1.0.9") > 0, "1.0.10 > 1.0.9");
    CHECK(pkg_vercmp("1.2", "1.2.0") == 0, "1.2 == 1.2.0");
    CHECK(pkg_vercmp("2.0-2", "2.0-1") > 0, "2.0-2 > 2.0-1");
    CHECK(pkg_vercmp("2.0", "2.0-1") < 0, "2.0 < 2.0-1");
    CHECK(pkg_vercmp("1.10", "1.9") > 0, "1.10 > 1.9");
    CHECK(pkg_vercmp("1.0a", "1.0b") < 0, "alpha compare");
    CHECK(pkg_vercmp("0.9", "1.0") < 0, "0.9 < 1.0");
    CHECK(pkg_vercmp(NULL, "1") < 0, "NULL < 1");
}

static void test_sha256(void)
{
    uint8_t md[32]; char hex[65];
    pkg_sha256("abc", 3, md);
    pkg_hex(md, 32, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "sha256(abc) = %s", hex);
    {
        pkg_sha256_ctx c; uint8_t md2[32];
        pkg_sha256_init(&c);
        pkg_sha256_update(&c, "a", 1);
        pkg_sha256_update(&c, "bc", 2);
        pkg_sha256_final(&c, md2);
        CHECK(memcmp(md, md2, 32) == 0, "streaming sha256");
    }
    {
        uint8_t b[3];
        CHECK(pkg_unhex("0aFf10", b, 3) == 0 && b[0] == 0x0a && b[1] == 0xff && b[2] == 0x10, "unhex");
        CHECK(pkg_unhex("0aFf1", b, 3) != 0, "unhex odd");
        CHECK(pkg_hex_equal("ABC", "abc") && !pkg_hex_equal("abc", "abd"), "hex_equal");
    }
}

static void test_ed25519(void)
{
    /* RFC 8032 §7.1 TEST 1 (empty message) and TEST 2 (one byte 0x72) */
    uint8_t pk[32], sig[64], tampered[64];
    char keyid[33];

    CHECK(pkg_unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", pk, 32) == 0, "pk1 hex");
    CHECK(pkg_unhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", sig, 64) == 0, "sig1 hex");
    CHECK(pkg_ed25519_verify((const uint8_t *)"", 0, sig, pk) == 1, "RFC8032 test 1 verifies");
    memcpy(tampered, sig, 64); tampered[10] ^= 1;
    CHECK(pkg_ed25519_verify((const uint8_t *)"", 0, tampered, pk) == 0, "tampered sig rejected");
    CHECK(pkg_ed25519_verify((const uint8_t *)"x", 1, sig, pk) == 0, "wrong message rejected");

    CHECK(pkg_unhex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", pk, 32) == 0, "pk2 hex");
    CHECK(pkg_unhex("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00", sig, 64) == 0, "sig2 hex");
    CHECK(pkg_ed25519_verify((const uint8_t *)"\x72", 1, sig, pk) == 1, "RFC8032 test 2 verifies");

    pkg_keyid(pk, keyid);
    CHECK(strlen(keyid) == 32, "keyid length");
}

/* --- tar: the Makefile builds tests/fixture.tar.gz with system tar --- */
struct fctx { FILE *f; };
static long freadfn(void *ctx, void *buf, size_t n)
{
    struct fctx *c = ctx;
    size_t r = fread(buf, 1, n, c->f);
    if (r == 0 && ferror(c->f)) return -1;
    return (long)r;
}

static void test_tar(const char *path)
{
    struct fctx c;
    struct pkg_tar *t;
    struct pkg_tar_entry e;
    int rc, files = 0, dirs = 0, saw_pkginfo = 0, saw_big = 0;
    c.f = fopen(path, "rb");
    if (!c.f) { printf("SKIP tar test: %s missing\n", path); return; }
    t = pkg_tar_open(freadfn, &c);
    CHECK(t != NULL, "tar open");
    while ((rc = pkg_tar_next(t, &e)) > 0)
    {
        if (e.type == PKG_TAR_DIR) dirs++;
        else
        {
            char buf[4096]; long got; uint64_t total = 0;
            files++;
            if (strcmp(e.name, "PKGINFO") == 0)
            {
                got = pkg_tar_read(t, buf, sizeof buf - 1);
                CHECK(got > 0, "read PKGINFO");
                if (got > 0) { buf[got] = 0; saw_pkginfo = strstr(buf, "Package: Fixture") != NULL; }
                total = got;
            }
            while ((got = pkg_tar_read(t, buf, sizeof buf)) > 0) total += (uint64_t)got;
            CHECK(got == 0, "entry read end %s (%ld)", e.name, got);
            CHECK(total == e.size, "size match %s %llu/%llu", e.name, (unsigned long long)total, (unsigned long long)e.size);
            if (strcmp(e.name, "files/big.bin") == 0) saw_big = total == 300000;
        }
    }
    CHECK(rc == 0, "tar walk end rc=%d (%s)", rc, pkg_tar_error(t));
    CHECK(saw_pkginfo, "PKGINFO content");
    CHECK(saw_big, "big file spans blocks");
    CHECK(files >= 3 && dirs >= 1, "entries files=%d dirs=%d", files, dirs);
    pkg_tar_close(t);
    fclose(c.f);
}

int main(int argc, char **argv)
{
    test_stanza();
    test_vercmp();
    test_sha256();
    test_ed25519();
    test_tar(argc > 1 ? argv[1] : "fixture.tar.gz");
    printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
