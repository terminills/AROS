/*
    Copyright (C) 2004-2026, The AROS Development Team. All rights reserved.
*/

#include <aros/debug.h>

#include <unistd.h>

#include <sys/ioctl.h>

#include <errno.h>

#include <proto/dos.h>
#include <libraries/fd.h>
#include "__fdesc.h"

/* Window Status Request; the answer is CSI 1;1;<rows>;<cols> SPACE r */
#define WSR_TIMEOUT 500000
#define WSR_BUFSIZE 256

static LONG ticks_since(const struct DateStamp *start)
{
    struct DateStamp now;

    DateStamp(&now);
    return ((now.ds_Days - start->ds_Days) * 24 * 60 + now.ds_Minute - start->ds_Minute)
           * 60 * TICKS_PER_SECOND + now.ds_Tick - start->ds_Tick;
}

static BOOL console_size(fdesc *desc, struct winsize *ws)
{
    static const UBYTE request[] = { 0x9b, '0', ' ', 'q' };
    BPTR fh = desc->fcb->handle;
    struct FileHandle *h = BADDR(fh);
    BOOL raw = (desc->fcb->privflags & _FCB_CONSOLERAW) != 0;
    LONG num[4] = { 0 }, n = 0, state = 0, left;
    LONG got = 0, seqstart = 0;
    struct DateStamp start;
    BOOL done = FALSE;
    UBYTE buf[WSR_BUFSIZE];

    if (!raw && !SetMode(fh, 1))
        return FALSE;

    Flush(fh);
    DateStamp(&start);
    if (Write(fh, (APTR)request, sizeof(request)) == sizeof(request))
    {
        /* Everything read that is not the report goes back with ACTION_STACK */
        while (!done && got < WSR_BUFSIZE &&
               (left = WSR_TIMEOUT - ticks_since(&start) * (1000000 / TICKS_PER_SECOND)) > 0 &&
               WaitForChar(fh, left) && Read(fh, &buf[got], 1) == 1)
        {
            UBYTE c = buf[got++];

            if (c == 0x9b)
            {
                seqstart = got - 1;
                state = 2;
                n = 0;
                num[0] = num[1] = num[2] = num[3] = 0;
            }
            else if (c == 0x1b)
            {
                seqstart = got - 1;
                state = 1;
            }
            else if (state == 1)
            {
                state = (c == '[') ? 2 : 0;
                n = 0;
                num[0] = num[1] = num[2] = num[3] = 0;
            }
            else if (state == 2)
            {
                if (c >= '0' && c <= '9' && n < 4)
                    num[n] = num[n] * 10 + c - '0';
                else if (c == ';' && n < 3)
                    n++;
                else if (c == 'r' && n == 3)
                    done = TRUE;
                else if (c != ' ')
                    state = 0;
            }
        }
    }

    if (done)
        got = seqstart;
    if (got > 0)
        DoPkt(h->fh_Type, ACTION_STACK, (SIPTR)h->fh_Arg1, (SIPTR)buf, got, 0, 0);

    if (!raw)
        SetMode(fh, 0);

    if (!done || num[2] <= 0 || num[3] <= 0 || num[2] > 0xffff || num[3] > 0xffff)
        return FALSE;

    ws->ws_row = num[2];
    ws->ws_col = num[3];
    ws->ws_xpixel = 0;
    ws->ws_ypixel = 0;
    return TRUE;
}

/*****************************************************************************

    NAME */

#include <sys/ioctl.h>

        int ioctl(

/*  SYNOPSIS */
        int fd,
        int request,
        ...)

/*  FUNCTION
        Control device. Function to manipulate and fetch special device
        parameters.

    INPUTS
        fd      - file descriptor
        request - ioctl request id, containing request type, input or output
                  type and argument size in bytes. Use macros and defines
                  from <sys/ioctl.h>:

                  TIOCGWINSZ - fill in rows, columns, width and height of
                               console window

        ...     - Other arguments for the specified request

    RESULT
        0 on success. DOS handles return -1 with errno set to EBADF
        (invalid descriptor), EFAULT (no output structure) or ENOTTY
        (not a console, or the console did not answer). Foreign hooks
        define their own return values and errors.

    NOTES
        TIOCGWINSZ sends the console a Window Status Request and reads the
        Window Bounds Report it answers with, switching the handle to raw
        mode for the exchange if it is not already, and back afterwards.
        Input read on the way that is not the report is given back to the
        console with ACTION_STACK. The console does not report pixel
        sizes, so ws_xpixel and ws_ypixel are 0. Foreign descriptor hooks
        retain their own request handling.

    EXAMPLE
        #include <stdio.h>
        #include <unistd.h>
        #include <sys/ioctl.h>

        {
            int ret;
            struct winsize w;
            ret = ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
            if(ret)
            {
                printf("ERROR: %d\n", ret);
            }
            else
            {
                printf ("columns: %4d\n", w.ws_col);
                printf ("lines:   %4d\n", w.ws_row);
                printf ("width:   %4d\n", w.ws_xpixel);
                printf ("height:  %4d\n", w.ws_ypixel);
            }
        }

    BUGS
        A console that does not answer within half a second gives ENOTTY.
        Input read on the way is lost if the handler does not support
        ACTION_STACK.

    SEE ALSO

    INTERNALS

******************************************************************************/
{
    fdesc *desc = __getfdesc(fd);
    struct winsize *ws;
    va_list args;

    /* Descriptor owned by another subsystem (e.g. a bsdsocket socket):
       dispatch the ioctl (FIONBIO/FIONREAD/...) through its hook. */
    if (desc == NULL)
    {
        APTR data;
        const struct fd_hooks *hooks = __getfdhooks(fd, &data);
        if (hooks && hooks->fdh_ioctl)
        {
            va_list ap;
            APTR arg;
            LONG err = 0, r;
            va_start(ap, request);
            arg = va_arg(ap, APTR);
            va_end(ap);
            /* request is an int but the codes are unsigned 32-bit values
               with bit 31 set (FIONBIO = _IOW('f', 126, long) = 0x8008667e),
               so on 64-bit targets a plain (IPTR) cast sign-extends and no
               hook's switch can match it. Zero-extend instead. */
            r = hooks->fdh_ioctl(data, fd, (IPTR)(ULONG)request, arg, &err);
            if (r < 0)
                errno = err;
            return r;
        }
        errno = hooks ? ENOTTY : EBADF;
        return -1;
    }

    if (!desc->fcb)
    {
        errno = EBADF;
        return -1;
    }
    if (request != TIOCGWINSZ)
    {
        errno = ENOTTY;
        return -1;
    }

    va_start(args, request);
    ws = va_arg(args, struct winsize *);
    va_end(args);
    if (!ws)
    {
        errno = EFAULT;
        return -1;
    }

    /* A directory descriptor holds a FileLock, not a FileHandle. */
    if ((desc->fcb->privflags & _FCB_ISDIR) || !IsInteractive(desc->fcb->handle) ||
        !console_size(desc, ws))
    {
        errno = ENOTTY;
        return -1;
    }
    return 0;
}
