/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    uvc.library internal definitions.
*/
#ifndef UVC_INTERN_H
#define UVC_INTERN_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/semaphores.h>

#include <vhi/vhi.h>

/* One open VHI session. Real UVC capture state (Poseidon uvc.class binding,
 * negotiated format, streaming endpoint) hangs off uh_UvcBackend once wired;
 * until then the frame source is the built-in test pattern. */
struct UvcHandle
{
    ULONG            uh_ColorMode;   /* VHI_MODE_COLOR / VHI_MODE_GRAYSCALE */
    ULONG            uh_Format;      /* VHI_FORMAT_#? */
    ULONG            uh_Options;     /* VHI_OPT_#? bitmask */
    ULONG            uh_Width;
    ULONG            uh_Height;
    ULONG            uh_Input;       /* selected VHI input: 0=test pattern, N=camera */
    APTR             uh_UvcBackend;  /* NULL until uvc.class capture is wired */
    ULONG            uh_FrameSeq;    /* advances the test pattern per grab */
};

struct UvcBase
{
    struct Library          ub_Lib;
    struct Library         *ub_UtilityBase;
    struct SignalSemaphore  ub_Lock;
    ULONG                   ub_OpenCount;
};

#endif /* UVC_INTERN_H */
