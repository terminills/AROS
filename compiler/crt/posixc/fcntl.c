/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.
*/

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/posixc.h>
#include <aros/debug.h>
#include <dos/record.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <libraries/fd.h>
#include <aros/debug.h>
#include "__fdesc.h"

static int __fcntl_record_lock(fdesc *desc, int cmd, struct flock *lock)
{
    ULONG mode;
    ULONG timeout = 0;
    ULONG offset;
    ULONG length;
    LONG ioerr;
    static int unsupported_reported;

    if (lock == NULL)
    {
        errno = EFAULT;
        return -1;
    }

    if (lock->l_whence != SEEK_SET || lock->l_start < 0 || lock->l_len < 0 ||
        (uint64_t)lock->l_start > UINT32_MAX ||
        (uint64_t)lock->l_len > UINT32_MAX)
    {
        errno = EINVAL;
        return -1;
    }

    offset = (ULONG)lock->l_start;
    length = lock->l_len == 0 ? UINT32_MAX - offset : (ULONG)lock->l_len;

    if (lock->l_type == F_UNLCK)
    {
        if (UnLockRecord(desc->fcb->handle, offset, length))
            return 0;
    }
    else
    {
        if (lock->l_type == F_RDLCK)
            mode = cmd == F_SETLKW ? REC_SHARED : REC_SHARED_IMMED;
        else if (lock->l_type == F_WRLCK)
            mode = cmd == F_SETLKW ? REC_EXCLUSIVE : REC_EXCLUSIVE_IMMED;
        else
        {
            errno = EINVAL;
            return -1;
        }

        if (cmd == F_SETLKW)
            timeout = UINT32_MAX;

        if (LockRecord(desc->fcb->handle, offset, length, mode, timeout))
            return 0;
    }

    ioerr = IoErr();
    if (ioerr == ERROR_ACTION_NOT_KNOWN || ioerr == ERROR_NOT_IMPLEMENTED)
    {
        /* POSIX locks are process-owned, and SQLite arbitrates threads in its
           own inode table. A handler without ACTION_LOCK_RECORD can therefore
           proceed only when the caller separately enforces one process per
           profile, as Chromium's ProcessSingleton does. */
        if (!unsupported_reported)
        {
            unsupported_reported = TRUE;
            bug("[POSIXC:FD] record locks unsupported by filesystem; "
                "caller must enforce single-process access\n");
        }
        return 0;
    }

    if (ioerr == ERROR_LOCK_COLLISION || ioerr == ERROR_LOCK_TIMEOUT)
        errno = EACCES;
    else
        errno = __stdc_ioerr2errno(ioerr);

    return -1;
}

/*****************************************************************************

    NAME */
#include <fcntl.h>

        int fcntl(

/*  SYNOPSIS */
        int fd,
        int cmd,
        ...)

