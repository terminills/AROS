/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Stanza format reader/writer (PROTOCOL.md §2) — portable C.

    AI_PHASE: SOFTWARE_CENTER_LIBPKG
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: LOW
*/

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "pkg_core.h"

static int keyeq(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++; b++;
    }
    return *a == *b;
}

static char *dupn(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

struct pkg_stanza *pkg_stanza_new(void)
{
    struct pkg_stanza *s = calloc(1, sizeof *s);
    return s;
}

static void field_free(struct pkg_field *f)
{
    while (f)
    {
        struct pkg_field *n = f->next;
        free(f->key);
        free(f->value);
        free(f);
        f = n;
    }
}

void pkg_stanza_free(struct pkg_stanza *list)
{
    while (list)
    {
        struct pkg_stanza *n = list->next;
        field_free(list->fields);
        free(list);
        list = n;
    }
}

struct pkg_stanza *pkg_stanza_unlink(struct pkg_stanza *head, struct pkg_stanza *s)
{
    struct pkg_stanza **pp = &head;
    while (*pp)
    {
        if (*pp == s)
        {
            *pp = s->next;
            s->next = NULL;
            pkg_stanza_free(s);
            break;
        }
        pp = &(*pp)->next;
    }
    return head;
}

const char *pkg_stanza_get(const struct pkg_stanza *s, const char *key)
{
    const struct pkg_field *f;
    if (!s) return NULL;
    for (f = s->fields; f; f = f->next)
        if (keyeq(f->key, key))
            return f->value;
    return NULL;
}

static int append_field(struct pkg_stanza *s, char *key, char *value)
{
    struct pkg_field *f = calloc(1, sizeof *f);
    if (!f) return -1;
    f->key = key;
    f->value = value;
    if (s->last) s->last->next = f; else s->fields = f;
    s->last = f;
    return 0;
}

int pkg_stanza_set(struct pkg_stanza *s, const char *key, const char *value)
{
    struct pkg_field *f;
    char *v = dupn(value, strlen(value));
    if (!v) return -1;
    for (f = s->fields; f; f = f->next)
    {
        if (keyeq(f->key, key))
        {
            free(f->value);
            f->value = v;
            return 0;
        }
    }
    {
        char *k = dupn(key, strlen(key));
        if (!k || append_field(s, k, v) < 0)
        {
            free(k); free(v);
            return -1;
        }
    }
    return 0;
}

/* Grow-append helper for continuation lines */
static int value_append(struct pkg_field *f, const char *text, size_t n)
{
    size_t ol = strlen(f->value);
    char *nv = realloc(f->value, ol + 1 + n + 1);
    if (!nv) return -1;
    nv[ol] = '\n';
    memcpy(nv + ol + 1, text, n);
    nv[ol + 1 + n] = 0;
    f->value = nv;
    return 0;
}

struct pkg_stanza *pkg_stanza_parse(const char *buf, size_t len, int *count)
{
    struct pkg_stanza *head = NULL, *tail = NULL, *cur = NULL;
    const char *p = buf, *end = buf + len;
    int n = 0;

    if (count) *count = 0;

    while (p < end)
    {
        const char *eol = memchr(p, '\n', end - p);
        const char *line = p;
        size_t ll;
        if (!eol) eol = end;
        ll = eol - line;
        p = (eol < end) ? eol + 1 : end;

        /* tolerate stray CR */
        if (ll && line[ll - 1] == '\r') ll--;

        if (ll == 0)
        {
            cur = NULL;                     /* record separator */
            continue;
        }
        if (line[0] == '#')
            continue;

        if (line[0] == ' ' || line[0] == '\t')
        {
            /* continuation */
            if (!cur || !cur->last)
                continue;                   /* orphan continuation: ignore  */
            if (ll == 2 && line[1] == '.')
            {
                if (value_append(cur->last, "", 0) < 0) goto oom;
            }
            else if (value_append(cur->last, line + 1, ll - 1) < 0)
                goto oom;
            continue;
        }

        {
            const char *colon = memchr(line, ':', ll);
            const char *v;
            size_t kl, vl;
            char *k, *val;
            if (!colon)
                continue;                   /* junk line: ignore            */
            kl = colon - line;
            v = colon + 1;
            while (v < line + ll && (*v == ' ' || *v == '\t')) v++;
            vl = (line + ll) - v;
            while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;

            if (!cur)
            {
                cur = pkg_stanza_new();
                if (!cur) goto oom;
                if (tail) tail->next = cur; else head = cur;
                tail = cur;
                n++;
            }
            k = dupn(line, kl);
            val = dupn(v, vl);
            if (!k || !val || append_field(cur, k, val) < 0)
            {
                free(k); free(val);
                goto oom;
            }
        }
    }
    if (count) *count = n;
    return head;

oom:
    pkg_stanza_free(head);
    if (count) *count = -1;
    return NULL;
}

char *pkg_stanza_write(const struct pkg_stanza *list, size_t *len)
{
    size_t cap = 1024, used = 0;
    char *out = malloc(cap);
    const struct pkg_stanza *s;

    if (!out) return NULL;

#define PUT(ptr, n) do { \
        if (used + (n) + 1 > cap) { \
            size_t nc = cap; char *no; \
            while (used + (n) + 1 > nc) nc *= 2; \
            no = realloc(out, nc); \
            if (!no) { free(out); return NULL; } \
            out = no; cap = nc; \
        } \
        memcpy(out + used, (ptr), (n)); used += (n); \
    } while (0)

    for (s = list; s; s = s->next)
    {
        const struct pkg_field *f;
        for (f = s->fields; f; f = f->next)
        {
            const char *v = f->value;
            PUT(f->key, strlen(f->key));
            PUT(": ", 2);
            /* first line, then continuation lines */
            for (;;)
            {
                const char *nl = strchr(v, '\n');
                size_t seg = nl ? (size_t)(nl - v) : strlen(v);
                if (v != f->value)
                {
                    if (seg == 0) PUT(" .", 2);
                    else { PUT(" ", 1); PUT(v, seg); }
                }
                else
                    PUT(v, seg);
                PUT("\n", 1);
                if (!nl) break;
                v = nl + 1;
            }
        }
        if (s->next) PUT("\n", 1);
    }
#undef PUT
    out[used] = 0;
    if (len) *len = used;
    return out;
}

