/*
    (C) 1995-96 AROS - The Amiga Replacement OS
    $Id$

    Desc: ANSI C function bzero()
    Lang: english
*/
#include <clib/exec_protos.h>

/*****************************************************************************

    NAME */
#include <string.h>

	void bzero (

/*  SYNOPSIS */
	void * ptr,
	int    len)

/*  FUNCTION
	Write len zero bytes to ptr. If len is zero, does nothing.

    INPUTS
	ptr - The first byte of the area in memory to be cleared.
	len - How many bytes to clear.

    RESULT

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS

    HISTORY
	28-10-96    ldp created

******************************************************************************/
{
    UBYTE * bptr = ptr;

    while (((IPTR)bptr)&(AROS_LONGALIGN-1) && len)
    {
	*bptr ++ = 0;
	len --;
    }

    if (len > sizeof(ULONG))
    {
	ULONG * ulptr = (ULONG *)bptr;

	while (len > sizeof(ULONG))
	{
	    *ulptr ++ = 0UL;
	    len -= sizeof(ULONG);
	}
    }

    while (len --)
	*bptr ++ = 0;

} /* bzero */

