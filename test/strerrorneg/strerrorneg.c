/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Regression test for the negative-errno out-of-bounds read in
          strerror()/strerror_r().

    Before posixc 3aa58032 / stdc c113d49e, a negative argument indexed BEFORE
    _errstrings[] - MIN(n, ELAST+1) returns n unchanged when n is negative - and
    the out-of-bounds value, usually non-NULL, was handed back as a string. The
    caller then did strlen() on it and took a page fault.

    This is not hypothetical: it is the AROS Chromium "SSL_do_handshake crash".
    Every Chromium net error is negative by design, and -109 is
    net::ERR_ADDRESS_UNREACHABLE, so any net error that reached strerror()
    killed the browser.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>

/* The exact value from the field crash report, plus the rest of the net error
   range Chromium would hand over, plus the edges. */
static const int cases[] =
{
    -109,           /* net::ERR_ADDRESS_UNREACHABLE - the reported crash */
    -1, -2, -7, -100, -105, -118, -200, -1000, -32768,
    0, 1, 2,
};

int main(void)
{
    int i;
    int failed = 0;

    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++)
    {
        int   n = cases[i];
        char  buf[256];
        char *s;
        int   rc;

        /* strerror(): must return a readable string, never a bogus pointer.
           strlen() here is the operation that used to fault. */
        s = strerror(n);
        if (s == NULL)
        {
            printf("STRERRNEG: FAIL strerror(%d) returned NULL\n", n);
            failed++;
            continue;
        }
        if (strlen(s) == 0 || strlen(s) > 200)
        {
            printf("STRERRNEG: FAIL strerror(%d) implausible length %lu\n",
                   n, (unsigned long)strlen(s));
            failed++;
            continue;
        }

        /* strerror_r(): the path Chromium actually takes, through
           base::safe_strerror() -> wrap_posix_strerror_r(). */
        buf[0] = '\0';
        rc = strerror_r(n, buf, sizeof(buf));
        if (buf[0] == '\0')
        {
            printf("STRERRNEG: FAIL strerror_r(%d) left the buffer empty"
                   " (rc=%d)\n", n, rc);
            failed++;
            continue;
        }

        printf("STRERRNEG: ok %7d -> \"%s\" / \"%s\"\n", n, s, buf);
    }

    if (failed)
    {
        printf("STRERRNEG: FAIL %d case(s)\n", failed);
        return 20;
    }

    printf("STRERRNEG: PASS all %d cases, no fault\n",
           (int)(sizeof(cases) / sizeof(cases[0])));
    return 0;
}
