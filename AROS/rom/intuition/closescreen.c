/*
    (C) 1995-96 AROS - The Amiga Replacement OS
    $Id$
    $Log$
    Revision 1.1  1996/09/21 14:11:39  digulla
    Open and close screens


    Desc:
    Lang: english
*/
#include "intuition_intern.h"
#include <clib/exec_protos.h>

#ifndef DEBUG_CloseScreen
#   define DEBUG_CloseScreen 0
#endif
#if DEBUG_CloseScreen
#   undef DEBUG
#   define DEBUG 1
#endif
#include <aros/debug.h>

/*****************************************************************************

    NAME */
	#include <intuition/screens.h>
	#include <clib/intuition_protos.h>

	__AROS_LH1(BOOL, CloseScreen,

/*  SYNOPSIS */
	__AROS_LHA(struct Screen *, screen, A0),

/*  LOCATION */
	struct IntuitionBase *, IntuitionBase, 11, Intuition)

/*  FUNCTION

    INPUTS

    RESULT

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS

    HISTORY
	29-10-95    digulla automatically created from
			    intuition_lib.fd and clib/intuition_protos.h

*****************************************************************************/
{
    __AROS_FUNC_INIT
    __AROS_BASE_EXT_DECL(struct IntuitionBase *,IntuitionBase)
    struct Screen * parent;

    D(bug("CloseScreen (%p)\n", screen));

    /* Trick: Since NextScreen is the first field of the structure,
	we can use the pointer in the IntuitionBase as a screen with
	the structure-size of one pointer */
    parent = (struct Screen *)&(IntuitionBase->FirstScreen);

    /* For all screens... */
    while (parent->NextScreen)
    {
	/* If the screen to close is the next screen... */
	if (parent->NextScreen == screen)
	{
	    /* Unlink it */
	    parent->NextScreen = screen->NextScreen;

	    /* Check ActiveScreen */
	    if (IntuitionBase->ActiveScreen == screen)
	    {
		if (screen->NextScreen)
		    IntuitionBase->ActiveScreen = screen->NextScreen;
		else if (IntuitionBase->FirstScreen)
		    IntuitionBase->ActiveScreen = parent;
		else
		    IntuitionBase->ActiveScreen = NULL;
	    }

	    /* Free the memory */
	    FreeMem (screen, sizeof (struct Screen));

	    ReturnBool("CloseScreen",TRUE);
	}
    }

    ReturnBool("CloseScreen",FALSE);
    __AROS_FUNC_EXIT
} /* CloseScreen */
