/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    FindPrinters: small HTTP/1.1 client for a printer's own web server. Plain
    http, or https through mbedTLS: a printer serves its pictures and settings
    pages over https with a certificate it made itself, so the certificate is
    not verified - FindPrinters only reads pictures from a printer it has just
    found on the link.
*/

#include <exec/types.h>
#include <proto/exec.h>
#include <proto/bsdsocket.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>

#include <stdlib.h>
#include <string.h>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "psa/crypto.h"

#include "fp_http.h"

extern struct Library *SocketBase;

#define HTTP_TIMEOUT    10

LONG HTTP_LastError;

struct Conn {
    LONG c_Socket;
    BOOL c_TLS;
    mbedtls_ssl_context c_SSL;
    mbedtls_ssl_config c_Conf;
    mbedtls_entropy_context c_Entropy;
    mbedtls_ctr_drbg_context c_DRBG;
};

static int bioSend(void *ctx, const unsigned char *buf, size_t len)
{
    LONG n = send(((struct Conn *)ctx)->c_Socket, (APTR)buf, len, 0);
    return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : n;
}

static int bioRecv(void *ctx, unsigned char *buf, size_t len)
{
    LONG n = recv(((struct Conn *)ctx)->c_Socket, buf, len, 0);
    return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : n;
}

static LONG connWrite(struct Conn *c, const UBYTE *data, LONG len)
{
    LONG done = 0, n;

    while (done < len) {
        n = c->c_TLS ? mbedtls_ssl_write(&c->c_SSL, data + done, len - done)
                     : send(c->c_Socket, (APTR)(data + done), len - done, 0);
        if (n <= 0)
            return -1;
        done += n;
    }
    return done;
}

static LONG connRead(struct Conn *c, UBYTE *buf, LONG len)
{
    LONG n;

    if (!c->c_TLS)
        return recv(c->c_Socket, buf, len, 0);
    n = mbedtls_ssl_read(&c->c_SSL, buf, len);
    if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == 0)
        return 0;
    return n < 0 ? -1 : n;
}

static void connClose(struct Conn *c)
{
    if (c->c_TLS) {
        mbedtls_ssl_close_notify(&c->c_SSL);
        mbedtls_ssl_free(&c->c_SSL);
        mbedtls_ssl_config_free(&c->c_Conf);
        mbedtls_ctr_drbg_free(&c->c_DRBG);
        mbedtls_entropy_free(&c->c_Entropy);
    }
    if (c->c_Socket >= 0)
        CloseSocket(c->c_Socket);
}