uint64_t pkg_parse_u64(const char *s)
{
    uint64_t v = 0;
    if (!s) return 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9')
    {
        v = v * 10 + (uint64_t)(*s - '0');
        s++;
    }
    return v;
}

int pkg_parse_bool(const char *s)
{
    if (!s) return 0;
    return keyeq(s, "yes") || keyeq(s, "true") || keyeq(s, "1") || keyeq(s, "on");
}

int pkg_parse_digest_line(const char *v, char hex[65], uint64_t *size)
{
    const char *p;
    int i;
    if (!v) return -1;
    while (*v == ' ') v++;
    if (strncmp(v, "SHA256", 6) != 0) return -1;
    v += 6;
    while (*v == ' ') v++;
    for (i = 0; i < 64; i++)
    {
        if (!isxdigit((unsigned char)v[i])) return -1;
        hex[i] = (char)tolower((unsigned char)v[i]);
    }
    hex[64] = 0;
    p = v + 64;
    if (*p != ' ') return -1;
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return -1;
    if (size) *size = pkg_parse_u64(p);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Version compare                                                           */

static int cmp_component(const char *a, size_t al, const char *b, size_t bl)
{
    size_t i;
    int anum = al > 0, bnum = bl > 0;
    for (i = 0; i < al; i++) if (!isdigit((unsigned char)a[i])) { anum = 0; break; }
    for (i = 0; i < bl; i++) if (!isdigit((unsigned char)b[i])) { bnum = 0; break; }
    if (al == 0) anum = 1;    /* missing component == 0 */
    if (bl == 0) bnum = 1;
    if (anum && bnum)
    {
        uint64_t x = 0, y = 0;
        for (i = 0; i < al; i++) x = x * 10 + (uint64_t)(a[i] - '0');
        for (i = 0; i < bl; i++) y = y * 10 + (uint64_t)(b[i] - '0');
        return (x > y) - (x < y);
    }
    {
        size_t n = al < bl ? al : bl;
        int r = memcmp(a, b, n);
        if (r) return r < 0 ? -1 : 1;
        return (al > bl) - (al < bl);
    }
}

static int cmp_dotted(const char *a, size_t al, const char *b, size_t bl)
{
    const char *ae = a + al, *be = b + bl;
    for (;;)
    {
        const char *ad = memchr(a, '.', ae - a);
        const char *bd = memchr(b, '.', be - b);
        size_t acl = (ad ? ad : ae) - a;
        size_t bcl = (bd ? bd : be) - b;
        int r;
        if (a >= ae && b >= be) return 0;
        r = cmp_component(a, a < ae ? acl : 0, b, b < be ? bcl : 0);
        if (r) return r;
        a = ad ? ad + 1 : ae;
        b = bd ? bd + 1 : be;
        if (a >= ae && b >= be) return 0;
    }
}

int pkg_vercmp(const char *a, const char *b)
{
    const char *ar, *br;
    size_t aul, bul;
    int r;
    if (!a) a = "";
    if (!b) b = "";
    ar = strchr(a, '-');
    br = strchr(b, '-');
    aul = ar ? (size_t)(ar - a) : strlen(a);
    bul = br ? (size_t)(br - b) : strlen(b);
    r = cmp_dotted(a, aul, b, bul);
    if (r) return r;
    ar = ar ? ar + 1 : "";
    br = br ? br + 1 : "";
    return cmp_dotted(ar, strlen(ar), br, strlen(br));
}
