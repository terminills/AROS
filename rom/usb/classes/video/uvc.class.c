/*
 *----------------------------------------------------------------------------
 *                  uvc (USB Video Class) class for poseidon
 *----------------------------------------------------------------------------
 *
 * Poseidon USB Video Class capture with bounded YUY2 frame acquisition.
 * The existing class dispatcher exposes stable camera IDs and RGB24 frames
 * to the VHI driver. Binding release waits for an in-flight bounded grab.
 */

#include "debug.h"

#include "uvc.class.h"
#include <devices/timer.h>
#include <devices/uvc_capture.h>
#include "uvc_payload.h"

#define __LOG_LIBBASE (nh->nh_LogResBase)
#include <proto/log.h>
#include <resources/log.h>

/* Diagnostics go to log.resource (System/LogView, or LogDump to a file) as
 * well as the debug channel: a machine without a serial port has no other
 * way to show why a grab failed. */
#define UVCLOG(type, fmt, ...) do { \
    if (nh->nh_LogRHandle) \
        logAddEntry(((type) | 20), nh->nh_LogRHandle, "", __func__, 0, fmt, ##__VA_ARGS__); \
    bug("uvc.class: " fmt "\n", ##__VA_ARGS__); \
} while (0)

/* /// "Lib Stuff" */
static const STRPTR libname = MOD_NAME_STRING;

static int libInit(LIBBASETYPEPTR nh)
{
    struct NepUVCBase *ret = NULL;

    KPRINTF(10, ("libInit nh: 0x%08lx SysBase: 0x%08lx\n", nh, SysBase));

    NEWLIST(&nh->nh_Cameras);
    InitSemaphore(&nh->nh_Lock);
    nh->nh_NextId = 0;
    nh->nh_UtilityBase = OpenLibrary("utility.library", 39);
    nh->nh_LogResBase = OpenResource("log.resource");
    if (nh->nh_LogResBase)
        nh->nh_LogRHandle = logInitialise(&nh->nh_Library.lib_Node);

#define	UtilityBase	nh->nh_UtilityBase

    if(UtilityBase)
    {
        ret = nh;
    } else {
        KPRINTF(20, ("libInit: OpenLibrary(\"utility.library\", 39) failed!\n"));
    }

    KPRINTF(10, ("libInit: Ok\n"));
    return(ret ? TRUE : FALSE);
}

static int libExpunge(LIBBASETYPEPTR nh)
{
    KPRINTF(10, ("libExpunge nh: 0x%08lx\n", nh));
    if (nh->nh_LogRHandle)
        logFinalise(nh->nh_LogRHandle);
    CloseLibrary((struct Library *) UtilityBase);
    return(TRUE);
}

ADD2INITLIB(libInit, 0)
ADD2EXPUNGELIB(libExpunge, 0)
/* \\\ */

/* Only YUY2 is accepted here: other uncompressed GUIDs have different layouts.
 * Retain supported descriptors for enumeration; selection prefers a fitting
 * device default. The bDefaultFrameInterval belongs to that exact frame. */
/* Isochronous transfers in flight. Each covers one 125 us service interval
 * and the controller drops whatever arrives while its ring is empty, so this
 * is how long the refilling task may be away: 112 x 125 us = 14 ms. The
 * endpoint ring holds 128 entries. */
#define UVC_SLOTS 112
struct uvc_frame_cand {
    UWORD w, h;
    UBYTE format, frame;
    ULONG interval;   /* slowest advertised interval of at least 10 fps */
    ULONG defint;     /* the frame descriptor's own default interval */
    ULONG maxbuf;     /* dwMaxVideoFrameBufferSize from the descriptor */
};

static BOOL uvc_parse_streaming(struct NepClassUVC *nch)
{
    struct NepUVCBase *nh = nch->ncu_ClsBase;
    struct Library *ps = OpenLibrary("poseidon.library", 4);
    APTR dd = NULL;
    ULONG alts_with_iso = 0;
    UBYTE format = 0;
    struct uvc_frame_cand cand[UVC_MAX_FRAMES];
    ULONG ncand = 0;
    LONG chosen = -1;
    const char *reason = "";
    IPTR superspeed = FALSE;
    ULONG altcap;
    static const UBYTE yuy2[16] = {
        'Y','U','Y','2',0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71
    };
    if (!ps) return FALSE;
    /* The VideoControl header (CS_INTERFACE subtype 1) names the streaming
     * interfaces that belong to this control interface (baInterfaceNr). A
     * device with two camera functions, such as an RGB and an IR module,
     * has two of each; take ours, not the first one on the device. */
    while ((dd = psdFindDescriptor(nch->ncu_Device, dd,
        DDA_DescriptorType, UDT_CS_INTERFACE,
        DDA_Interface, nch->ncu_Interface, TAG_END))) {
        UBYTE *p = NULL;
        IPTR len = 0;
        ULONG i;
        psdGetAttrs(PGA_DESCRIPTOR, dd, DDA_DescriptorData, &p,
                    DDA_DescriptorLength, &len, TAG_END);
        if (!p || len < 13 || p[0] > len || p[2] != 0x01) continue;
        for (i = 0; i < p[11] && 12 + i < p[0] && !nch->ncu_VSInterface; i++)
            nch->ncu_VSInterface = psdFindInterface(nch->ncu_Device, NULL,
                IFA_InterfaceNum, p[12 + i],
                IFA_Class, VIDEO_CLASSCODE, IFA_SubClass, VIDEO_STREAM_SUBCLASS,
                TAG_END);
        break;
    }
    dd = NULL;
    if (!nch->ncu_VSInterface)
        nch->ncu_VSInterface = psdFindInterface(nch->ncu_Device, NULL,
            IFA_Class, VIDEO_CLASSCODE, IFA_SubClass, VIDEO_STREAM_SUBCLASS,
            TAG_END);
    if (!nch->ncu_VSInterface) goto done;
    while ((dd = psdFindDescriptor(nch->ncu_Device, dd,
        DDA_DescriptorType, UDT_CS_INTERFACE,
        DDA_Interface, nch->ncu_VSInterface, TAG_END))) {
        UBYTE *p = NULL;
        IPTR len = 0;
        psdGetAttrs(PGA_DESCRIPTOR, dd, DDA_DescriptorData, &p,
                    DDA_DescriptorLength, &len, TAG_END);
        if (!p || len < 3 || p[0] > len || p[0] < 3) continue;
        if (p[2] == UDST_VIDEO_STREAM_FORMAT_UNCOMPRESSED) {
            if (p[0] >= 27) {
                char guid[48];
                IPTR vsnum = 0;
                ULONG k;
                /* Raw descriptor byte order avoids guessing GUID aliases or
                 * treating a monochrome stream as packed YUY2. */
                for (k = 0; k < 16; k++)
                    snprintf(guid + 3*k, sizeof(guid) - 3*k,
                             k == 15 ? "%02lx" : "%02lx:", (unsigned long)p[5+k]);
                psdGetAttrs(PGA_INTERFACE, nch->ncu_VSInterface,
                            IFA_InterfaceNum, &vsnum, TAG_END);
                UVCLOG(LOGF_Flag_Type_Information,
                       "'%s': VC %lu VS %lu uncompressed format %lu GUID bytes %s bpp %lu frames %lu default frame %lu",
                       nch->ncu_Name, (IPTR)nch->ncu_IfNum, vsnum,
                       (IPTR)p[3], guid, (IPTR)p[21], (IPTR)p[4], (IPTR)p[22]);
            }
            format = (p[0] >= 27 && !memcmp(p+5, yuy2, 16) && p[21] == 16)
                ? p[3] : 0;
        } else if (p[2] == UDST_VIDEO_STREAM_FORMAT_MJPEG) {
            format = 0;
        } else if (p[2] == UDST_VIDEO_STREAM_FRAME_UNCOMPRESSED &&
                   format && p[0] >= 26) {
            ULONG w = p[5] | ((ULONG)p[6] << 8);
            ULONG h = p[7] | ((ULONG)p[8] << 8);
            ULONG interval = p[21] | ((ULONG)p[22] << 8) |
                             ((ULONG)p[23] << 16) | ((ULONG)p[24] << 24);
            /* Start with a conservative advertised rate (at least 10 fps).
             * Discrete intervals follow the fixed 26-byte frame descriptor. */
            if (p[25] && p[0] >= 26 + 4 * (ULONG)p[25]) {
                ULONG j;
                for (j = 0; j < p[25]; ++j) {
                    UBYTE *q = p + 26 + 4*j;
                    ULONG candidate = q[0] | ((ULONG)q[1]<<8) |
                        ((ULONG)q[2]<<16) | ((ULONG)q[3]<<24);
                    if (candidate > interval && candidate <= 1000000)
                        interval = candidate;
                }
            }
            if (w && h && !(w & 1) && w <= 1920 && h <= 1080 && interval &&
                ncand < UVC_MAX_FRAMES) {
                cand[ncand].w = w;
                cand[ncand].h = h;
                cand[ncand].format = format;
                cand[ncand].frame = p[3];
                cand[ncand].interval = interval;
                cand[ncand].defint = p[21] | ((ULONG)p[22] << 8) |
                                     ((ULONG)p[23] << 16) | ((ULONG)p[24] << 24);
                cand[ncand].maxbuf = p[17] | ((ULONG)p[18] << 8) |
                                     ((ULONG)p[19] << 16) | ((ULONG)p[20] << 24);
                {
                    struct UVCModeInfo *mode = &nch->ncu_Modes[ncand];
                    mode->width = w; mode->height = h;
                    mode->format_index = format; mode->frame_index = p[3];
                    mode->pixel_format = UVC_MODE_YUY2;
                    mode->default_interval = cand[ncand].defint;
                    mode->capture_interval = interval;
                    mode->max_frame_bytes = cand[ncand].maxbuf;
                }
                nch->ncu_ModeCount = ++ncand;
            }
        }
    }
    /* The stream endpoint: a bulk IN on the interface itself, or the
     * isochronous IN alternate with the most bandwidth the bus allows
     * (3 x 1024 per microframe at high speed, 48 x 1024 at SuperSpeed). */
    psdGetAttrs(PGA_DEVICE, nch->ncu_Device, DA_IsSuperspeed, &superspeed, TAG_END);
    altcap = superspeed ? 49152 : 3072;
    {
        struct List *alts = NULL;
        struct Node *node;
        nch->ncu_StreamEP = psdFindEndpoint(nch->ncu_VSInterface, NULL,
            EA_IsIn, TRUE, EA_TransferType, USEAF_BULK, TAG_END);
        nch->ncu_IsBulk = nch->ncu_StreamEP != NULL;
        if (!nch->ncu_IsBulk) {
            psdGetAttrs(PGA_INTERFACE, nch->ncu_VSInterface,
                        IFA_AlternateIfList, &alts, TAG_END);
            if (alts) for (node=alts->lh_Head; node->ln_Succ; node=node->ln_Succ) {
                APTR ep = psdFindEndpoint((APTR)node, NULL, EA_IsIn, TRUE,
                                          EA_TransferType, USEAF_ISOCHRONOUS, TAG_END);
                IPTR raw = 0, transactions = 1;
                ULONG bytes;
                if (!ep) continue;
                psdGetAttrs(PGA_ENDPOINT, ep, EA_MaxPktSize, &raw,
                            EA_NumTransMuFrame, &transactions, TAG_END);
                bytes = raw * transactions;
                alts_with_iso++;
                if (bytes > nch->ncu_StreamMaxPkt && bytes <= altcap) {
                    nch->ncu_StreamEP = ep;
                    nch->ncu_StreamAltIf = (APTR)node;
                    nch->ncu_StreamMaxPkt = bytes;
                }
            }
        }
    }
    if (ncand && nch->ncu_StreamEP) {
        /* Negotiate rather than assume: ask the device for its default
         * probe (GET_DEF) and take that frame when it is one of ours and the
         * stream endpoint can carry it; otherwise the largest frame that fits. */
        struct MsgPort *mp = CreateMsgPort();
        APTR pipe = mp ? psdAllocPipe(nch->ncu_Device, mp, NULL) : NULL;
        UBYTE probe[26];
        IPTR vsnum = 0;
        LONG ioerr = -1, dflt = -1;
        ULONG devinterval = 0, i;
        /* bytes per second the stream endpoint carries: 8000 service
         * intervals per second at high and super speed */
        ULONG budget = nch->ncu_IsBulk ? 0xFFFFFFFFUL : (ULONG)nch->ncu_StreamMaxPkt * 8000UL;
        psdGetAttrs(PGA_INTERFACE, nch->ncu_VSInterface, IFA_InterfaceNum, &vsnum, TAG_END);
        memset(probe, 0, sizeof(probe));
        if (pipe) {
            psdPipeSetup(pipe, URTF_IN|URTF_CLASS|URTF_INTERFACE,
                         UVUDR_GET_DEF, UVVSCS_PROBE_CONTROL << 8, vsnum);
            ioerr = psdDoPipe(pipe, probe, sizeof(probe));
            if (ioerr == 0 && psdGetPipeActual(pipe) < 8) ioerr = -2;
            psdFreePipe(pipe);
        }
        if (mp) DeleteMsgPort(mp);
        if (ioerr == 0) {
            devinterval = probe[4] | ((ULONG)probe[5] << 8) |
                          ((ULONG)probe[6] << 16) | ((ULONG)probe[7] << 24);
            for (i = 0; i < ncand; i++)
                if (cand[i].format == probe[2] && cand[i].frame == probe[3]) dflt = i;
        }
        if (dflt >= 0) {
            ULONG size = cand[dflt].maxbuf ? cand[dflt].maxbuf : (ULONG)cand[dflt].w * cand[dflt].h * 2;
            ULONG ival = devinterval ? devinterval : cand[dflt].defint;
            ULONG fps = ival ? 10000000UL / ival : 30;
            if (!fps) fps = 1;
            if (size <= budget / fps) {
                chosen = dflt;
                cand[dflt].interval = ival;
                reason = "device default";
            } else {
                UVCLOG(LOGF_Flag_Type_Information, "'%s': device default %lux%lu at %lu fps needs %lu bytes/s, stream endpoint carries %lu; choosing the largest frame that fits",
                       nch->ncu_Name, (IPTR)cand[dflt].w, (IPTR)cand[dflt].h, (IPTR)fps,
                       (IPTR)(size * fps), (IPTR)budget);
            }
        } else if (ioerr == 0) {
            UVCLOG(LOGF_Flag_Type_Information, "'%s': device default is format %lu frame %lu, not one of its YUY2 frames; choosing the largest YUY2 frame that fits",
                   nch->ncu_Name, (IPTR)probe[2], (IPTR)probe[3]);
        } else {
            UVCLOG(LOGF_Flag_Type_Warn, "'%s': GET_DEF probe failed (%ld); choosing the largest YUY2 frame that fits",
                   nch->ncu_Name, (IPTR)ioerr);
        }
        if (chosen < 0) {
            ULONG bestarea = 0;
            for (i = 0; i < ncand; i++) {
                ULONG size = cand[i].maxbuf ? cand[i].maxbuf : (ULONG)cand[i].w * cand[i].h * 2;
                ULONG fps = cand[i].interval ? 10000000UL / cand[i].interval : 10;
                ULONG area = (ULONG)cand[i].w * cand[i].h;
                if (!fps) fps = 1;
                if (size <= budget / fps && area > bestarea) {
                    bestarea = area;
                    chosen = i;
                }
            }
            reason = "largest frame the stream endpoint carries";
        }
        if (chosen >= 0) {
            nch->ncu_Width = cand[chosen].w;
            nch->ncu_Height = cand[chosen].h;
            nch->ncu_FormatIndex = cand[chosen].format;
            nch->ncu_FrameIndex = cand[chosen].frame;
            nch->ncu_Interval = cand[chosen].interval;
            nch->ncu_MaxFrameBuf = (ULONG)cand[chosen].w * cand[chosen].h * 2;
            if (cand[chosen].maxbuf > nch->ncu_MaxFrameBuf && cand[chosen].maxbuf <= 4 * nch->ncu_MaxFrameBuf)
                nch->ncu_MaxFrameBuf = cand[chosen].maxbuf;
        }
    }
    nch->ncu_HaveStream = chosen >= 0 && nch->ncu_StreamEP != NULL;
done:
    if (!nch->ncu_VSInterface)
        UVCLOG(LOGF_Flag_Type_Warn, "'%s': no VideoStreaming interface", nch->ncu_Name);
    else if (!ncand)
        UVCLOG(LOGF_Flag_Type_Warn, "'%s': no uncompressed YUY2 frame up to 1920x1080", nch->ncu_Name);
    else if (!nch->ncu_StreamEP)
        UVCLOG(LOGF_Flag_Type_Warn, "'%s': %lu YUY2 frames but no bulk IN or isochronous IN alternate up to %lu bytes (%lu isochronous alternates)",
               nch->ncu_Name, (IPTR)ncand, (IPTR)altcap, (IPTR)alts_with_iso);
    else if (!nch->ncu_HaveStream)
        UVCLOG(LOGF_Flag_Type_Warn, "'%s': none of %lu YUY2 frames fits the %s stream endpoint (%lu bytes/interval)",
               nch->ncu_Name, (IPTR)ncand, nch->ncu_IsBulk ? "bulk" : "isochronous", (IPTR)nch->ncu_StreamMaxPkt);
    else
        UVCLOG(LOGF_Flag_Type_Information, "'%s': YUY2 %lux%lu format %lu frame %lu interval %lu (%s; %lu YUY2 frames offered), %s, largest alternate %lu bytes/interval (%lu isochronous alternates), frame buffer %lu",
               nch->ncu_Name, (IPTR)nch->ncu_Width, (IPTR)nch->ncu_Height,
               (IPTR)nch->ncu_FormatIndex, (IPTR)nch->ncu_FrameIndex, (IPTR)nch->ncu_Interval, reason, (IPTR)ncand,
               nch->ncu_IsBulk ? "bulk" : "isochronous", (IPTR)nch->ncu_StreamMaxPkt, (IPTR)alts_with_iso,
               (IPTR)nch->ncu_MaxFrameBuf);
    CloseLibrary(ps);
    return nch->ncu_HaveStream;
}

/* Every transfer shares a deadline for the whole grab, including controls.
 * A submitted pipe is collected exactly once, also after abort. */
static LONG uvc_transfer(struct NepUVCBase *nh, struct Library *ps, APTR pipe,
                         APTR data, ULONG size, struct MsgPort *mp,
                         struct timerequest *timer, const char *what)
{
    LONG err;
    if (CheckIO((struct IORequest *)timer)) {
        UVCLOG(LOGF_Flag_Type_Warn, "%s: not sent, grab deadline already passed", what);
        return -1;
    }
    psdSendPipe(pipe, data, size);
    while (!psdCheckPipe(pipe) && !CheckIO((struct IORequest *)timer))
        Wait((1UL << mp->mp_SigBit) |
             (1UL << timer->tr_node.io_Message.mn_ReplyPort->mp_SigBit));
    if (!psdCheckPipe(pipe)) psdAbortPipe(pipe);
    err = psdWaitPipe(pipe);
    if (err)
        UVCLOG(LOGF_Flag_Type_Warn, "%s: transfer error %ld, actual %lu of %lu%s", what,
               (IPTR)err, (IPTR)psdGetPipeActual(pipe), (IPTR)size,
               CheckIO((struct IORequest *)timer) ? " (grab deadline passed)" : "");
    return CheckIO((struct IORequest *)timer) ? -1 : err;
}

static BOOL uvc_alt(struct NepUVCBase *nh, struct Library *ps, APTR control,
                    APTR interface, struct MsgPort *mp, struct timerequest *timer)
{
    IPTR number = 0, alternate = 0;
    UBYTE current[8] = {0};
    psdGetAttrs(PGA_INTERFACE, interface, IFA_InterfaceNum, &number,
                IFA_AlternateNum, &alternate, TAG_END);
    psdPipeSetup(control, URTF_STANDARD|URTF_INTERFACE,
                 USR_SET_INTERFACE, alternate, number);
    if (uvc_transfer(nh, ps, control, NULL, 0, mp, timer, "SET_INTERFACE")) return FALSE;
    psdPipeSetup(control, URTF_IN|URTF_STANDARD|URTF_INTERFACE,
                 USR_GET_INTERFACE, 0, number);
    if (uvc_transfer(nh, ps, control, current, 1, mp, timer, "GET_INTERFACE")) return FALSE;
    if (psdGetPipeActual(control) != 1 || current[0] != alternate) {
        UVCLOG(LOGF_Flag_Type_Warn, "interface %lu: asked for alternate %lu, device reports %lu (%lu bytes)",
               number, alternate, (IPTR)current[0], (IPTR)psdGetPipeActual(control));
        return FALSE;
    }
    if (!psdSetAltInterface(NULL, interface)) {
        UVCLOG(LOGF_Flag_Type_Warn, "interface %lu alternate %lu: psdSetAltInterface failed (host controller endpoint setup)",
               number, alternate);
        return FALSE;
    }
    return TRUE;
}

static ULONG uvc_grab(struct NepClassUVC *nch, UBYTE *rgb, ULONG capacity)
{
    struct NepUVCBase *nh = nch->ncu_ClsBase;
    struct Library *ps = NULL;
    struct MsgPort *mp = NULL, *tp = NULL;
    struct timerequest *timer = NULL;
    APTR control = NULL, pipes[UVC_SLOTS] = {0};
    APTR streamalt = nch->ncu_StreamAltIf, streamep = nch->ncu_StreamEP;
    ULONG streamsize = nch->ncu_StreamMaxPkt;
    BOOL pending[UVC_SLOTS] = {0};
    struct Task *me = FindTask(NULL);
    BYTE oldpri = me->tc_Node.ln_Pri;
    BOOL raised = FALSE;
    ULONG sofgaps = 0, sofskip = 0;
    UWORD lastsof = 0;
    BOOL havesof = FALSE;
    /* Length check: every buffer is filled with a marker before it is sent;
     * afterwards the last non-marker byte says how much the controller
     * really wrote, to compare with the length it reported. */
    ULONG lenover = 0, lenunder = 0, lenoverbytes = 0, lenunderbytes = 0;
    ULONG lensample[4][2] = {{0}};
    ULONG lensamples = 0;
    ULONG slots = 0, slot = 0;
    UBYTE *raw = NULL, *payload = NULL, *payloadraw = NULL;
    ULONG stride = 0;
    UBYTE probe[26];
    IPTR ifnum = 0;
    ULONG result = 0, maxpayload = 0, maxvideoframe = 0, expected = 0, size, i, packets = 0;
    ULONG eof = 0, damaged = 0, maxframe = 0;
    ULONG nonzero = 0, totalbytes = 0, hdrbad = 0, oversize = 0;
    LONG firsterr = 0;
    IPTR altnum = 0;
    UBYTE firsthdr[12] = {0};
    ULONG firsthdrlen = 0, szhist[6] = {0}, largest = 0;
    /* Replay detector: a 12-byte header carries PTS and a source clock
     * sample, so two payloads with identical headers are the same payload
     * seen twice. Remember the last 128 and how far back the twin was. */
    UBYTE (*hdrring)[12] = NULL;
    ULONG hdrcount = 0, dups = 0, dupdist[4] = {0}, dupslot[4] = {0};
    const char *stage = "allocate";
    BOOL timeropen = FALSE, timersent = FALSE, active = FALSE, timedout = FALSE;
    struct uvc_payload frame = {0};
    struct uvc_bulk_payload bulk = {0};
    if (!nch->ncu_HaveStream || !rgb ||
        capacity < (ULONG)nch->ncu_Width*nch->ncu_Height*3) {
        UVCLOG(LOGF_Flag_Type_Warn, "grab id=%lu refused: stream %lu, buffer %lu of %lu bytes",
               (IPTR)nch->ncu_Id, (IPTR)nch->ncu_HaveStream, (IPTR)capacity,
               (IPTR)nch->ncu_Width*nch->ncu_Height*3);
        return 0;
    }
    ps = OpenLibrary("poseidon.library", 4);
    if (!ps || !(mp = CreateMsgPort()) || !(tp = CreateMsgPort())) goto out;
    timer = (struct timerequest *)CreateIORequest(tp, sizeof(*timer));
    if (!timer || OpenDevice("timer.device", UNIT_MICROHZ,
                             (struct IORequest *)timer, 0)) goto out;
    timeropen = TRUE;
    timer->tr_node.io_Command = TR_ADDREQUEST;
    timer->tr_time.tv_secs = 3;
    timer->tr_time.tv_micro = 0;
    SendIO((struct IORequest *)timer);
    timersent = TRUE;
    control = psdAllocPipe(nch->ncu_Device, mp, NULL);
    if (!control) goto out;
    psdGetAttrs(PGA_INTERFACE, nch->ncu_VSInterface, IFA_InterfaceNum, &ifnum, TAG_END);
    stage = "probe-default";
    memset(probe, 0, sizeof(probe));
    psdPipeSetup(control, URTF_IN|URTF_CLASS|URTF_INTERFACE,
                 UVUDR_GET_CUR, UVVSCS_PROBE_CONTROL << 8, ifnum);
    if (uvc_transfer(nh, ps, control, probe, sizeof(probe), mp, timer, "GET_CUR probe (default)")) goto out;
    if (psdGetPipeActual(control) != sizeof(probe)) {
        UVCLOG(LOGF_Flag_Type_Warn, "GET_CUR probe returned %lu bytes, expected %lu",
               (IPTR)psdGetPipeActual(control), (IPTR)sizeof(probe));
        goto out;
    }
    stage = "probe-set";
    probe[0] = 1; probe[1] = 0;
    probe[2] = nch->ncu_FormatIndex; probe[3] = nch->ncu_FrameIndex;
    for (i=0; i<4; ++i) probe[4+i] = nch->ncu_Interval >> (8*i);
    psdPipeSetup(control, URTF_OUT|URTF_CLASS|URTF_INTERFACE,
                 UVUDR_SET_CUR, UVVSCS_PROBE_CONTROL << 8, ifnum);
    if (uvc_transfer(nh, ps, control, probe, sizeof(probe), mp, timer, "SET_CUR probe")) goto out;
    psdPipeSetup(control, URTF_IN|URTF_CLASS|URTF_INTERFACE,
                 UVUDR_GET_CUR, UVVSCS_PROBE_CONTROL << 8, ifnum);
    if (uvc_transfer(nh, ps, control, probe, sizeof(probe), mp, timer, "GET_CUR probe (negotiated)")) goto out;
    if (psdGetPipeActual(control) != sizeof(probe) ||
        probe[2] != nch->ncu_FormatIndex || probe[3] != nch->ncu_FrameIndex) {
        UVCLOG(LOGF_Flag_Type_Warn, "probe came back as format %lu frame %lu (%lu bytes); asked for format %lu frame %lu",
               (IPTR)probe[2], (IPTR)probe[3], (IPTR)psdGetPipeActual(control),
               (IPTR)nch->ncu_FormatIndex, (IPTR)nch->ncu_FrameIndex);
        goto out;
    }
    stage = "probe-validate";
    maxvideoframe = probe[18] | ((ULONG)probe[19]<<8) |
                    ((ULONG)probe[20]<<16) | ((ULONG)probe[21]<<24);
    maxpayload = probe[22] | ((ULONG)probe[23]<<8) |
                 ((ULONG)probe[24]<<16) | ((ULONG)probe[25]<<24);
    if (maxpayload < 2 || maxpayload > 1024*1024 ||
        (!nch->ncu_IsBulk && maxpayload > nch->ncu_StreamMaxPkt)) {
        UVCLOG(LOGF_Flag_Type_Warn, "device wants %lu bytes per payload, more than the largest alternate (%lu bytes/interval)",
               (IPTR)maxpayload, (IPTR)nch->ncu_StreamMaxPkt);
        goto out;
    }
    psdPipeSetup(control, URTF_OUT|URTF_CLASS|URTF_INTERFACE,
                 UVUDR_SET_CUR, UVVSCS_COMMIT_CONTROL << 8, ifnum);
    if (uvc_transfer(nh, ps, control, probe, sizeof(probe), mp, timer, "SET_CUR commit")) goto out;
    if (!nch->ncu_IsBulk) {
        struct List *alts = NULL;
        struct Node *node;
        psdGetAttrs(PGA_INTERFACE, nch->ncu_VSInterface,
                    IFA_AlternateIfList, &alts, TAG_END);
        if (alts) for (node=alts->lh_Head; node->ln_Succ; node=node->ln_Succ) {
            APTR ep = psdFindEndpoint((APTR)node, NULL, EA_IsIn, TRUE,
                                      EA_TransferType, USEAF_ISOCHRONOUS, TAG_END);
            IPTR packet = 0, transactions = 1;
            ULONG bytes;
            if (!ep) continue;
            psdGetAttrs(PGA_ENDPOINT, ep, EA_MaxPktSize, &packet,
                        EA_NumTransMuFrame, &transactions, TAG_END);
            bytes = packet * transactions;
            if (bytes >= maxpayload && bytes < streamsize) {
                streamsize = bytes;
                streamalt = (APTR)node;
                streamep = ep;
            }
        }
    }
    stage = "alternate";
    /* psdSetAltInterface updates Poseidon's selected-interface bookkeeping. */
    if (!nch->ncu_IsBulk) {
        psdGetAttrs(PGA_INTERFACE, streamalt, IFA_AlternateNum, &altnum, TAG_END);
        /* SET_INTERFACE can succeed even if its confirming GET times out. */
        active = TRUE;
        if (!uvc_alt(nh, ps, control, streamalt, mp, timer)) goto out;
    }
    size = nch->ncu_IsBulk ? maxpayload : streamsize;
    if (nch->ncu_IsBulk) {
        bulk.capacity = maxpayload;
        bulk.data = AllocVec(maxpayload, MEMF_PUBLIC);
        if (!bulk.data) goto out;
    }
    /* The frame is as large as the device committed to, not as large as
     * the geometry implies; the picture is the first width*height*2 bytes. */
    expected = nch->ncu_MaxFrameBuf;
    if (maxvideoframe > expected && maxvideoframe <= 4 * (ULONG)nch->ncu_Width * nch->ncu_Height * 2)
        expected = maxvideoframe;
    raw = AllocVec(expected, MEMF_PUBLIC);
    slots = nch->ncu_IsBulk ? 2 : UVC_SLOTS;
    /* One transfer buffer per slot, laid out so that no transfer crosses a
     * 64 KB boundary: a power-of-two stride from a 64 KB aligned base. The
     * host controller driver splits a crossing transfer into two TRBs and
     * then reports a short payload's length against the wrong one. */
    stride = 1024;
    while (stride < size) stride <<= 1;
    if (stride > 65536) stride = size;
    payloadraw = AllocVec(stride * slots + 65536, MEMF_PUBLIC);
    payload = payloadraw ? (UBYTE *)(((IPTR)payloadraw + 65535) & ~(IPTR)65535) : NULL;
    hdrring = AllocVec(128 * 12, MEMF_PUBLIC | MEMF_CLEAR);
    if (!raw || !payload || !hdrring) goto out;
    for (i=0; i<slots; ++i) {
        pipes[i] = psdAllocPipe(nch->ncu_Device, mp, streamep);
        if (!pipes[i]) goto out;
        psdSetAttrs(PGA_PIPE, pipes[i], PPA_AllowRuntPackets, TRUE,
                    PPA_NakTimeout, TRUE, PPA_NakTimeoutTime, 1000, TAG_END);
        memset(payload + i*stride, 0xA5, size);
        psdSendPipe(pipes[i], payload + i*stride, size);
        pending[i] = TRUE;
    }
    stage = "payload";
    frame.data = raw; frame.capacity = expected;
    /* Refilling the ring is time-critical; run above ordinary tasks while
     * streaming, the way a kernel capture driver completes in softirq. */
    if (oldpri < 10) {
        SetTaskPri(me, 10);
        raised = TRUE;
    }
    while (!CheckIO((struct IORequest *)timer)) {
        ULONG actual;
        LONG error;
        while (!psdCheckPipe(pipes[slot]) && !CheckIO((struct IORequest *)timer))
            Wait((1UL << mp->mp_SigBit) | (1UL << tp->mp_SigBit));
        if (!psdCheckPipe(pipes[slot])) break;
        error = psdWaitPipe(pipes[slot]);
        pending[slot] = FALSE;
        if (error) {
            firsterr = error;
            stage = "payload-error";
            break;
        }
        packets++;
        actual = psdGetPipeActual(pipes[slot]);
        if (actual > size) {
            oversize = actual;
            stage = "payload-oversize";
            break;
        }
        {
            UBYTE *b = payload + slot*stride;
            ULONG filled = size;
            while (filled && b[filled - 1] == 0xA5) filled--;
            if (actual + 4 < filled) {
                lenunder++; lenunderbytes += filled - actual;
            } else if (filled + 4 < actual) {
                lenover++; lenoverbytes += actual - filled;
            }
            if ((actual + 4 < filled || filled + 4 < actual) && lensamples < 4) {
                lensample[lensamples][0] = actual;
                lensample[lensamples][1] = filled;
                lensamples++;
            }
        }
        if (actual) {
            UBYTE *p = payload + slot*stride;
            nonzero++;
            totalbytes += actual;
            if (actual > largest) largest = actual;
            szhist[actual < 1024 ? 0 : actual == 1024 ? 1 : actual < 2048 ? 2 :
                   actual == 2048 ? 3 : actual < 3072 ? 4 : 5]++;
            if (!firsthdrlen) {
                firsthdrlen = actual < sizeof(firsthdr) ? actual : sizeof(firsthdr);
                memcpy(firsthdr, p, firsthdrlen);
            }
            /* Continuation bytes in bulk transfers are pixels, not headers. */
            if (!nch->ncu_IsBulk) {
                if (actual < 2 || p[0] < 2 || p[0] > actual) hdrbad++;
                else {
                    if (p[1] & 2) eof++;
                    if (p[1] & 0x40) damaged++;
                }
                if (actual >= 12 && p[0] == 12 && (p[1] & 0x08)) {
                    /* SCR present: bytes 10-11 carry the 1 ms frame number; a
                     * jump of more than one between consecutive payloads means
                     * microframes went by without a transfer to land in. */
                    UWORD sof = (p[10] | ((UWORD)p[11] << 8)) & 0x7FF;
                    if (havesof) {
                        UWORD d = (sof - lastsof) & 0x7FF;
                        if (d > 1 && d < 1024) { sofgaps++; sofskip += d - 1; }
                    }
                    lastsof = sof;
                    havesof = TRUE;
                }
                if (actual >= 12 && p[0] == 12) {
                    ULONG k, n = hdrcount < 128 ? hdrcount : 128;
                    for (k = 1; k <= n; k++) {
                        if (!memcmp(hdrring[(hdrcount - k) % 128], p, 12)) {
                            if (dups < 4) { dupdist[dups] = k; dupslot[dups] = packets; }
                            dups++;
                            break;
                        }
                    }
                    memcpy(hdrring[hdrcount % 128], p, 12);
                    hdrcount++;
                }
            }
        }
        if (frame.used > maxframe) maxframe = frame.used;
        if (nch->ncu_IsBulk ?
            uvc_bulk_feed(&bulk, &frame, payload + slot*stride, actual, size) :
            (actual && uvc_payload_feed(&frame, payload + slot*stride, actual))) {
            uvc_yuy2_rgb(raw, rgb, (ULONG)nch->ncu_Width * nch->ncu_Height * 2);
            result = (ULONG)nch->ncu_Width*nch->ncu_Height*3;
            break;
        }
        memset(payload + slot*stride, 0xA5, size);
        psdSendPipe(pipes[slot], payload + slot*stride, size);
        pending[slot] = TRUE;
        slot = (slot + 1) % slots;
    }

out:
    if (raised) SetTaskPri(me, oldpri);
    if (frame.used > maxframe) maxframe = frame.used;
    if (nch->ncu_IsBulk) {
        hdrbad = frame.badhdr;
        eof = frame.eofs;
        damaged = frame.errbit;
        if (!result && bulk.data)
            UVCLOG(LOGF_Flag_Type_Information, "grab id=%lu bulk boundary state: synchronized %lu, pending payload %lu of %lu bytes",
                   (IPTR)nch->ncu_Id, (IPTR)bulk.synchronized,
                   (IPTR)bulk.used, (IPTR)bulk.capacity);
    }
    if (timer && timersent && CheckIO((struct IORequest *)timer)) timedout = TRUE;
    if (!result) {
        nh->nh_FramesFailed++;
        UVCLOG(LOGF_Flag_Type_Error, "grab id=%lu '%s' failed at %s%s: %s alt %lu, %lu bytes/transfer, device maxpayload %lu maxframe %lu; %lu transfers done, %lu with data, %lu bytes, %lu bad headers, %lu EOF, %lu ERR, longest frame %lu of %lu; first error %ld, oversize %lu (ok %lu, failed %lu)",
               (IPTR)nch->ncu_Id, nch->ncu_Name, stage, timedout ? " (3 s deadline)" : "",
               nch->ncu_IsBulk ? "bulk" : "isochronous", altnum, (IPTR)size, (IPTR)maxpayload,
               (IPTR)maxvideoframe, (IPTR)packets, (IPTR)nonzero, (IPTR)totalbytes,
               (IPTR)hdrbad, (IPTR)eof, (IPTR)damaged, (IPTR)maxframe, (IPTR)expected,
               (IPTR)firsterr, (IPTR)oversize, (IPTR)nh->nh_FramesOK, (IPTR)nh->nh_FramesFailed);
        if (firsthdrlen)
            UVCLOG(LOGF_Flag_Type_Information, "grab id=%lu first transfer bytes: %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx",
                   (IPTR)nch->ncu_Id, (IPTR)firsthdr[0], (IPTR)firsthdr[1], (IPTR)firsthdr[2],
                   (IPTR)firsthdr[3], (IPTR)firsthdr[4], (IPTR)firsthdr[5], (IPTR)firsthdr[6],
                   (IPTR)firsthdr[7], (IPTR)firsthdr[8], (IPTR)firsthdr[9], (IPTR)firsthdr[10],
                   (IPTR)firsthdr[11]);
        if (nonzero)
            UVCLOG(LOGF_Flag_Type_Information, "grab id=%lu payloads: headers 2-byte %lu, 12-byte %lu, other %lu, bad %lu; FID toggles %lu, ERR %lu, over capacity %lu, after EOF %lu; EOF frames %lu, first frames kept/sent %lu%s/%lu %lu%s/%lu %lu%s/%lu %lu%s/%lu %lu%s/%lu %lu%s/%lu",
                   (IPTR)nch->ncu_Id, (IPTR)frame.hdr2, (IPTR)frame.hdr12, (IPTR)frame.hdrother,
                   (IPTR)frame.badhdr, (IPTR)frame.fidflips, (IPTR)frame.errbit, (IPTR)frame.overshoot,
                   (IPTR)frame.afterEOF, (IPTR)frame.eofs,
                   (IPTR)frame.eofused[0], frame.eofstate[0] == 1 ? "d" : frame.eofstate[0] == 2 ? "s" : "", (IPTR)frame.eofraw[0],
                   (IPTR)frame.eofused[1], frame.eofstate[1] == 1 ? "d" : frame.eofstate[1] == 2 ? "s" : "", (IPTR)frame.eofraw[1],
                   (IPTR)frame.eofused[2], frame.eofstate[2] == 1 ? "d" : frame.eofstate[2] == 2 ? "s" : "", (IPTR)frame.eofraw[2],
                   (IPTR)frame.eofused[3], frame.eofstate[3] == 1 ? "d" : frame.eofstate[3] == 2 ? "s" : "", (IPTR)frame.eofraw[3],
                   (IPTR)frame.eofused[4], frame.eofstate[4] == 1 ? "d" : frame.eofstate[4] == 2 ? "s" : "", (IPTR)frame.eofraw[4],
                   (IPTR)frame.eofused[5], frame.eofstate[5] == 1 ? "d" : frame.eofstate[5] == 2 ? "s" : "", (IPTR)frame.eofraw[5]);
        if (nonzero)
            UVCLOG(LOGF_Flag_Type_Information, "grab id=%lu transfer sizes: empty %lu, 1-1023 %lu, 1024 %lu, 1025-2047 %lu, 2048 %lu, 2049-3071 %lu, 3072+ %lu; largest %lu; replayed payloads %lu (first at transfer %lu, twin %lu back); SOF gaps %lu, ~%lu ms of stream missed, %lu slots",
                   (IPTR)nch->ncu_Id, (IPTR)(packets - nonzero), (IPTR)szhist[0], (IPTR)szhist[1],
                   (IPTR)szhist[2], (IPTR)szhist[3], (IPTR)szhist[4], (IPTR)szhist[5], (IPTR)largest,
                   (IPTR)dups, (IPTR)dupslot[0], (IPTR)dupdist[0], (IPTR)sofgaps, (IPTR)sofskip, (IPTR)slots);
        UVCLOG(LOGF_Flag_Type_Information, "grab id=%lu reported length vs bytes written: over-reported %lu transfers (%lu bytes), under-reported %lu (%lu bytes); samples reported/written %lu/%lu %lu/%lu %lu/%lu %lu/%lu",
               (IPTR)nch->ncu_Id, (IPTR)lenover, (IPTR)lenoverbytes, (IPTR)lenunder, (IPTR)lenunderbytes,
               (IPTR)lensample[0][0], (IPTR)lensample[0][1], (IPTR)lensample[1][0], (IPTR)lensample[1][1],
               (IPTR)lensample[2][0], (IPTR)lensample[2][1], (IPTR)lensample[3][0], (IPTR)lensample[3][1]);
    } else {
        nh->nh_FramesOK++;
        if (nh->nh_FramesOK <= 3 || (nh->nh_FramesOK % 500) == 0 || dups)
            UVCLOG(LOGF_Flag_Type_Information, "grab id=%lu '%s' frame %lu: %lux%lu via %s alt %lu, %lu transfers, %lu with data, %lu bytes, %lu EOF, %lu replayed, SOF gaps %lu (~%lu ms missed) (failed so far %lu)",
                   (IPTR)nch->ncu_Id, nch->ncu_Name, (IPTR)nh->nh_FramesOK,
                   (IPTR)nch->ncu_Width, (IPTR)nch->ncu_Height,
                   nch->ncu_IsBulk ? "bulk" : "isochronous", altnum, (IPTR)packets,
                   (IPTR)nonzero, (IPTR)totalbytes, (IPTR)eof, (IPTR)dups, (IPTR)sofgaps, (IPTR)sofskip,
                   (IPTR)nh->nh_FramesFailed);
    }
    for (i=0; i<slots; ++i) {
        if (pending[i]) {
            psdAbortPipe(pipes[i]);
            psdWaitPipe(pipes[i]);
        }
        if (pipes[i]) psdFreePipe(pipes[i]);
    }
    if (active) {
        /* Give cleanup its own bounded deadline even if capture timed out. */
        if (!CheckIO((struct IORequest *)timer)) AbortIO((struct IORequest *)timer);
        WaitIO((struct IORequest *)timer);
        timer->tr_time.tv_secs = 1;
        timer->tr_time.tv_micro = 0;
        SendIO((struct IORequest *)timer);
        if (!uvc_alt(nh, ps, control, nch->ncu_VSInterface, mp, timer))
            UVCLOG(LOGF_Flag_Type_Warn, "grab id=%lu: failed to restore the idle alternate setting",
                   (IPTR)nch->ncu_Id);
    }
    if (control) psdFreePipe(control);
    if (timersent) {
        if (!CheckIO((struct IORequest *)timer)) AbortIO((struct IORequest *)timer);
        WaitIO((struct IORequest *)timer);
    }
    if (timeropen) CloseDevice((struct IORequest *)timer);
    if (timer) DeleteIORequest((struct IORequest *)timer);
    if (tp) DeleteMsgPort(tp);
    if (mp) DeleteMsgPort(mp);
    FreeVec(payloadraw); FreeVec(raw); FreeVec(hdrring); FreeVec(bulk.data);
    if (ps) CloseLibrary(ps);
    return result;
}

static IPTR uvc_method(struct NepUVCBase *nh, ULONG method, IPTR *args)
{
    struct NepClassUVC *nch, snapshot;
    struct Library *ps = NULL;
    ULONG index = 0;
    IPTR result = 0;
    BOOL found = FALSE;
    if (method == UVCM_GRAB) {
        ps = OpenLibrary("poseidon.library", 4);
        if (!ps) return 0;
        psdLockReadPBase();
    }
    ObtainSemaphore(&nh->nh_Lock);
    ForeachNode(&nh->nh_Cameras, nch) {
        if (method == UVCM_COUNT) { result++; continue; }
        if (!args) break;
        if (method == UVCM_INFO && index++ == args[0]) {
            struct UVCCameraInfo *info = (APTR)args[1];
            if (info) {
                memset(info, 0, sizeof(*info));
                info->id = nch->ncu_Id;
                info->width = nch->ncu_Width; info->height = nch->ncu_Height;
                memcpy(info->name, nch->ncu_Name, sizeof(info->name));
                result = TRUE;
            }
            break;
        }
        if (method == UVCM_MODECOUNT && nch->ncu_Id == args[0]) {
            result = nch->ncu_ModeCount;
            break;
        }
        if (method == UVCM_MODEINFO && nch->ncu_Id == args[0]) {
            if (args[1] < nch->ncu_ModeCount && args[2] &&
                args[3] >= sizeof(struct UVCModeInfo)) {
                memcpy((APTR)args[2], &nch->ncu_Modes[args[1]],
                       sizeof(struct UVCModeInfo));
                result = TRUE;
            }
            break;
        }
        if (method == UVCM_GRAB && nch->ncu_Id == args[0]) {
            snapshot = *nch;
            found = TRUE;
            break;
        }
    }
    ReleaseSemaphore(&nh->nh_Lock);
    if (ps) {
        APTR pd = NULL;
        /* The base read lock pins the active inventory while acquiring the
         * device lock. Never wait for a device lock with the class lock held:
         * binding release runs under the device lock and takes the class lock. */
        if (found) {
            while ((pd = psdGetNextDevice(pd)))
                if (pd == snapshot.ncu_Device) break;
        }
        if (pd) psdLockWriteDevice(pd);
        psdUnlockPBase();
        if (pd) {
            /* A manual unbind or configuration switch may have won while
             * acquiring the device lock. Revalidate before using descriptors. */
            found = FALSE;
            ObtainSemaphore(&nh->nh_Lock);
            ForeachNode(&nh->nh_Cameras, nch) {
                if (nch->ncu_Id == args[0]) { found = TRUE; break; }
            }
            ReleaseSemaphore(&nh->nh_Lock);
            if (found) result = uvc_grab(&snapshot, (APTR)args[1], args[2]);
            psdUnlockDevice(pd);
        }
        CloseLibrary(ps);
    }
    return result;
}

/*
 * ***********************************************************************
 * * Library functions                                                   *
 * ***********************************************************************
 */

/* /// "usbAttemptInterfaceBinding()" */
struct NepClassUVC * usbAttemptInterfaceBinding(struct NepUVCBase *nh, struct PsdInterface *pif)
{
    struct Library *ps;
    IPTR ifclass;
    IPTR subclass;

    KPRINTF(1, ("nepUVCAttemptInterfaceBinding(%08lx)\n", pif));
    if((ps = OpenLibrary("poseidon.library", 4)))
    {
        psdGetAttrs(PGA_INTERFACE, pif,
                    IFA_Class, &ifclass,
                    IFA_SubClass, &subclass,
                    TAG_DONE);
        CloseLibrary(ps);

        /* Bind on the VideoControl interface; the associated VideoStreaming
         * interface(s) are claimed via the interface association once streaming
         * is implemented. */
        if((ifclass == VIDEO_CLASSCODE) && (subclass == VIDEO_CTRL_SUBCLASS))
        {
            return(usbForceInterfaceBinding(nh, pif));
        }
    }
    return(NULL);
}
/* \\\ */

/* /// "usbForceInterfaceBinding()" */
struct NepClassUVC * usbForceInterfaceBinding(struct NepUVCBase *nh, struct PsdInterface *pif)
{
    struct Library *ps;
    struct NepClassUVC *nch;
    struct PsdConfig *pc;
    struct PsdDevice *pd;
    struct PsdDevice *hubpd;
    STRPTR devname;
    STRPTR ifidstr;
    STRPTR devidstr;
    IPTR ifnum;
    IPTR hubport;

    KPRINTF(1, ("nepUVCForceInterfaceBinding(%08lx)\n", pif));
    if((ps = OpenLibrary("poseidon.library", 4)))
    {
        psdGetAttrs(PGA_INTERFACE, pif,
                    IFA_Config, &pc,
                    IFA_IDString, &ifidstr,
                    IFA_InterfaceNum, &ifnum,
                    TAG_END);
        psdGetAttrs(PGA_CONFIG, pc,
                    CA_Device, &pd,
                    TAG_END);
        psdGetAttrs(PGA_DEVICE, pd,
                    DA_ProductName, &devname,
                    DA_IDString, &devidstr,
                    DA_HubDevice, &hubpd,
                    DA_AtHubPortNumber, &hubport,
                    TAG_END);

        if((nch = psdAllocVec(sizeof(struct NepClassUVC))))
        {
            memset(nch, 0, sizeof(*nch));
            if (devname) strncpy(nch->ncu_Name, devname, sizeof(nch->ncu_Name)-1);
            nch->ncu_ClsBase = nh;
            nch->ncu_Device = pd;
            nch->ncu_Hub = hubpd;
            nch->ncu_Config = pc;
            nch->ncu_Interface = pif;
            nch->ncu_HubPort = hubport;
            nch->ncu_IfNum = ifnum;
            nch->ncu_DevIDString = devidstr;
            nch->ncu_IfIDString = ifidstr;

            if (!uvc_parse_streaming(nch)) {
                psdAddErrorMsg(RETURN_WARN, (STRPTR)libname,
                    "No supported YUY2 mode (up to 640x480) on '%s'", devname);
                psdFreeVec(nch);
                CloseLibrary(ps);
                return NULL;
            }
            ObtainSemaphore(&nh->nh_Lock);
            nch->ncu_Id = ++nh->nh_NextId;
            AddTail(&nh->nh_Cameras, &nch->ncu_Node);
            ReleaseSemaphore(&nh->nh_Lock);
            UVCLOG(LOGF_Flag_Type_Information, "bound id=%lu '%s' (interface %lu, hub port %lu) YUY2 %lux%lu",
                   (IPTR)nch->ncu_Id, devname, (IPTR)ifnum, (IPTR)hubport,
                   (IPTR)nch->ncu_Width, (IPTR)nch->ncu_Height);
            CloseLibrary(ps);
            return(nch);
        }
        CloseLibrary(ps);
    }
    return(NULL);
}
/* \\\ */

/* /// "usbReleaseInterfaceBinding()" */
void usbReleaseInterfaceBinding(struct NepUVCBase *nh, struct NepClassUVC *nch)
{
    struct Library *ps;
    struct PsdConfig *pc;
    struct PsdDevice *pd;
    STRPTR devname;

    KPRINTF(1, ("nepUVCReleaseInterfaceBinding(%08lx)\n", nch));
    if((ps = OpenLibrary("poseidon.library", 4)))
    {
        psdGetAttrs(PGA_INTERFACE, nch->ncu_Interface, IFA_Config, &pc, TAG_END);
        psdGetAttrs(PGA_CONFIG, pc, CA_Device, &pd, TAG_END);
        psdGetAttrs(PGA_DEVICE, pd, DA_ProductName, &devname, TAG_END);
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                       "Released USB Video device '%s'!", devname);
        ObtainSemaphore(&nh->nh_Lock);
        Remove(&nch->ncu_Node);
        psdFreeVec(nch);
        ReleaseSemaphore(&nh->nh_Lock);
        CloseLibrary(ps);
    }
}
/* \\\ */