static BOOL connOpen(struct Conn *c, const char *host, UWORD port, BOOL tls)
{
    struct sockaddr_in sin;
    struct hostent *he;
    struct timeval tv = { HTTP_TIMEOUT, 0 };

    memset(c, 0, sizeof(*c));
    c->c_Socket = -1;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = inet_addr((STRPTR)host);
    if (sin.sin_addr.s_addr == INADDR_NONE) {
        if (!(he = gethostbyname((STRPTR)host)) || he->h_addrtype != AF_INET) {
            HTTP_LastError = HTTP_ERR_HOST;
            return FALSE;
        }
        memcpy(&sin.sin_addr, he->h_addr, sizeof(sin.sin_addr));
    }
    if ((c->c_Socket = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return FALSE;
    setsockopt(c->c_Socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(c->c_Socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(c->c_Socket, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        HTTP_LastError = HTTP_ERR_CONNECT;
        CloseSocket(c->c_Socket);
        c->c_Socket = -1;
        return FALSE;
    }
    if (!tls)
        return TRUE;

    /* TLS 1.3 works through PSA, which has to be started first. */
    if (psa_crypto_init() != PSA_SUCCESS) {
        HTTP_LastError = HTTP_ERR_TLS_SETUP;
        CloseSocket(c->c_Socket);
        c->c_Socket = -1;
        return FALSE;
    }
    c->c_TLS = TRUE;
    mbedtls_ssl_init(&c->c_SSL);
    mbedtls_ssl_config_init(&c->c_Conf);
    mbedtls_entropy_init(&c->c_Entropy);
    mbedtls_ctr_drbg_init(&c->c_DRBG);
    if (mbedtls_ctr_drbg_seed(&c->c_DRBG, mbedtls_entropy_func, &c->c_Entropy, (const unsigned char *)"FindPrinters", 12) != 0
        || mbedtls_ssl_config_defaults(&c->c_Conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        HTTP_LastError = HTTP_ERR_TLS_SETUP;
        connClose(c);
        return FALSE;
    }
    mbedtls_ssl_conf_authmode(&c->c_Conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&c->c_Conf, mbedtls_ctr_drbg_random, &c->c_DRBG);
    if (mbedtls_ssl_setup(&c->c_SSL, &c->c_Conf) != 0) {
        connClose(c);
        return FALSE;
    }
    mbedtls_ssl_set_hostname(&c->c_SSL, host);
    mbedtls_ssl_set_bio(&c->c_SSL, c, bioSend, bioRecv, NULL);
    for (;;) {
        int r = mbedtls_ssl_handshake(&c->c_SSL);
        if (r == 0)
            return TRUE;
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            HTTP_LastError = r;
            connClose(c);
            return FALSE;
        }
    }
}

/* http[s]://host[:port]/path */
static BOOL splitURL(const char *url, BOOL *tls, char *host, LONG hostmax, UWORD *port, const char **path)
{
    const char *p, *h;
    ULONG n, v = 0;

    if (strncmp(url, "https://", 8) == 0) {
        *tls = TRUE;
        *port = 443;
        p = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        *tls = FALSE;
        *port = 80;
        p = url + 7;
    } else if (strncmp(url, "ipp://", 6) == 0) {
        *tls = FALSE;
        *port = 631;
        p = url + 6;
    } else
        return FALSE;
    h = p;
    while (*p && *p != ':' && *p != '/')
        p++;
    n = p - h;
    if (n == 0 || n >= (ULONG)hostmax)
        return FALSE;
    memcpy(host, h, n);
    host[n] = 0;
    if (*p == ':') {
        for (p++; *p >= '0' && *p <= '9'; p++)
            v = v * 10 + (*p - '0');
        if (v == 0 || v > 65535)
            return FALSE;
        *port = v;
    }
    *path = *p == '/' ? p : "/";
    return TRUE;
}

static void appendNum(char *s, ULONG v)
{
    char d[12];
    LONG i = 0;

    do {
        d[i++] = '0' + v % 10;
        v /= 10;
    } while (v);
    s += strlen(s);
    while (i)
        *s++ = d[--i];
    *s = 0;
}

static LONG hexValue(const char *s)
{
    LONG v = 0;

    for (;; s++) {
        if (*s >= '0' && *s <= '9') v = v * 16 + *s - '0';
        else if (*s >= 'a' && *s <= 'f') v = v * 16 + *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') v = v * 16 + *s - 'A' + 10;
        else return v;
    }
}

/* Removes chunked transfer coding in place; returns the body length. */
static LONG unchunk(UBYTE *body, LONG len)
{
    LONG in = 0, out = 0;

    while (in < len) {
        LONG size = hexValue((const char *)body + in);
        while (in < len && body[in] != '\n')
            in++;
        in++;
        if (size <= 0 || in + size > len)
            break;
        memmove(body + out, body + in, size);
        out += size;
        in += size + 2;
    }
    return out;
}

LONG HTTP_Request(const char *method, const char *url, const char *type, const UBYTE *data, LONG datalen,
    UBYTE *buf, LONG bufmax, UBYTE **body, LONG *bodylen, char *location, LONG locmax)
{
    struct Conn c;
    char host[128], head[512];
    const char *path;
    UWORD port;
    BOOL tls;
    LONG got = 0, n, status = -1, i, hdrend = -1;
    BOOL chunked = FALSE;

    *body = NULL;
    *bodylen = 0;
    HTTP_LastError = 0;
    if (location)
        location[0] = 0;
    if (!splitURL(url, &tls, host, sizeof(host), &port, &path) || strlen(path) > 256) {
        HTTP_LastError = HTTP_ERR_URL;
        return -1;
    }
    if (!connOpen(&c, host, port, tls))
        return -1;

    strcpy(head, method);
    strcat(head, " ");
    strcat(head, path);
    strcat(head, " HTTP/1.1\r\nHost: ");
    strcat(head, host);
    strcat(head, "\r\nUser-Agent: FindPrinters (AROS)\r\nConnection: close\r\n");
    if (data) {
        strcat(head, "Content-Type: ");
        strcat(head, type);
        strcat(head, "\r\nContent-Length: ");
        appendNum(head, datalen);
        strcat(head, "\r\n");
    }
    strcat(head, "\r\n");
    if (connWrite(&c, (UBYTE *)head, strlen(head)) < 0 || (data && connWrite(&c, data, datalen) < 0)) {
        HTTP_LastError = HTTP_ERR_SEND;
        connClose(&c);
        return -1;
    }
    while (got < bufmax - 1 && (n = connRead(&c, buf + got, bufmax - 1 - got)) > 0)
        got += n;
    connClose(&c);
    buf[got] = 0;

    if (got < 12 || strncmp((char *)buf, "HTTP/1.", 7) != 0) {
        HTTP_LastError = HTTP_ERR_REPLY;
        return -1;
    }
    status = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
    for (i = 0; i + 3 < got; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            hdrend = i;
            break;
        }
    if (hdrend < 0)
        return status;
    for (i = 0; i < hdrend; i++) {
        if (buf[i] != '\n')
            continue;
        if (strncasecmp((char *)buf + i + 1, "Transfer-Encoding:", 18) == 0) {
            const char *v = (const char *)buf + i + 19;
            while (*v == ' ')
                v++;
            chunked = strncasecmp(v, "chunked", 7) == 0;
        } else if (location && strncasecmp((char *)buf + i + 1, "Location:", 9) == 0) {
            const char *v = (const char *)buf + i + 10;
            LONG k = 0;
            while (*v == ' ')
                v++;
            while (v[k] && v[k] != '\r' && v[k] != '\n' && k < locmax - 1) {
                location[k] = v[k];
                k++;
            }
            location[k] = 0;
        }
    }
    *body = buf + hdrend + 4;
    *bodylen = got - (hdrend + 4);
    if (chunked)
        *bodylen = unchunk(*body, *bodylen);
    return status;
}

LONG HTTP_Get(const char *url, UBYTE *buf, LONG bufmax, UBYTE **body, LONG *bodylen)
{
    char where[256], next[256];
    LONG status, hops;

    strncpy(where, url, sizeof(where) - 1);
    where[sizeof(where) - 1] = 0;
    for (hops = 0; hops < 3; hops++) {
        status = HTTP_Request("GET", where, NULL, NULL, 0, buf, bufmax, body, bodylen, next, sizeof(next));
        if ((status != 301 && status != 302 && status != 303 && status != 307 && status != 308) || next[0] == 0)
            return status;
        strcpy(where, next);
    }
    return status;
}
