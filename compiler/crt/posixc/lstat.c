/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.
*/

#include <aros/debug.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include <errno.h>
#include <string.h>

#include "__posixc_intbase.h"
#include "__stat.h"
#include "__upath.h"

/*****************************************************************************

    NAME */

#include <sys/stat.h>

        int lstat(

/*  SYNOPSIS */
        const char  *path,
        struct stat *sb)

/*  FUNCTION
        Returns information about a file like stat does except that lstat
        does not follow symbolic links. Information is stored in stat
        structure. Consult stat() documentation for detailed description
        of that structure.

    INPUTS
        path - Pathname of the file
        sb - Pointer to stat structure that will be filled by the lstat() call.

    RESULT
        0 on success and -1 on error. If an error occurred, the global
        variable errno is set.

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO
        __posixc_stat(), __posixc_fstat()

    INTERNALS
        Consult stat() documentation for details.

******************************************************************************/
{
    struct PosixCIntBase *PosixCBase =
        (struct PosixCIntBase *)__aros_getbase_PosixCBase();
    int res = 0;
    BPTR lock;

    /* check for empty path before potential conversion from "." to "" */
    if (PosixCBase->doupath && path && *path == '\0')
    {
        errno = ENOENT;
        return -1;
    }

    /* A POSIX trailing slash requires a directory and follows the link.
       Strip it before DOS path handling, then enforce the directory type. */
    if (PosixCBase->doupath && path && *path && path[strlen(path) - 1] == '/'
        && path[strspn(path, "/")] != '\0')
    {
        size_t length = strlen(path);
        char *trimmed;
        while (length > 1 && path[length - 1] == '/')
            length--;
        trimmed = AllocVec(length + 1, MEMF_ANY);
        if (!trimmed)
        {
            errno = ENOMEM;
            return -1;
        }
        memcpy(trimmed, path, length);
        trimmed[length] = 0;
        res = stat(trimmed, sb);
        FreeVec(trimmed);
        if (res == 0 && !S_ISDIR(sb->st_mode))
        {
            errno = ENOTDIR;
            return -1;
        }
        return res;
    }

    path = __path_u2a(path);
    if (path == NULL)
        return -1;

    lock = __posixc_lock_nofollow(path);
    if (!lock)
    {
        if (   IoErr() == ERROR_IS_SOFT_LINK
            || IoErr() == ERROR_OBJECT_IN_USE)
        {
            /* either the file is already locked exclusively
               or it is a soft link, in both cases only way
               to get info about it is to find it in the
               parent directory with the ExNext() function
            */

            SetIoErr(0);
            return __stat_from_path(path, sb);
        }

        errno = __stdc_ioerr2errno(IoErr());
        return -1;
    }
    else
        res = __stat(lock, sb, FALSE);

    UnLock(lock);

    return res;
}
