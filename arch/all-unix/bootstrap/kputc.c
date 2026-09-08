/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.
*/

#include <stdarg.h>
#include <unistd.h>

#include "hostlib.h"

/*
 * Redirect debug output to stderr. This is especially
 * needed on iOS where reading stdout is only possible with
 * remote gdb, which is tied to XCode's own build system.
 * On other unixes this won't hurt either.
 *
 * This runs in AROS task context with interrupts enabled, and the
 * hosted kernel switches tasks from inside a signal handler on this
 * very thread. fputc(stderr) takes glibc's stream lock, which is only
 * recursive once its owner field is set; a task switch landing between
 * the futex acquire and the owner store (or the owner clear and the
 * futex release) left the lock orphaned, and the next task's KPutC
 * blocked the whole emulation in futex_wait forever. stderr is
 * unbuffered, so a plain write() is what fputc did anyway, minus the
 * lock, and is async-signal-safe.
 */
int KPutC(int chr)
{
    unsigned char c = chr;

    if (write(STDERR_FILENO, &c, 1) != 1)
        return -1;

    return c;
}
