/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Reaction speedbar.gadget - Button node management functions
*/
#define DEBUG 0

#include <proto/exec.h>
#include <proto/utility.h>

#include <exec/memory.h>
#include <utility/tagitem.h>
#include <gadgets/speedbar.h>

#include "speedbar_intern.h"

void speedbar_node_set(struct SpeedButtonNode *sbn, struct TagItem *tags)
{
    struct TagItem *tag, *tstate = tags;

    while ((tag = NextTagItem(&tstate)))
    {
        switch (tag->ti_Tag)
        {
            case SBNA_Image:
                sbn->sbn_Image = (Object *)tag->ti_Data;
                break;
            case SBNA_SelImage:
                sbn->sbn_SelImage = (Object *)tag->ti_Data;
                break;
            case SBNA_Help:
                sbn->sbn_Help = (STRPTR)tag->ti_Data;
                break;
            case SBNA_UserData:
                sbn->sbn_UserData = (APTR)tag->ti_Data;
                break;
            case SBNA_Left:
                sbn->sbn_Left = (WORD)tag->ti_Data;
                break;
            case SBNA_Top:
                sbn->sbn_Top = (WORD)tag->ti_Data;
                break;
            case SBNA_Width:
                sbn->sbn_Width = (WORD)tag->ti_Data;
                break;
            case SBNA_Height:
                sbn->sbn_Height = (WORD)tag->ti_Data;
                break;
            case SBNA_Spacing:
                sbn->sbn_Spacing = (WORD)tag->ti_Data;
                break;
            case SBNA_Highlight:
                sbn->sbn_Highlight = (UWORD)tag->ti_Data;
                break;
            case SBNA_MXGroup:
                sbn->sbn_MXGroup = tag->ti_Data;
                if (tag->ti_Data)
                    sbn->sbn_Flags |= SBNF_TOGGLE;
                break;
            case SBNA_Enabled:
                if (tag->ti_Data)
                    sbn->sbn_Flags &= ~SBNF_DISABLED;
                else
                    sbn->sbn_Flags |= SBNF_DISABLED;
                break;
            case SBNA_Disabled:
                if (tag->ti_Data)
                    sbn->sbn_Flags |= SBNF_DISABLED;
                else
                    sbn->sbn_Flags &= ~SBNF_DISABLED;
                break;
            case SBNA_Toggle:
                if (tag->ti_Data)
                    sbn->sbn_Flags |= SBNF_TOGGLE;
                else
                    sbn->sbn_Flags &= ~SBNF_TOGGLE;
                break;
            case SBNA_Selected:
                if (tag->ti_Data)
                    sbn->sbn_Flags |= SBNF_SELECTED;
                else
                    sbn->sbn_Flags &= ~SBNF_SELECTED;
                break;
        }
    }
}

/*****************************************************************************

    NAME */
#include <proto/speedbar.h>

        AROS_LH2(struct Node *, AllocSpeedButtonNodeA,

/*  SYNOPSIS */
        AROS_LHA(UWORD, number, D0),
        AROS_LHA(struct TagItem *, tags, A0),

/*  LOCATION */
        struct Library *, SpeedBarBase, 6, SpeedBar)

/*  FUNCTION
        Allocates a speed button node for use in a SpeedBar gadget.

    INPUTS
        number - Button identifier number.
        tags   - Tag list controlling node attributes.

    RESULT
        Pointer to the allocated node, or NULL on failure.

******************************************************************************/
{
    AROS_LIBFUNC_INIT

    D(bug("[SpeedBar] AllocSpeedButtonNodeA: number=%d\n", number));
    struct SpeedButtonNode *node;

    node = AllocVec(sizeof(struct SpeedButtonNode), MEMF_CLEAR | MEMF_PUBLIC);
    if (!node)
        return NULL;

    node->sbn_Node.ln_Type = NT_USER;
    node->sbn_Number = number;

    node->sbn_Highlight = SBH_RECESS;
    speedbar_node_set(node, tags);

    return &node->sbn_Node;

    AROS_LIBFUNC_EXIT
}

