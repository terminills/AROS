/*
    (C) 1995-96 AROS - The Amiga Replacement OS
    $Id$

    Desc:
    Lang: english
*/
#include <intuition/classes.h>
#include <proto/alib.h>
#include "intuition_intern.h"

/*****************************************************************************

    NAME */
#include <intuition/classusr.h>
#include <proto/intuition.h>
#include <proto/boopsi.h>

	AROS_LH1(void, DisposeObject,

/*  SYNOPSIS */
	AROS_LHA(APTR, object, A0),

/*  LOCATION */
	struct IntuitionBase *, IntuitionBase, 107, Intuition)

/*  FUNCTION
	Deletes a BOOPSI object. All memory associated with the object
	is freed. The object must have been created with NewObject().
	Some object contain other object which might be freed as well
	when this function is used on the "parent" while others might
	also contain children but won't free them. Read the documentation
	of the class carefully to find out how it behaves.

    INPUTS
	object - The result of a call to NewObject() or a similar function,
	         may be NULL.

    RESULT
	None.

    NOTES
	This functions sends OM_DISPOSE to the oejct.

    EXAMPLE

    BUGS

    SEE ALSO
	NewObject(), SetAttrs((), GetAttr(), MakeClass()
	"Basic Object-Oriented Programming System for Intuition" and
	"boopsi Class Reference" Dokument.

    INTERNALS

    HISTORY
	29-10-95    digulla automatically created from
			    intuition_lib.fd and clib/intuition_protos.h

*****************************************************************************/
{
    AROS_LIBFUNC_INIT
    AROS_LIBBASE_EXT_DECL(struct IntuitionBase *,IntuitionBase)

    /* pass to boopsi.library */
    DisposeObject(object);

#if 0
    ULONG MethodID = OM_DISPOSE;

    if (!object)
        return;

    OCLASS (object)->cl_ObjectCount --;

    DoMethodA (object, (Msg)&MethodID);
#endif

    AROS_LIBFUNC_EXIT
} /* DisposeObject */