/* /// "usbGetAttrsA()" */
AROS_LH3(LONG, usbGetAttrsA,
         AROS_LHA(ULONG, type, D0),
         AROS_LHA(APTR, usbstruct, A0),
         AROS_LHA(struct TagItem *, tags, A1),
         LIBBASETYPEPTR, nh, 5, nep)
{
    AROS_LIBFUNC_INIT

    struct TagItem *ti;
    LONG count = 0;

    KPRINTF(1, ("nepUVCGetAttrsA(%ld, %08lx, %08lx)\n", type, usbstruct, tags));
    switch(type)
    {
        case UGA_CLASS:
            if((ti = FindTagItem(UCCA_Priority, tags)))
            {
                *((SIPTR *) ti->ti_Data) = -100;
                count++;
            }
            if((ti = FindTagItem(UCCA_Description, tags)))
            {
                *((STRPTR *) ti->ti_Data) = "USB Video Class (webcams, capture)";
                count++;
            }
            if((ti = FindTagItem(UCCA_HasClassCfgGUI, tags)))
            {
                *((IPTR *) ti->ti_Data) = FALSE;
                count++;
            }
            if((ti = FindTagItem(UCCA_HasBindingCfgGUI, tags)))
            {
                *((IPTR *) ti->ti_Data) = FALSE;
                count++;
            }
            if((ti = FindTagItem(UCCA_AfterDOSRestart, tags)))
            {
                *((IPTR *) ti->ti_Data) = FALSE;
                count++;
            }
            if((ti = FindTagItem(UCCA_UsingDefaultCfg, tags)))
            {
                *((IPTR *) ti->ti_Data) = TRUE;
                count++;
            }
            break;

        case UGA_BINDING:
            if((ti = FindTagItem(UCBA_UsingDefaultCfg, tags)))
            {
                *((IPTR *) ti->ti_Data) = TRUE;
                count++;
            }
            break;
    }
    return(count);
    AROS_LIBFUNC_EXIT
}
/* \\\ */

