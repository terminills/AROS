/*
**	$VER: libheader.c 37.15 (14.8.97)
**
**	This file must be compiled and must be passed as the first
**	object to link to the linker.
**
**	(C) Copyright 1996-97 Andreas R. Kleinert
**	All Rights Reserved.
*/

#define __USE_SYSBASE	     /* perhaps only recognized by SAS/C */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <exec/initializers.h>

#ifdef __MAXON__
#include <pragma/exec_lib.h>
#include <linkerfunc.h>
#else
#include <proto/exec.h>    /* all other compilers */
#endif
#include <libcore/compiler.h>
#include <libcore/base.h>

#ifndef LC_LIBHEADERTYPEPTR
#   define LC_LIBHEADERTYPEPTR	       struct LibHeader *
#endif
#ifndef LC_LIB_FIELD
#   define LC_LIB_FIELD(libBase)       (libBase)->lh_LibNode
#endif
#ifndef LC_SYSBASE_FIELD
#   define LC_SYSBASE_FIELD(libBase)   (libBase)->lh_SysBase
#endif
#ifndef LC_SEGLIST_FIELD
#   define LC_SEGLIST_FIELD(libBase)   (libBase)->lh_SegList
#endif
#ifndef LC_RESIDENTNAME
#   define LC_RESIDENTNAME     LC_BUILDNAME(ROMTag)
#endif
#ifndef LC_RESIDENTPRI
#   define LC_RESIDENTPRI      0
#endif
#ifndef LC_RESIDENTFLAGS
#   define LC_RESIDENTFLAGS    RTF_AUTOINIT
#endif
#ifndef LC_LIBBASESIZE
#   define LC_LIBBASESIZE  sizeof (struct LibHeader)
#endif
/* If the file with the #defines for this library is not "libdefs.h",
    then you can redefine it. */
#ifndef LC_LIBDEFS_FILE
#   define LC_LIBDEFS_FILE "libdefs.h"
#endif

/* Include the file with the #defines for this library */
#include LC_LIBDEFS_FILE

/* -----------------------------------------------------------------------
    entry:

    If someone tries to start a library as an executable, it must return
    (LONG) -1 as result. That's what we are doing here.
----------------------------------------------------------------------- */

LONG ASM LC_BUILDNAME(entry) (void)
{
    return(-1);
}

/* Predeclarations */
extern const int LIBEND;	  /* The end of the library */
extern const APTR LIBFUNCTABLE[]; /* The function table */
static const struct InitTable LC_BUILDNAME(InitTab);
static const struct DataTable LC_BUILDNAME(DataTab);

extern const char ALIGNED LC_BUILDNAME(LibName)   [];
extern const char ALIGNED LC_BUILDNAME(LibID)     [];
extern const char ALIGNED LC_BUILDNAME(Copyright) [];

AROS_LD2 (LC_LIBHEADERTYPEPTR, LC_BUILDNAME(InitLib),
    AROS_LDA(LC_LIBHEADERTYPEPTR, lh,      D0),
    AROS_LDA(BPTR,                segList, A0),
    struct ExecBase *, sysBase, 0, LibHeader
);
AROS_LD1 (BPTR, LC_BUILDNAME(ExpungeLib),
    AROS_LDA(LC_LIBHEADERTYPEPTR, lh,      D0),
    struct ExecBase *, sysBase, 0, LibHeader
);

/* -------------------------------------------------------------------------
    ROMTag and Library inilitalization structure:

    Below you find the ROMTag, which is the most important "magic" part of
    a library (as for any other resident module). You should not need to
    modify any of the structures directly, since all the data is referenced
    from constants from somewhere else.

    You may place the ROMTag directly after the LibStart (-> StartUp.c)
    function as well.

    EndResident can be placed somewhere else - but it must follow the
    ROMTag and it must not be placed in a different SECTION.
------------------------------------------------------------------------- */
struct Resident const ALIGNED LC_RESIDENTNAME =
{
    RTC_MATCHWORD,			    /* This is a romtag */
    (struct Resident *)&LC_RESIDENTNAME,
    (APTR) &LIBEND,
    LC_RESIDENTFLAGS,
    VERSION_NUMBER,
    NT_TYPE,
    LC_RESIDENTPRI,
    (char *) &LC_BUILDNAME(LibName)[0],
    (char *) &LC_BUILDNAME(LibID)[0],
    (APTR) &LC_BUILDNAME(InitTab)
};

static struct InitTable 		      /* do not change */
{
    ULONG		    LibBaseSize;
    const APTR		   *FunctionTable;
    const struct DataTable *DataTable;
    APTR		    InitLibTable;
}
const LC_BUILDNAME(InitTab) =
{
    LC_LIBBASESIZE,
    &LIBFUNCTABLE[0],
    &LC_BUILDNAME(DataTab),
    (APTR) AROS_SLIB_ENTRY(LC_BUILDNAME(InitLib), LibHeader)
};

