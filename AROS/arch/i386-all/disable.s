/*
    (C) 1995-96 AROS - The Amiga Replacement OS
    $Id$

    Desc: Exec function Disable
    Lang: english
*/

/******************************************************************************

    NAME
	AROS_LH0(void, Disable,

    LOCATION
	struct ExecBase *, SysBase, 20, Exec)

    FUNCTION

    INPUTS

    RESULT

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS

    HISTORY

******************************************************************************/

	#include "machine.i"

	.text
	.balign 16
	.globl	AROS_SLIB_ENTRY(Disable,Exec)
	.type	AROS_SLIB_ENTRY(Disable,Exec),@function

AROS_SLIB_ENTRY(Disable,Exec):
	call  _disable
	pushl %eax
	movl  8(%esp),%eax
	incb  IDNestCnt(%eax)
	popl  %eax
	ret

	.globl	_disable
	.type	_disable,@function
_disable:
	pushl %eax
	pushl %ecx
	pushl %edx

	pushl $-1
	pushl $0
	leal  4(%esp),%eax
	pushl %eax
	pushl $0
	call  AROS_CSYMNAME(sigprocmask)
	addl  $16,%esp

	popl  %edx
	popl  %ecx
	popl  %eax

	ret