/*****************************************************************************

    NAME */

        AROS_LH1(void, FreeSpeedButtonNode,

/*  SYNOPSIS */
        AROS_LHA(struct Node *, node, A0),

/*  LOCATION */
        struct Library *, SpeedBarBase, 7, SpeedBar)

/*  FUNCTION
        Frees a speed button node allocated by AllocSpeedButtonNodeA().

    INPUTS
        node - Node to free, or NULL (safely ignored).

******************************************************************************/
{
    AROS_LIBFUNC_INIT

    D(bug("[SpeedBar] FreeSpeedButtonNode: node=%p\n", node));
    if (node)
    {
        FreeVec(node);
    }

    AROS_LIBFUNC_EXIT
}

/*****************************************************************************

    NAME */

        AROS_LH2(void, SetSpeedButtonNodeAttrsA,

/*  SYNOPSIS */
        AROS_LHA(struct Node *, node, A0),
        AROS_LHA(struct TagItem *, tags, A1),

/*  LOCATION */
        struct Library *, SpeedBarBase, 8, SpeedBar)

/*  FUNCTION
        Modifies attributes of a speed button node.

    INPUTS
        node - Node to modify.
        tags - Tag list with new attribute values.

******************************************************************************/
{
    AROS_LIBFUNC_INIT

    D(bug("[SpeedBar] SetSpeedButtonNodeAttrsA: node=%p\n", node));
    struct SpeedButtonNode *sbn = (struct SpeedButtonNode *)node;

    if (!node)
        return;

    speedbar_node_set(sbn, tags);

    AROS_LIBFUNC_EXIT
}

/*****************************************************************************

    NAME */

        AROS_LH2(void, GetSpeedButtonNodeAttrsA,

/*  SYNOPSIS */
        AROS_LHA(struct Node *, node, A0),
        AROS_LHA(struct TagItem *, tags, A1),

/*  LOCATION */
        struct Library *, SpeedBarBase, 9, SpeedBar)

/*  FUNCTION
        Retrieves attributes from a speed button node.

    INPUTS
        node - Node to query.
        tags - Tag list with ti_Data pointing to storage for each value.

******************************************************************************/
{
    AROS_LIBFUNC_INIT

    D(bug("[SpeedBar] GetSpeedButtonNodeAttrsA: node=%p\n", node));
    struct SpeedButtonNode *sbn = (struct SpeedButtonNode *)node;
    struct TagItem *tag, *tstate;

    if (!node)
        return;

    tstate = tags;
    while ((tag = NextTagItem(&tstate)))
    {
        switch (tag->ti_Tag)
        {
            case SBNA_Image:
                *(Object **)tag->ti_Data = sbn->sbn_Image;
                break;
            case SBNA_SelImage:
                *(Object **)tag->ti_Data = sbn->sbn_SelImage;
                break;
            case SBNA_Help:
                *(STRPTR *)tag->ti_Data = sbn->sbn_Help;
                break;
            case SBNA_UserData:
                *(APTR *)tag->ti_Data = sbn->sbn_UserData;
                break;
            case SBNA_MXGroup:
                *(ULONG *)tag->ti_Data = sbn->sbn_MXGroup;
                break;
            case SBNA_Enabled:
                *(LONG *)tag->ti_Data = (sbn->sbn_Flags & SBNF_DISABLED) ? FALSE : TRUE;
                break;
            case SBNA_Disabled:
                *(LONG *)tag->ti_Data = (sbn->sbn_Flags & SBNF_DISABLED) ? TRUE : FALSE;
                break;
            case SBNA_Toggle:
                *(LONG *)tag->ti_Data = (sbn->sbn_Flags & SBNF_TOGGLE) ? TRUE : FALSE;
                break;
            case SBNA_Selected:
                *(LONG *)tag->ti_Data = (sbn->sbn_Flags & SBNF_SELECTED) ? TRUE : FALSE;
                break;
        }
    }

    AROS_LIBFUNC_EXIT
}
