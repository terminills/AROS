/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.
 */

#include <aros/debug.h>

#include <dos/stdio.h>

#include <proto/dos.h>
#include <proto/arossupport.h>

#include <string.h>

#include "Shell.h"

#define PIPE_NAME "PIPE "

static BOOL checkPipe(STRPTR pchar, STRPTR mchar, STRPTR in, LONG inlen)
{
    LONG c, n;
    BOOL quoted = FALSE;
    BOOL escaped = FALSE;
    int mcharn = strlen(mchar), pcharn = strlen(pchar);

    for (n = 0; n < inlen; n++)
    {
        c = in[n];
        if (c == '"' && !escaped)
        {
            quoted = !quoted;
        }

        if (c == '*' && !escaped)
            escaped = TRUE;
        else
            escaped = FALSE;

        if (quoted)
            continue;

        if (mcharn > 0 && c == mchar[0] && memcmp(&in[n], mchar, mcharn) == 0)
            return TRUE;

        if (pcharn > 0 && c == pchar[0] && memcmp(&in[n], pchar, pcharn) == 0)
            return TRUE;
    }
    return FALSE;
}

LONG readLine(ShellState *ss, struct CommandLineInterface *cli, Buffer *out, WORD *moreLeft)
{
    BPTR fh = cli->cli_CurrentInput;
    STRPTR buf = out->buf; /* pre-allocated by caller */
    BOOL comment = FALSE;
    BOOL quoted = FALSE;
    BOOL escaped = FALSE;
    LONG c, j, len;
    TEXT pchar[3], mchar[3];

    len = GetVar("_pchar", pchar, sizeof pchar, GVF_LOCAL_ONLY | LV_VAR);
    if (len <= 0)
        pchar[0] = 0;
    pchar[2] = 0;

    len = GetVar("_mchar", mchar, sizeof mchar, GVF_LOCAL_ONLY | LV_VAR);
    if (len <= 0)
        mchar[0] = 0;
    mchar[2] = 0;
    ss->pchar0 = pchar[0];
    ss->mchar0 = mchar[0];

    /*
     * The line is appended to 'out' one character at a time, so it is only
     * bounded by memory: a command line built by another program (openurl's
     * browser launch with a 400-character OAuth URL, a generated Execute
     * script) is routinely longer than the 512 characters the fixed limit
     * used to allow, and failing it with ERROR_LINE_TOO_LONG just dropped
     * the command.  'buf' is re-read after every append because the buffer
     * may move when it grows.
     */
    for (j = 0;;)
    {
        TEXT ch;

        c = FGetC(fh);

        if (c == ENDSTREAMCH)
            break;

        if (c == '"' && !escaped)
            quoted = !quoted;

        if (c == '*' && !escaped)
            escaped = TRUE;
        else
            escaped = FALSE;
 
        if (!quoted && c == ';' && c != mchar[0] ) /* comment line */
        {
            comment = TRUE;
            continue;
        }
        else if (c == '\n') /* end of line */
        {
            comment = FALSE;

            /* '+' continuation */
            /* requires "<space>+", validated with AmigaOS 3.1/2.1 Shell */
            if (j > 1 && buf[j-1] == '+' && buf[j-2] == ' ') {
                buf[j-1] = c;
                continue;
            }
        }
        else if (comment)
            continue;

        ch = c;
        if (bufferAppend(&ch, 1, out, ss) != 0)
            return ERROR_NO_FREE_STORE;
        buf = out->buf;
        j++;

        if (c == '\n') /* end of line */
            break;
    }

    /* bufferAppend() kept out->len == j and NUL-terminated the line */

    if (checkPipe(pchar, mchar, buf, j))
        bufferInsert(PIPE_NAME, strlen(PIPE_NAME), out, ss);

    *moreLeft = (c != ENDSTREAMCH);

    D(
        bug("[Shell] readLine %d%s: %s", j, *moreLeft ?  "" : "'", buf);
    )
    return 0;
}
