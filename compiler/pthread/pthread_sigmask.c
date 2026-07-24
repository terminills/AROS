/*
  Copyright (C) 2014 Szilard Biro

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "pthread_intern.h"

#include <errno.h>

/*****************************************************************************

    NAME */

#include <signal.h>

	int pthread_sigmask (

/*  SYNOPSIS */
	int  how,
	const  sigset_t *set,
	sigset_t *oldset)

/*  FUNCTION
        Examine or change the signal mask of the calling thread.

    INPUTS

    RESULT
        0 on success, or an error number on failure.

    NOTES
        AROS signal masks are maintained by posixc for the calling task.

    EXAMPLE

    BUGS

    SEE ALSO
        sigprocmask()

    INTERNALS

******************************************************************************/
{
    if (sigprocmask(how, set, oldset) == 0)
        return 0;

    return errno;
} /* pthread_sigmask */
