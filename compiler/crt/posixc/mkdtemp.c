/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    POSIX.1-2008 function mkdtemp().
*/
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

/*****************************************************************************

    NAME */
#include <stdlib.h>

        char *mkdtemp(

/*  SYNOPSIS */
        char *template)

/*  FUNCTION
        Create a uniquely-named directory. The last six characters of
        template must be "XXXXXX"; they are replaced with a string that
        makes the directory name unique. The directory is created with
        permissions 0700.

    INPUTS
        template - A modifiable string ending in "XXXXXX".

    RESULT
        A pointer to template (now holding the created directory's name)
        on success, or NULL on error with errno set. On error the contents
        of template are undefined.

    NOTES
        Sibling of mkstemp(); uses mkdir() (which performs the AROS path
        translation), so it needs no direct __upath handling.

    EXAMPLE

    BUGS

    SEE ALSO
        mkstemp(), mkdir()

    INTERNALS

******************************************************************************/
{
    char *c = template + strlen(template);
    char *c_start;
    int ctr = 0;
    int tries;
    /* 62 filename-safe characters, indexed with rand() % 62. */
    static const char letters[] =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

    while (c > template && *--c == 'X')
        ctr++;

    if (ctr < 6)
    {
        errno = EINVAL;
        return NULL;
    }

    c++;
    c_start = c;

    /* Try around 1000 candidate names, then give up (mirrors mkstemp()). */
    for (tries = 0; tries < 1000; tries++)
    {
        c = c_start;
        while (*c)
        {
            *c = letters[rand() % 62];
            c++;
        }

        if (mkdir(template, 0700) == 0)
            return template;

        /* Only a name clash is retryable; any other error is fatal
           (e.g. ENOENT on a missing parent, EACCES). */
        if (errno != EEXIST)
            return NULL;
    }

    errno = EEXIST;
    return NULL;
} /* mkdtemp */
