/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    uvc.library — a VHI (Video Hardware Interface) driver for USB Video Class
    cameras.

    VHI model (per the cAMIra reference and <vhi/vhi.h>): the driver is a
    library opened by name; a consumer walks OPEN -> GET/SET -> PERFORM
    (digitize) -> CLOSE. Multiple cameras are exposed as VHI *inputs* (the
    "unit" in Kalamatee's "device + unit" model), enumerated per-driver via
    GET VHI_NUMBER_OF_INPUTS / VHI_NAME_OF_INPUT and selected with
    SET VHI_OPTIONS(option=VHI_OPT_INPUT). There is no global device registry —
    an app is told the driver name explicitly and picks an input.

    Inputs exposed here:
      input 0        = built-in TEST PATTERN (always present; deterministic,
                       hardware-free — this is what CI/VHIProbe validates).
      input 1 .. N   = the UVC cameras Poseidon currently has enumerated
                       (each device carrying a VideoControl interface, class
                       VIDEO_CLASSCODE / VIDEO_CTRL_SUBCLASS).

    Frame CAPTURE from a real camera is not yet implemented: selecting a camera
    input and digitising returns VHI_ERR_NO_INPUT_DATA. Enumeration (count +
    names) is live so enumerateDevices()-style discovery works today; streaming
    (descriptor walk -> isoc pipe -> decode, via uvc.class) is the labelled
    follow-on. Clean-room from <vhi/vhi.h> + the cAMIra reference; not derived
    from any GPL source.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <aros/libcall.h>
#include <proto/exec.h>

#include <devices/usb.h>            /* VIDEO_CLASSCODE */
#include <devices/usb_video.h>      /* VIDEO_CTRL_SUBCLASS */
#include <libraries/poseidon.h>
#include <proto/poseidon.h>

#include <vhi/vhi.h>

#include "uvc_intern.h"
#include LC_LIBDEFS_FILE

/* poseidon.library base for the proto/poseidon.h inline stubs (libbase "ps"). */
struct Library *ps = NULL;

/* Default geometry until real UVC descriptors are read from the device. */
#define UVC_DEF_WIDTH   320
#define UVC_DEF_HEIGHT  240

#define TESTPATTERN_NAME  "Test Pattern (built-in)"

/* --------------------------------------------------------------------------
 * Camera enumeration via Poseidon. A "camera input" is any enumerated device
 * carrying a VideoControl interface (VIDEO_CLASSCODE / VIDEO_CTRL_SUBCLASS).
 * Walk under the read-locked PBase. If want_index >= 0, stop at that camera
 * (0-based) and copy its product name into namebuf; return the total count via
 * *count_out. All Poseidon use is serialised by ub_Lock and brackets its own
 * OpenLibrary/CloseLibrary, so the shared "ps" base is never raced.
 * ------------------------------------------------------------------------ */
static void uvc_walk_cameras(struct UvcBase *UvcBase, LONG want_index,
                             ULONG *count_out, STRPTR namebuf, ULONG namelen)
{
    ULONG count = 0;
    APTR pd;

    if (count_out) *count_out = 0;
    if (namebuf && namelen) namebuf[0] = 0;

    ObtainSemaphore(&UvcBase->ub_Lock);
    if ((ps = OpenLibrary("poseidon.library", 4)))
    {
        psdLockReadPBase();
        pd = NULL;
        while ((pd = psdGetNextDevice(pd)))
        {
            /* does this device expose a UVC VideoControl interface? */
            APTR pif = psdFindInterface(pd, NULL,
                                        IFA_Class, VIDEO_CLASSCODE,
                                        IFA_SubClass, VIDEO_CTRL_SUBCLASS,
                                        TAG_END);
            if (pif)
            {
                if (want_index >= 0 && (ULONG)want_index == count &&
                    namebuf && namelen)
                {
                    STRPTR devname = NULL;
                    psdGetAttrs(PGA_DEVICE, pd, DA_ProductName, &devname, TAG_END);
                    if (devname)
                    {
                        ULONG i = 0;
                        while (devname[i] && i < namelen - 1)
                        {
                            namebuf[i] = devname[i];
                            i++;
                        }
                        namebuf[i] = 0;
                    }
                }
                count++;
            }
        }
        psdUnlockPBase();
        CloseLibrary(ps);
        ps = NULL;
    }
    ReleaseSemaphore(&UvcBase->ub_Lock);

    if (count_out) *count_out = count;
}

static ULONG uvc_count_cameras(struct UvcBase *UvcBase)
{
    ULONG n = 0;
    uvc_walk_cameras(UvcBase, -1, &n, NULL, 0);
    return n;
}

/* Return an AllocVec'd copy of the name of VHI input `index`; the consumer
 * frees it (VHI FREEVEC). input 0 = test pattern, 1..N = cameras. */
static STRPTR uvc_input_name(struct UvcBase *UvcBase, ULONG index)
{
    char buf[128];
    STRPTR out;
    ULONG len;

    if (index == 0)
    {
        const char *s = TESTPATTERN_NAME;
        ULONG i = 0;
        while (s[i]) { buf[i] = s[i]; i++; }
        buf[i] = 0;
    }
    else
    {
        ULONG dummy;
        buf[0] = 0;
        uvc_walk_cameras(UvcBase, (LONG)(index - 1), &dummy, buf, sizeof(buf));
        if (!buf[0])
            return NULL;
    }

    for (len = 0; buf[len]; len++) ;
    out = AllocVec(len + 1, MEMF_PUBLIC);
    if (out) CopyMem(buf, out, len + 1);
    return out;
}

/* --------------------------------------------------------------------------
 * Test-pattern frame source (input 0). Deterministic RGB24 colour bars that
 * scroll by frame sequence, so a known sequence is repeatably checksummable.
 * ------------------------------------------------------------------------ */
static APTR uvc_grab_testpattern(struct UvcHandle *uh)
{
    ULONG w = uh->uh_Width, h = uh->uh_Height, x, y;
    ULONG seq = uh->uh_FrameSeq;
    UBYTE *buf = AllocVec(w * h * 3, MEMF_PUBLIC);
    if (!buf)
        return NULL;
    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            UBYTE *p = buf + (y * w + x) * 3;
            ULONG bar = ((x + seq) * 8 / (w ? w : 1)) & 7;
            p[0] = (bar & 1) ? 255 : 0;        /* R */
            p[1] = (bar & 2) ? 255 : 0;        /* G */
            p[2] = (bar & 4) ? 255 : 0;        /* B */
            if (uh->uh_ColorMode == VHI_MODE_GRAYSCALE)
            {
                UBYTE g = (UBYTE)((p[0] * 30 + p[1] * 59 + p[2] * 11) / 100);
                p[0] = p[1] = p[2] = g;
            }
        }
    }
    return buf;
}