static struct DataTable 		   /* do not change */
{
    UWORD ln_Type_Init;      UWORD ln_Type_Offset;	UWORD ln_Type_Content;
    UBYTE ln_Name_Init;      UBYTE ln_Name_Offset;	ULONG ln_Name_Content;
    UWORD lib_Flags_Init;    UWORD lib_Flags_Offset;	UWORD lib_Flags_Content;
    UWORD lib_Version_Init;  UWORD lib_Version_Offset;	UWORD lib_Version_Content;
    UWORD lib_Revision_Init; UWORD lib_Revision_Offset; UWORD lib_Revision_Content;
    UBYTE lib_IdString_Init; UBYTE lib_IdString_Offset; ULONG lib_IdString_Content;
    ULONG ENDMARK;
}
const LC_BUILDNAME(DataTab) =
{
    INITBYTE(OFFSET(Node,         ln_Type),      NT_TYPE),
    0x80, (UBYTE) OFFSET(Node,    ln_Name),      (ULONG) &LC_BUILDNAME(LibName[0]),
    INITBYTE(OFFSET(Library,      lib_Flags),    LIBF_SUMUSED|LIBF_CHANGED),
    INITWORD(OFFSET(Library,      lib_Version),  VERSION_NUMBER),
    INITWORD(OFFSET(Library,      lib_Revision), REVISION_NUMBER),
    0x80, (UBYTE) OFFSET(Library, lib_IdString), (ULONG) &LC_BUILDNAME(LibID)[0],
    (ULONG) 0
};

const char ALIGNED LC_BUILDNAME(LibName)   [] = NAME_STRING;
const char ALIGNED LC_BUILDNAME(LibID)     [] = VERSION_STRING;
const char ALIGNED LC_BUILDNAME(Copyright) [] = COPYRIGHT_STRING;

/* Use supplied functions to initialize the non-standard parts of the
   library */
#ifndef LC_NO_INITLIB
ULONG SAVEDS STDARGS L_InitLib	(LC_LIBHEADERTYPEPTR lh);
#else
#   undef L_InitLib
#   define L_InitLib(x)         1
#endif
#ifndef LC_NO_OPENLIB
ULONG SAVEDS STDARGS L_OpenLib	(LC_LIBHEADERTYPEPTR lh);
#else
#   undef L_OpenLib
#   define L_OpenLib(x)         1
#endif
#ifndef LC_NO_CLOSELIB
void  SAVEDS STDARGS L_CloseLib (LC_LIBHEADERTYPEPTR lh);
#else
#   undef L_CloseLib
#   define L_CloseLib(x)        /* eps */
#endif
#ifndef LC_NO_EXPUNGELIB
void  SAVEDS STDARGS L_ExpungeLib (LC_LIBHEADERTYPEPTR lh);
#else
#   undef L_ExpungeLib
#   define L_ExpungeLib(x)        /* eps */
#endif

#ifndef SysBase
#   define SysBase	(LC_SYSBASE_FIELD(lh))
#   define __LC_OWN_SYSBASE
#endif

/* -----------------------------------------------------------------------
    InitLib:

    This one is single-threaded by the Ramlib process. Theoretically you
    can do, what you like here, since you have full exclusive control over
    all the library code and data. But due to some bugs in Ramlib V37-40,
    you can easily cause a deadlock when opening certain libraries here
    (which open other libraries, that open other libraries, that...)
----------------------------------------------------------------------- */

AROS_LH2 (LC_LIBHEADERTYPEPTR, LC_BUILDNAME(InitLib),
    AROS_LHA(LC_LIBHEADERTYPEPTR, lh,      D0),
    AROS_LHA(BPTR,                segList, A0),
    struct ExecBase *, sysBase, 0, LibHeader
)
{
    LC_SYSBASE_FIELD(lh) = sysBase;
    LC_SEGLIST_FIELD(lh) = segList;

#ifndef LC_NO_INITLIB
    if (L_InitLib (lh))
	return (lh);

    L_ExpungeLib (lh);

    {
	ULONG negsize, possize, fullsize;
	UBYTE *negptr = (UBYTE *) lh;

	negsize  = LC_LIB_FIELD(lh).lib_NegSize;
	possize  = LC_LIB_FIELD(lh).lib_PosSize;
	fullsize = negsize + possize;
	negptr	-= negsize;

	FreeMem (negptr, fullsize);
    }

    return (NULL);
#else
    return (lh);
#endif /* LC_NO_INITLIB */
}

/* -----------------------------------------------------------------------
    OpenLib:

    This one is enclosed within a Forbid/Permit pair by Exec V37-40. Since
    a Wait() call would break this Forbid/Permit(), you are not allowed to
    start any operations that may cause a Wait() during their processing.
    It's possible, that future OS versions won't turn the multi-tasking
    off, but instead use semaphore protection for this function.

    Currently you only can bypass this restriction by supplying your own
    semaphore mechanism.
----------------------------------------------------------------------- */
AROS_LH1 (LC_LIBHEADERTYPEPTR, LC_BUILDNAME(OpenLib),
    AROS_LHA (ULONG, version, D0),
    LC_LIBHEADERTYPEPTR, lh, 1, LibHeader
)
{
#ifdef __MAXON__
    GetBaseReg();
    InitModules();
#endif

    if (L_OpenLib (lh))
    {
#ifndef NOEXPUNGE
	LC_LIB_FIELD(lh).lib_OpenCnt++;
#else
	LC_LIB_FIELD(lh).lib_OpenCnt = 1;
#endif /* NOEXPUNGE */

	LC_LIB_FIELD(lh).lib_Flags &= ~LIBF_DELEXP;

	return(lh);
    }

    return NULL;
}


