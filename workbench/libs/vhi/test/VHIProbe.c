/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    VHIProbe - minimal VHI (Video Hardware Interface) driver verification client.

    Opens a VHI driver .library by name and walks the standard vhi_api()
    contract end to end:

        OPEN  -> GET card identity + maximum size
              -> SET colour mode
              -> PERFORM VHI_DIGITIZE_PICTURE (grab one frame)
              -> checksum the returned frame
              -> CLOSE

    It is hardware-independent: it exercises whatever VHI driver is named on
    the command line (default "uvc.library"). Input zero of uvc.library is
    synthetic; subsequent inputs address supported bound USB cameras.

    ABI is taken solely from <vhi/vhi.h> (compiler/include/vhi/vhi.h) and the
    existing VHI library vector.

    Usage:  VHIProbe [drivername] [dumpfile] [input]
*/

#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <aros/debug.h>   /* bug() -> debug console (0xe9), captured headless */

#include <vhi/vhi.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Mirror the load-bearing result lines to BOTH the shell (printf, for a human
 * at the console) and the debug console (bug, tagged "VHIProbe:", captured in
 * the QEMU debugcon log). The debug mirror is what makes a headless boot's pass
 * observable — no screenshot or shell-redirect needed. */
#define PROBE_LOG(...)  do { printf(__VA_ARGS__); bug("VHIProbe: " __VA_ARGS__); } while (0)

/* Dispatch through the explicitly opened driver at the standard VHI vector. */
struct Library *VHIBase = NULL;
#include <aros/libcall.h>
static IPTR call_vhi(ULONG method, ULONG submethod, APTR attr,
                     ULONG *error, APTR handle)
{
    return AROS_LC5(IPTR, vhi_api,
        AROS_LCA(ULONG, method, D2), AROS_LCA(ULONG, submethod, D3),
        AROS_LCA(APTR, attr, D4), AROS_LCA(ULONG *, error, D5),
        AROS_LCA(APTR, handle, D6), struct Library *, VHIBase, 5, Vhi);
}
#define vhi_api call_vhi

static const char *vhi_errstr(ULONG err)
{
    switch (err)
    {
        case 0:                          return "OK";
        case VHI_ERR_UNKNOWN_METHOD:     return "unknown/unhandled method";
        case VHI_ERR_COULD_NOT_INIT:     return "device initialization failed";
        case VHI_ERR_NO_HARDWARE:        return "device not present";
        case VHI_ERR_NO_INPUT_DATA:      return "no input data";
        case VHI_ERR_ERR_WHILE_DIG:      return "error while digitizing";
        case VHI_ERR_OUT_OF_MEMORY:      return "out of memory";
        case VHI_ERR_INTERNAL_ERROR:     return "internal driver error";
        case VHI_ERR_UNKNOWN_OPTION:     return "unknown option";
        default:                         return "?";
    }
}

static const char *vhi_typestr(ULONG type)
{
    switch (type)
    {
        case VHI_GRAYSCALE_8:  return "GRAYSCALE_8";
        case VHI_RGB_24:       return "RGB_24";
        case VHI_YUV_411:      return "YUV_411";
        case VHI_YUV_422:      return "YUV_422";
        case VHI_YUV_444:      return "YUV_444";
        case VHI_YUV_ACCUPAK:  return "YUV_ACCUPAK";
        case VHI_JPEG:         return "JPEG";
        case VHI_EXIF_JPEG:    return "EXIF_JPEG";
        default:               return "?";
    }
}

/* Return the primary contiguous buffer of a frame and its byte length. For
 * grayscale/RGB/JPEG the data lives in ->chunky; for planar YUV we checksum
 * the Y plane (a stable-enough fingerprint for a round-trip probe). */