/* /// "usbSetAttrsA()" */
AROS_LH3(LONG, usbSetAttrsA,
         AROS_LHA(ULONG, type, D0),
         AROS_LHA(APTR, usbstruct, A0),
         AROS_LHA(struct TagItem *, tags, A1),
         LIBBASETYPEPTR, nh, 6, nep)
{
    AROS_LIBFUNC_INIT
    return(0);
    AROS_LIBFUNC_EXIT
}
/* \\\ */

/* /// "usbDoMethodA()" */
AROS_LH2(IPTR, usbDoMethodA,
         AROS_LHA(ULONG, methodid, D0),
         AROS_LHA(IPTR *, methoddata, A1),
         LIBBASETYPEPTR, nh, 7, nep)
{
    AROS_LIBFUNC_INIT

    KPRINTF(10, ("Do Method %ld\n", methodid));
    if (methodid >= UVCM_COUNT && methodid <= UVCM_MODEINFO)
        return uvc_method(nh, methodid, methoddata);
    switch(methodid)
    {
        case UCM_AttemptInterfaceBinding:
            return((IPTR) usbAttemptInterfaceBinding(nh, (struct PsdInterface *) methoddata[0]));

        case UCM_ForceInterfaceBinding:
            return((IPTR) usbForceInterfaceBinding(nh, (struct PsdInterface *) methoddata[0]));

        case UCM_ReleaseInterfaceBinding:
            usbReleaseInterfaceBinding(nh, (struct NepClassUVC *) methoddata[0]);
            return(TRUE);

        default:
            return(0);
    }
    AROS_LIBFUNC_EXIT
}
/* \\\ */