/* -----------------------------------------------------------------------
    CloseLib:

    This one is enclosed within a Forbid/Permit pair by Exec V37-40. Since
    a Wait() call would break this Forbid/Permit(), you are not allowed to
    start any operations that may cause a Wait() during their processing.
    It's possible, that future OS versions won't turn the multi-tasking
    off, but instead use semaphore protection for this function.

    Currently you only can bypass this restriction by supplying your own
    semaphore mechanism.
----------------------------------------------------------------------- */
AROS_LH0 (BPTR, LC_BUILDNAME(CloseLib),
    LC_LIBHEADERTYPEPTR, lh, 2, LibHeader
)
{
#ifndef NOEXPUNGE
    LC_LIB_FIELD(lh).lib_OpenCnt--;

    L_CloseLib (lh);

    if(!LC_LIB_FIELD(lh).lib_OpenCnt)
    {
	if(LC_LIB_FIELD(lh).lib_Flags & LIBF_DELEXP)
	{
	    return AROS_LC1(BPTR, LC_BUILDNAME(ExpungeLib),
		    AROS_LCA(LC_LIBHEADERTYPEPTR, lh, D0),
		    struct ExecBase *, SysBase, 3, LibHeader
	    );
	}
    }
#endif /* NOEXPUNGE */

    return (NULL);
}

/* -----------------------------------------------------------------------
    ExpungeLib:

    This one is enclosed within a Forbid/Permit pair by Exec V37-40. Since
    a Wait() call would break this Forbid/Permit(), you are not allowed to
    start any operations that may cause a Wait() during their processing.
    It's possible, that future OS versions won't turn the multi-tasking
    off, but instead use semaphore protection for this function.

    Currently you only can bypass this restriction by supplying your own
    semaphore mechanism but since expunging can't be done twice, one should
    avoid it here.
----------------------------------------------------------------------- */
AROS_LH1 (BPTR, LC_BUILDNAME(ExpungeLib),
    AROS_LHA(LC_LIBHEADERTYPEPTR, lh, D0),
    struct ExecBase *, sysBase, 3, LibHeader
)
{
#ifndef NOEXPUNGE
    BPTR seglist;

    if(!LC_LIB_FIELD(lh).lib_OpenCnt)
    {
	ULONG negsize, possize, fullsize;
	UBYTE *negptr = (UBYTE *)lh;

	seglist = LC_SEGLIST_FIELD(lh);

	Remove((struct Node *)lh);

	L_ExpungeLib (lh);

	negsize  = LC_LIB_FIELD(lh).lib_NegSize;
	possize  = LC_LIB_FIELD(lh).lib_PosSize;
	fullsize = negsize + possize;
	negptr	-= negsize;

	FreeMem(negptr, fullsize);

#ifdef __MAXON__
	CleanupModules();
#endif

	return(seglist);
    }

    LC_LIB_FIELD(lh).lib_Flags |= LIBF_DELEXP;
#endif /* NOEXPUNGE */

    return (NULL);
}

/* -----------------------------------------------------------------------
    ExtFunct:

    This is a function which is reserved for later extension.
----------------------------------------------------------------------- */
AROS_LH0 (LC_LIBHEADERTYPEPTR, LC_BUILDNAME(ExtFuncLib),
    LC_LIBHEADERTYPEPTR, lh, 4, LibHeader
)
{
    return(NULL);
}

#ifdef __LC_OWN_SYSBASE
#   undef SysBase
#endif

#ifdef __SASC

/*
    This is only for SAS/C - its intention is to turn off internal CTRL-C
    handling for standard C function and to avoid calls to exit() et al.
*/

#ifdef ARK_OLD_STDIO_FIX

ULONG XCEXIT	   = NULL; /* these symbols may be referenced by    */
ULONG _XCEXIT	   = NULL; /* some functions of sc.lib, but should  */
ULONG ONBREAK	   = NULL; /* never be used inside a shared library */
ULONG _ONBREAK	   = NULL;
ULONG base	   = NULL;
ULONG _base	   = NULL;
ULONG ProgramName  = NULL;
ULONG _ProgramName = NULL;
ULONG StackPtr	   = NULL;
ULONG _StackPtr    = NULL;
ULONG oserr	   = NULL;
ULONG _oserr	   = NULL;
ULONG OSERR	   = NULL;
ULONG _OSERR	   = NULL;

#endif /* ARK_OLD_STDIO_FIX */

void __regargs __chkabort(void) { }  /* a shared library cannot be    */
void __regargs _CXBRK(void)     { }  /* CTRL-C aborted when doing I/O */

#endif /* __SASC */