static APTR frame_primary(const struct vhi_image *img, ULONG *len_out)
{
    ULONG w = img->width, h = img->height, len = 0;
    APTR buf = NULL;

    switch (img->type)
    {
        case VHI_GRAYSCALE_8:  buf = img->chunky; len = w * h;       break;
        case VHI_RGB_24:       buf = img->chunky; len = w * h * 3;   break;
        case VHI_JPEG:
        case VHI_EXIF_JPEG:    buf = img->chunky; len = w; /* w = file size, h = 0 */ break;
        case VHI_YUV_411:
        case VHI_YUV_422:
        case VHI_YUV_444:      buf = img->y;      len = w * h;       break;
        default:               buf = img->chunky; len = w * h;       break;
    }
    *len_out = len;
    return buf;
}

/* FNV-1a over the frame's primary buffer - a cheap deterministic fingerprint
 * so a known (e.g. HDMI-capture-card) source can be verified repeatably. */
static ULONG frame_checksum(const APTR buf, ULONG len)
{
    const UBYTE *p = (const UBYTE *)buf;
    ULONG hash = 2166136261UL;
    ULONG i;
    for (i = 0; i < len; i++)
    {
        hash ^= p[i];
        hash *= 16777619UL;
    }
    return hash;
}

int main(int argc, char **argv)
{
    const char *drvname  = (argc > 1) ? argv[1] : "uvc.library";
    const char *dumpfile = (argc > 2) ? argv[2] : NULL;
    ULONG err = 0;
    APTR handle = NULL;
    struct vhi_size sz;
    struct vhi_digitize dig;
    struct vhi_image *img;
    APTR buf;
    ULONG len = 0;
    int rc = 20;

    printf("VHIProbe: opening VHI driver \"%s\"...\n", drvname);
    VHIBase = OpenLibrary((CONST_STRPTR)drvname, 0);
    if (!VHIBase)
    {
        PROBE_LOG("  FAILED: could not open \"%s\" "
               "(no such VHI driver / not installed).\n", drvname);
        return 20;
    }

    handle = (APTR)vhi_api(VHI_METHOD_OPEN, 0, NULL, &err, NULL);
    if (!handle || err)
    {
        printf("  OPEN failed: err=%lu (%s), handle=%p\n",
               (unsigned long)err, vhi_errstr(err), handle);
        goto out_close_lib;
    }
    printf("  OPEN ok (handle=%p)\n", handle);

    /* Identity (best-effort). Per the VHI convention (cAMIra reference), string
     * GETs return a STRPTR as the result with attr=NULL — not a caller buffer. */
    {
        STRPTR cardname, author;
        err = 0;
        cardname = (STRPTR)vhi_api(VHI_METHOD_GET, VHI_CARD_NAME, NULL, &err, handle);
        if (!err && cardname)
            PROBE_LOG("  card name: \"%s\"\n", cardname);
        err = 0;
        author = (STRPTR)vhi_api(VHI_METHOD_GET, VHI_CARD_DRIVERAUTHOR, NULL, &err, handle);
        if (!err && author)
            PROBE_LOG("  driver author: \"%s\"\n", author);
    }

    /* Input enumeration — the "unit" model: one driver, cameras as inputs.
     * GET VHI_NUMBER_OF_INPUTS returns the count; GET VHI_NAME_OF_INPUT takes
     * the index in attr and returns an allocated name the caller frees. This is
     * the enumerateDevices()-shaped discovery path. */
    {
        ULONG ninputs, i;
        err = 0;
        ninputs = (ULONG)vhi_api(VHI_METHOD_GET, VHI_NUMBER_OF_INPUTS, NULL, &err, handle);
        PROBE_LOG("  inputs: %lu\n", (unsigned long)ninputs);
        for (i = 0; i < ninputs; i++)
        {
            STRPTR iname;
            err = 0;
            iname = (STRPTR)vhi_api(VHI_METHOD_GET, VHI_NAME_OF_INPUT,
                                    (APTR)(IPTR)i, &err, handle);
            PROBE_LOG("    input %lu: \"%s\"\n",
                      (unsigned long)i, iname ? (char *)iname : "?");
            if (iname)
                FreeVec(iname);
        }
    }

    {
        struct vhi_setoptions option;
        option.option = VHI_OPT_INPUT;
        option.value = argc > 3 ? strtoul(argv[3], NULL, 10) : 0;
        vhi_api(VHI_METHOD_SET, VHI_OPTIONS, &option, &err, handle);
        PROBE_LOG("  selected input: %lu%s\n", (unsigned long)option.value,
                  option.value ? " (camera)" : " (SYNTHETIC)");
        if (err) {
            PROBE_LOG("  input selection failed: %lu (%s)\n",
                      (unsigned long)err, vhi_errstr(err));
            goto out_close_drv;
        }
    }
    memset(&sz, 0, sizeof(sz));
    err = 0;
    vhi_api(VHI_METHOD_GET, VHI_MAXIMUM_SIZE, &sz, &err, handle);
    if (err)
    {
        printf("  GET MAXIMUM_SIZE failed: err=%lu (%s)\n",
               (unsigned long)err, vhi_errstr(err));
        goto out_close_drv;
    }
    printf("  max size: %lux%lu (fixed=%d scalable=%d)\n",
           (unsigned long)sz.max_width, (unsigned long)sz.max_height,
           (int)sz.fixed, (int)sz.scalable);

    /* Colour is the common case for a webcam/HDMI source. */
    err = 0;
    vhi_api(VHI_METHOD_SET, VHI_COLOR_MODE, (APTR)VHI_MODE_COLOR, &err, handle);

    /* Grab one full frame. */
    memset(&dig, 0, sizeof(dig));
    dig.dim.x1 = 0;
    dig.dim.y1 = 0;
    dig.dim.x2 = sz.max_width;
    dig.dim.y2 = sz.max_height;
    dig.dim.dst_width  = sz.max_width;
    dig.dim.dst_height = sz.max_height;
    dig.custom_memhandling = FALSE;

    err = 0;
    img = (struct vhi_image *)vhi_api(VHI_METHOD_PERFORM,
                                      VHI_DIGITIZE_PICTURE, &dig, &err, handle);
    if (!img || err)
    {
        PROBE_LOG("  DIGITIZE failed: err=%lu (%s), img=%p\n",
               (unsigned long)err, vhi_errstr(err), img);
        goto out_close_drv;
    }

    buf = frame_primary(img, &len);
    PROBE_LOG("  frame: type=%s %lux%lu, %lu bytes, buf=%p, scaled=%d\n",
           vhi_typestr(img->type),
           (unsigned long)img->width, (unsigned long)img->height,
           (unsigned long)len, buf, (int)img->scaled);

    if (buf && len)
    {
        PROBE_LOG("  checksum (FNV-1a): 0x%08lx\n",
               (unsigned long)frame_checksum(buf, len));

        if (dumpfile)
        {
            BPTR fh = Open((CONST_STRPTR)dumpfile, MODE_NEWFILE);
            if (fh)
            {
                Write(fh, buf, len);
                Close(fh);
                printf("  dumped %lu bytes to \"%s\"\n",
                       (unsigned long)len, dumpfile);
            }
            else
                printf("  could not open dump file \"%s\"\n", dumpfile);
        }
        rc = 0;
    }
    else
    {
        printf("  frame had no readable buffer\n");
        rc = 10;
    }

    /* The driver owns the frame allocation; standard VHI frees it with
     * FreeVec (custom_memhandling was FALSE). */
    if (img->chunky) FreeVec(img->chunky);
    if (img->y)      FreeVec(img->y);
    if (img->u)      FreeVec(img->u);
    if (img->v)      FreeVec(img->v);
    FreeVec(img);

out_close_drv:
    err = 0;
    vhi_api(VHI_METHOD_CLOSE, 0, NULL, &err, handle);
out_close_lib:
    CloseLibrary(VHIBase);
    printf("VHIProbe: %s (rc=%d)\n", rc == 0 ? "PASS" : "FAIL", rc);
    bug("VHIProbe: RESULT %s rc=%d drv=%s\n",
        rc == 0 ? "PASS" : "FAIL", rc, drvname);
    return rc;
}
