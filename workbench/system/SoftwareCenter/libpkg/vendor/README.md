# Vendored third-party code

## tweetnacl.c / tweetnacl.h
TweetNaCl 20140427 by Daniel J. Bernstein, Bernard van Gastel, Wesley
Janssen, Tanja Lange, Peter Schwabe, Sjaak Smetsers — **public domain**.
Fetched verbatim from https://tweetnacl.cr.yp.to/20140427/ on 2026-09-05
(sha256 of tweetnacl.c: 02e65bc3013ff2168983365e55906bc783c4c7e0a60d8100f17bb303a17175c4).
Used only for `crypto_sign_open` (Ed25519 verify) and its SHA-512.
`randombytes()` is provided by `pkg_verify.c` (never called on the verify
path, but the symbol must exist).

## credentials_crypto.c / credentials_crypto.h
The self-contained SHA-256/HMAC/HKDF/ChaCha20-Poly1305 reference code of
credentials.library (`workbench/libs/credentials/`, AROS Development Team,
same licence as the rest of the tree), carried here verbatim so libpkg
builds in a tree without credentials.library.  Only
`cred_crypto_sha256_init/update/final` are used (`pkg_verify.c`).  Keep the
two copies identical; the credentials.library copy is the master.