/*  FUNCTION
        Perform operation specified in cmd on the file descriptor fd.
        Some operations require additional arguments, in this case they
        follow the cmd argument. The following operations are available:

        F_DUPFD (int)  - Duplicate file descriptor fd as the lowest numbered
                         file descriptor greater or equal to the operation
                         argument.

        F_GETFD (void) - Read the file descriptor flags

        F_SETFD (int)  - Set the file descriptor flags to value given in
                         the operation argument

        F_GETFL (void) - Read the file status flags

        F_SETFL (int)  - Set the file status flags to value given in the
                         operation argument.

        F_SETLK (struct flock *)  - Set or release a DOS record lock without
                                    waiting.

        F_SETLKW (struct flock *) - Set or release a DOS record lock, waiting
                                    for conflicting locks.

        File descriptor flags are zero or more ORed constants:

        FD_CLOEXEC - File descriptor will be closed during execve()

        File descriptor flags are not copied during duplication of file
        descriptors.

        File status flags are the flags given as mode parameter to open()
        function call. You can change only a few file status flags in opened
        file descriptor: O_NONBLOCK, O_APPEND and O_ASYNC. Any other file
        status flags passed in F_SETFL argument will be ignored.

        All duplicated file descriptors share the same set of file status
        flags.

    INPUTS
        fd - File descriptor to perform operation on.
        cmd - Operation specifier.
        ... - Operation arguments.

    RESULT
        The return value of the function depends on the performed operation:

        F_DUPFD  - New duplicated file descriptor

        F_GETFD  - File descriptor flags

        F_SETFD  - 0

        F_GETFL  - File status flags

        F_SETFL  - 0 on success, -1 on error. In case of error a global errno
               variable is set.

        F_SETLK, F_SETLKW - 0 on success, -1 on error.

    NOTES

    EXAMPLE

    BUGS
        DOS record locks use 32-bit offsets. F_GETLK is not implemented
        because DOS handlers do not expose the conflicting lock owner.

    SEE ALSO
        open()

    INTERNALS

******************************************************************************/
{
    fdesc *desc = __getfdesc(fd);
    int result = -1;

    if (!desc)
    {
        /* Descriptor owned by another subsystem (e.g. a bsdsocket socket).
           Honour the descriptor- and status-flag commands as best-effort
           no-ops so that callers setting FD_CLOEXEC / O_NONBLOCK during
           socket setup succeed and keep using the descriptor. */
        APTR data;
        const struct fd_hooks *hooks = __getfdhooks(fd, &data);
        if (hooks)
        {
            switch (cmd)
            {
                case F_GETFD: return 0;
                case F_SETFD: return 0;
                case F_GETFL:
                {
                    /* Report the real non-blocking state so callers that read,
                       modify and write back the flags behave correctly. */
                    LONG e2 = 0, nb = 0;
                    if (hooks->fdh_ioctl)
                        nb = hooks->fdh_ioctl(data, fd, FIONBIO, NULL, &e2);
                    return O_RDWR | (nb > 0 ? O_NONBLOCK : 0);
                }
                case F_SETFL:
                {
                    va_list ap;
                    int arg, nb;
                    LONG e2 = 0;
                    va_start(ap, cmd);
                    arg = va_arg(ap, int);
                    va_end(ap);
                    nb = (arg & O_NONBLOCK) ? 1 : 0;
                    if (hooks->fdh_ioctl)
                        hooks->fdh_ioctl(data, fd, FIONBIO, &nb, &e2);
                    return 0;
                }
                case F_DUPFD:
                {
                    va_list ap;
                    int arg;
                    va_start(ap, cmd);
                    arg = va_arg(ap, int);
                    va_end(ap);
                    int newfd = __getfirstfd(arg);
                    int result;
                    if (newfd < 0)
                        return -1;
                    result = dup2(fd, newfd);
                    if (result < 0)
                        __freefdslot(newfd);
                    return result;
                }
                default:
                    errno = EINVAL;
                    return -1;
            }
        }
        errno = EBADF;
        return -1;
    }

    switch (cmd)
    {
        case F_DUPFD:
        {
            va_list ap;
            int     arg;
            
            va_start(ap, cmd);
            arg = va_arg(ap, int);
            va_end(ap);
            
            /*
                FIXME: FD_CLOEXEC must be off on the copy, once this flag
                       is supported (related to F_GETFD and F_SETFD).
            */
            
            int newfd = __getfirstfd(arg);
            if (newfd < 0)
            {
                result = -1;
                break;
            }
            result = dup2(fd, newfd);
            if (result < 0)
                __freefdslot(newfd);
            break;
        }
        case F_GETFD:
            result = desc->fdflags;
            break;
            
        case F_SETFD:
        {
            va_list ap;
            int     arg;

            va_start(ap, cmd);
            arg = va_arg(ap, int);
            va_end(ap);

            desc->fdflags = arg;
            result = 0;
            break;
        }

        case F_GETFL:
            result = desc->fcb->flags &
                (O_ACCMODE|O_NONBLOCK|O_APPEND|O_ASYNC);
            break;

        case F_SETFL:
        {
            va_list ap;
            int     arg;
            int     oldmode = __oflags2amode(desc->fcb->flags & ~(O_NONBLOCK|O_APPEND|O_ASYNC));

            va_start(ap, cmd);
            arg = va_arg(ap, int);
            va_end(ap);

            arg &= (O_NONBLOCK|O_APPEND|O_ASYNC);

            __fcb_lock(desc->fcb);
            if (ChangeMode(CHANGE_FH, desc->fcb->handle, oldmode | __oflags2amode(arg)) == DOSTRUE)
            {
                desc->fcb->flags &= ~(O_NONBLOCK|O_APPEND|O_ASYNC);
                desc->fcb->flags |= arg;
                __fcb_unlock(desc->fcb);
                result = 0;
                break;
            }
            __fcb_unlock(desc->fcb);

            errno = __stdc_ioerr2errno(IoErr());
            break;
        }

        case F_SETLK:
        case F_SETLKW:
        {
            va_list ap;
            struct flock *lock;

            va_start(ap, cmd);
            lock = va_arg(ap, struct flock *);
            va_end(ap);

            /* LockRecord/UnLockRecord talk to the handle's DOS handler;
               serialise against concurrent I/O on the same fcb. */
            __fcb_lock(desc->fcb);
            result = __fcntl_record_lock(desc, cmd, lock);
            __fcb_unlock(desc->fcb);
            break;
        }

        default:
            errno = EINVAL;
            break;
    }

    return result;
}