/* --------------------------------------------------------------------------
 * The VHI ABI entry point. String GETs return a STRPTR as the RESULT (attr is
 * NULL, or an index for NAME_OF_INPUT) per the cAMIra reference; struct GETs
 * (MAXIMUM_SIZE) fill the caller's attr buffer; scalar GETs return the value.
 * ------------------------------------------------------------------------ */
AROS_LH5(IPTR, vhi_api,
         AROS_LHA(ULONG, method, D2),
         AROS_LHA(ULONG, submethod, D3),
         AROS_LHA(APTR, attr, D4),
         AROS_LHA(ULONG *, errcode, D5),
         AROS_LHA(APTR, vhi_handle, D6),
         struct UvcBase *, UvcBase, 5, Uvc)
{
    AROS_LIBFUNC_INIT

    struct UvcHandle *uh = (struct UvcHandle *)vhi_handle;
    ULONG err = 0;
    IPTR result = 0;

    switch (method)
    {
        case VHI_METHOD_OPEN:
            uh = AllocVec(sizeof(struct UvcHandle), MEMF_PUBLIC | MEMF_CLEAR);
            if (uh)
            {
                uh->uh_ColorMode = VHI_MODE_COLOR;
                uh->uh_Format = VHI_FORMAT_NOVIDEO;
                uh->uh_Width = UVC_DEF_WIDTH;
                uh->uh_Height = UVC_DEF_HEIGHT;
                uh->uh_Input = 0;              /* default: test pattern */
                uh->uh_UvcBackend = NULL;
                result = (IPTR)uh;
            }
            else
                err = VHI_ERR_OUT_OF_MEMORY;
            break;

        case VHI_METHOD_CLOSE:
            if (uh)
                FreeVec(uh);
            break;

        case VHI_METHOD_GET:
            if (!uh) { err = VHI_ERR_INTERNAL_ERROR; break; }
            switch (submethod)
            {
                case VHI_CARD_NAME:
                    result = (IPTR)"AROS USB Video (VHI/UVC)";
                    break;
                case VHI_CARD_MANUFACTURER:
                    result = (IPTR)"AROS";
                    break;
                case VHI_CARD_DRIVERAUTHOR:
                    result = (IPTR)"AROS Development Team";
                    break;
                case VHI_CARD_DRIVERVERSION:
                    result = 1;
                    break;
                case VHI_CARD_DRIVERREVISION:
                    result = 0;
                    break;
                case VHI_NUMBER_OF_INPUTS:
                    /* input 0 (test pattern) + one per enumerated camera */
                    result = 1 + uvc_count_cameras(UvcBase);
                    break;
                case VHI_NAME_OF_INPUT:
                    /* attr = input index; returns an AllocVec'd name to free */
                    result = (IPTR)uvc_input_name(UvcBase, (ULONG)(IPTR)attr);
                    break;
                case VHI_SUPPORTED_MODES:
                    result = VHI_MODE_COLOR | VHI_MODE_GRAYSCALE;
                    break;
                case VHI_SUPPORTED_VIDEOFORMATS:
                    result = VHI_FORMAT_NOVIDEO;
                    break;
                case VHI_SUPPORTED_OPTIONS:
                    result = VHI_OPT_INPUT;
                    break;
                case VHI_COLOR_MODE:
                    result = uh->uh_ColorMode;
                    break;
                case VHI_MAXIMUM_SIZE:
                    if (attr)
                    {
                        struct vhi_size *sz = (struct vhi_size *)attr;
                        sz->max_width = uh->uh_Width;
                        sz->max_height = uh->uh_Height;
                        sz->fixed = FALSE;
                        sz->scalable = FALSE;
                    }
                    break;
                default:
                    err = VHI_ERR_UNKNOWN_METHOD;
                    break;
            }
            break;

        case VHI_METHOD_SET:
            if (!uh) { err = VHI_ERR_INTERNAL_ERROR; break; }
            switch (submethod)
            {
                case VHI_OPTIONS:
                    /* attr = struct vhi_setoptions {option, value} */
                    if (attr)
                    {
                        struct vhi_setoptions *so = (struct vhi_setoptions *)attr;
                        if (so->option == VHI_OPT_INPUT)
                        {
                            ULONG ninputs = 1 + uvc_count_cameras(UvcBase);
                            if (so->value < ninputs)
                                uh->uh_Input = so->value;
                            else
                                err = VHI_ERR_UNKNOWN_OPTION;
                        }
                        else
                            uh->uh_Options = so->value;
                    }
                    break;
                case VHI_COLOR_MODE:  uh->uh_ColorMode = (ULONG)(IPTR)attr; break;
                case VHI_VIDEOFORMAT: uh->uh_Format = (ULONG)(IPTR)attr; break;
                default:              err = VHI_ERR_UNKNOWN_OPTION; break;
            }
            break;

        case VHI_METHOD_PERFORM:
            if (!uh) { err = VHI_ERR_INTERNAL_ERROR; break; }
            switch (submethod)
            {
                case VHI_CHECK_DIGITIZE_SIZE:
                    if (attr)
                    {
                        struct vhi_dimensions *dim = (struct vhi_dimensions *)attr;
                        if (dim->dst_width == 0)  dim->dst_width = uh->uh_Width;
                        if (dim->dst_height == 0) dim->dst_height = uh->uh_Height;
                    }
                    break;
                case VHI_DIGITIZE_PICTURE:
                {
                    struct vhi_image *img;
                    APTR pixels;

                    /* Real camera inputs: streaming not yet implemented. Be
                     * honest — the input exists and is selectable, but there
                     * is no frame source until uvc.class streaming is wired. */
                    if (uh->uh_Input != 0)
                    {
                        err = VHI_ERR_NO_INPUT_DATA;
                        break;
                    }

                    img = AllocVec(sizeof(struct vhi_image),
                                   MEMF_PUBLIC | MEMF_CLEAR);
                    if (!img) { err = VHI_ERR_OUT_OF_MEMORY; break; }
                    pixels = uvc_grab_testpattern(uh);
                    if (!pixels)
                    {
                        FreeVec(img);
                        err = VHI_ERR_ERR_WHILE_DIG;
                        break;
                    }
                    uh->uh_FrameSeq++;
                    img->chunky = pixels;
                    img->width = uh->uh_Width;
                    img->height = uh->uh_Height;
                    img->type = VHI_RGB_24;
                    img->scaled = FALSE;
                    result = (IPTR)img;   /* driver-allocated; consumer frees */
                    break;
                }
                default:
                    err = VHI_ERR_UNKNOWN_METHOD;
                    break;
            }
            break;

        default:
            err = VHI_ERR_UNKNOWN_METHOD;
            break;
    }

    if (errcode)
        *errcode = err;
    return result;

    AROS_LIBFUNC_EXIT
}
