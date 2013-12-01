// PenNames.h
// $Date$
// $Revision$

#ifndef	PENNAMES_H_INCLUDED
#define	PENNAMES_H_INCLUDED

//----------------------------------------------------------------------------

#include "Variables.h"

#define	UNKNOWN_PEN	((LONG) ~0)

struct PenNameDef
	{
	ULONG pnd_PenNumber;
	CONST_STRPTR pnd_PenName;
	};

static const struct PenNameDef PenNames[] =
{
	{ PENIDX_HSHINEPEN, 		"PENIDX_HSHINEPEN"	},
	{ PENIDX_HSHADOWPEN,		"PENIDX_HSHADOWPEN"	},
	{ PENIDX_ICONTEXTOUTLINEPEN, 	"PENIDX_ICONTEXTOUTLINEPEN",	},
	{ PENIDX_DRAWERTEXT, 		"PENIDX_DRAWERTEXT"	},
	{ PENIDX_DRAWERTEXTSEL, 	"PENIDX_DRAWERTEXTSEL"	},
	{ PENIDX_DRAWERBG,		"PENIDX_DRAWERBG",	},
	{ PENIDX_FILETEXT, 		"PENIDX_FILETEXT"	},
	{ PENIDX_FILETEXTSEL, 		"PENIDX_FILETEXTSEL"	},
	{ PENIDX_FILEBG, 		"PENIDX_FILEBG"	},
	{ PENIDX_BACKDROPDETAIL, 	"PENIDX_BACKDROPDETAIL"	},
	{ PENIDX_BACKDROPBLOCK, 	"PENIDX_BACKDROPBLOCK"	},
	{ PENIDX_TOOLTIP_TEXT, 		"PENIDX_TOOLTIP_TEXT"	},
	{ PENIDX_TOOLTIP_BG,		"PENIDX_TOOLTIP_BG"	},
	{ PENIDX_DRAGINFOTEXT_TEXT, 	"PENIDX_DRAGINFOTEXT_TEXT"	},
	{ PENIDX_DRAGINFOTEXT_BG,	"PENIDX_DRAGINFOTEXT_BG"	},
	{ PENIDX_STATUSBAR_BG, 		"PENIDX_STATUSBAR_BG"	},
	{ PENIDX_STATUSBAR_TEXT,	"PENIDX_STATUSBAR_TEXT"	},
	{ PENIDX_ICONTEXTPEN,		"PENIDX_ICONTEXTPEN"	},
	{ PENIDX_ICONTEXTPENSEL, 	"PENIDX_ICONTEXTPENSEL"	},
	{ PENIDX_ICONTEXTSHADOWPEN,	"PENIDX_ICONTEXTSHADOWPEN"	},
	{ PENIDX_ICONTEXTPENBGSEL,	"PENIDX_ICONTEXTPENBGSEL"	},
	{ PENIDX_THUMBNAILBACKGROUNDPEN,"PENIDX_THUMBNAILBACKGROUNDPEN"	},
	{ 0,				NULL }
};

#endif	/* PENNAMES_H_INCLUDED */
