#ifndef RESOURCES_EMUL_HOST_H
#define RESOURCES_EMUL_HOST_H

/* Unix metadata can fill the DOS64 examination packets without truncation. */
#define EMUL_HAS_EXAMINE64 1

#include <exec/libraries.h>
#include <hidd/unixio.h>
#include <oop/oop.h>

struct LibCInterface;

struct PlatformHandle
{
    ULONG dirpos;       /* Directory search position for Android */
    long  dirpos_first; /* Pointing to first dir entry for use by seekdir */
};

struct Emul_PlatformData
{
    OOP_Object		 *unixio;	  /* UnixIO object	     */
    struct LibCInterface *SysIFace;	  /* Libc interface	     */
    int			 *errnoPtr;	  /* Pointer to host's errno */
    struct Library	 *em_OOPBase;	  /* Library bases	     */
    struct UnixIOBase	 *em_UnixIOBase;
    struct Library 	 *em_UtilityBase;
};

/* Remove this later in the ABIv1 development cycle */
#define OOPBase     (emulbase->pdata.em_OOPBase)
#define UtilityBase (emulbase->pdata.em_UtilityBase)

#endif /* RESOURCES_EMUL_HOST_H */
