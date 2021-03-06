/*
 * @(#) $Header$
 *
 * BGUI library
 * radiobuttonclass.c
 *
 * (C) Copyright 1998 Manuel Lemos.
 * (C) Copyright 1996-1997 Ian J. Einman.
 * (C) Copyright 1993-1996 Jaba Development.
 * (C) Copyright 1993-1996 Jan van den Baard.
 * All Rights Reserved.
 *
 * $Log$
 * Revision 42.2  2004/06/16 20:16:48  verhaegs
 * Use METHODPROTO, METHOD_END and REGFUNCPROTOn where needed.
 *
 * Revision 42.1  2000/05/15 19:27:02  stegerg
 * another hundreds of REG() macro replacements in func headers/protos.
 *
 * Revision 42.0  2000/05/09 22:10:01  mlemos
 * Bumped to revision 42.0 before handing BGUI to AROS team
 *
 * Revision 41.11  2000/05/09 19:54:57  mlemos
 * Merged with the branch Manuel_Lemos_fixes.
 *
 * Revision 41.10  1998/02/25 21:12:56  mlemos
 * Bumping to 41.10
 *
 * Revision 1.1  1998/02/25 17:09:33  mlemos
 * Ian sources
 *
 *
 */

#include "include/classdefs.h"

/// OM_NEW
/*
 * Create a shiny new object.
 */
METHOD(RBClassNew, struct opSet *, ops)
{
   IPTR            rc = 0;
   Object         *rb;
   struct TagItem  ttags[2], *tags;

   /*
    * Get us a radiobutton image.
    */
   if ((rb = BGUI_NewObject(BGUI_VECTOR_IMAGE, VIT_BuiltIn, BUILTIN_RADIOBUTTON, VIT_DriPen, FILLPEN, TAG_DONE)))
   {
      ttags[0].ti_Tag  = BUTTON_SelectedVector;
      ttags[0].ti_Data = (IPTR)rb;
      ttags[1].ti_Tag  = TAG_MORE;
      ttags[1].ti_Data = (IPTR)ops->ops_AttrList;

      tags = DefTagList(BGUI_RADIOBUTTON_GADGET, ttags);

      /*
       * Let the superclass setup an object for us.
       */
      if ((rc = NewSuperObject(cl, obj, tags)))
      {
         /*
          * No recessed rendering.
          */
         DoSetMethodNG((Object *)rc, FRM_EdgesOnly, FALSE, GA_ToggleSelect, TRUE, TAG_DONE);
      }
      else
      {
         AsmDoMethod(rb, OM_DISPOSE);
      };
      FreeTagItems(tags);
   }
   return rc;
}
METHOD_END
///
/// Class Initialization.
/*
 * Class function table.
 */
STATIC DPFUNC ClassFunc[] = {
   { OM_NEW,           RBClassNew, },
   { DF_END },
};

/*
 * Initialize the radiobutton class.
 */
makeproto Class *InitRadioButtonClass(void)
{
   return BGUI_MakeClass(CLASS_SuperClassBGUI, BGUI_BUTTON_GADGET,
                         CLASS_DFTable,        ClassFunc,
                         TAG_DONE);
}
///
