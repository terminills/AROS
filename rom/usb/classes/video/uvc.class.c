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
    CloseLibrary((struct Library *) UtilityBase);
    return(TRUE);
}

ADD2INITLIB(libInit, 0)
ADD2EXPUNGELIB(libExpunge, 0)
/* \\\ */

/* Only YUY2 is accepted here: other uncompressed GUIDs have different layouts.
 * Prefer the smallest advertised mode, bounded to VGA for USB bandwidth and
 * memory use. The bDefaultFrameInterval belongs to that exact frame. */
static BOOL uvc_parse_streaming(struct NepClassUVC *nch)
{
    struct Library *ps = OpenLibrary("poseidon.library", 4);
    APTR dd = NULL;
    UBYTE format = 0;
    ULONG best = 0;
    static const UBYTE yuy2[16] = {
        'Y','U','Y','2',0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71
    };
    if (!ps) return FALSE;
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
            if (w && h && !(w & 1) && w <= 640 && h <= 480 && interval &&
                (!best || w*h < best)) {
                best = w*h;
                nch->ncu_Width = w;
                nch->ncu_Height = h;
                nch->ncu_FormatIndex = format;
                nch->ncu_FrameIndex = p[3];
                nch->ncu_Interval = interval;
                nch->ncu_MaxFrameBuf = best * 2;
            }
        }
    }
    if (best) {
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
                if (bytes > nch->ncu_StreamMaxPkt && bytes <= 3072) {
                    nch->ncu_StreamEP = ep;
                    nch->ncu_StreamAltIf = (APTR)node;
                    nch->ncu_StreamMaxPkt = bytes;
                }
            }
        }
        nch->ncu_HaveStream = nch->ncu_StreamEP != NULL;
    }
done:
    CloseLibrary(ps);
    return nch->ncu_HaveStream;
}

/* Every transfer shares a deadline for the whole grab, including controls.
 * A submitted pipe is collected exactly once, also after abort. */
static LONG uvc_transfer(struct Library *ps, APTR pipe, APTR data, ULONG size,
                         struct MsgPort *mp, struct timerequest *timer)
{
    LONG err;
    if (CheckIO((struct IORequest *)timer)) return -1;
    psdSendPipe(pipe, data, size);
    while (!psdCheckPipe(pipe) && !CheckIO((struct IORequest *)timer))
        Wait((1UL << mp->mp_SigBit) |
             (1UL << timer->tr_node.io_Message.mn_ReplyPort->mp_SigBit));
    if (!psdCheckPipe(pipe)) psdAbortPipe(pipe);
    err = psdWaitPipe(pipe);
    if (err) bug("uvc.class: transfer error=%ld actual=%lu requested=%lu\n",
                 (IPTR)err, (IPTR)psdGetPipeActual(pipe), (IPTR)size);
    return CheckIO((struct IORequest *)timer) ? -1 : err;
}

static BOOL uvc_alt(struct Library *ps, APTR control, APTR interface,
                    struct MsgPort *mp, struct timerequest *timer)
{
    IPTR number = 0, alternate = 0;
    UBYTE current[8] = {0};
    psdGetAttrs(PGA_INTERFACE, interface, IFA_InterfaceNum, &number,
                IFA_AlternateNum, &alternate, TAG_END);
    psdPipeSetup(control, URTF_STANDARD|URTF_INTERFACE,
                 USR_SET_INTERFACE, alternate, number);
    if (uvc_transfer(ps, control, NULL, 0, mp, timer)) return FALSE;
    psdPipeSetup(control, URTF_IN|URTF_STANDARD|URTF_INTERFACE,
                 USR_GET_INTERFACE, 0, number);
    if (uvc_transfer(ps, control, current, 1, mp, timer) ||
        psdGetPipeActual(control) != 1 || current[0] != alternate) return FALSE;
    return psdSetAltInterface(NULL, interface);
}

static ULONG uvc_grab(struct NepClassUVC *nch, UBYTE *rgb, ULONG capacity)
{
    struct Library *ps = NULL;
    struct MsgPort *mp = NULL, *tp = NULL;
    struct timerequest *timer = NULL;
    APTR control = NULL, pipes[64] = {0};
    APTR streamalt = nch->ncu_StreamAltIf, streamep = nch->ncu_StreamEP;
    ULONG streamsize = nch->ncu_StreamMaxPkt;
    BOOL pending[64] = {0};
    ULONG slots = 0, slot = 0;
    UBYTE *raw = NULL, *payload = NULL;
    UBYTE probe[26];
    IPTR ifnum = 0;
    ULONG result = 0, maxpayload = 0, size, i, packets = 0;
    ULONG eof = 0, damaged = 0, maxframe = 0;
    const char *stage = "allocate";
    BOOL timeropen = FALSE, timersent = FALSE, active = FALSE;
    struct uvc_payload frame = {0};
    if (!nch->ncu_HaveStream || !rgb ||
        capacity < (ULONG)nch->ncu_Width*nch->ncu_Height*3) return 0;
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
    if (uvc_transfer(ps, control, probe, sizeof(probe), mp, timer) ||
        psdGetPipeActual(control) != sizeof(probe)) goto out;
    stage = "probe-set";
    probe[0] = 1; probe[1] = 0;
    probe[2] = nch->ncu_FormatIndex; probe[3] = nch->ncu_FrameIndex;
    for (i=0; i<4; ++i) probe[4+i] = nch->ncu_Interval >> (8*i);
    psdPipeSetup(control, URTF_OUT|URTF_CLASS|URTF_INTERFACE,
                 UVUDR_SET_CUR, UVVSCS_PROBE_CONTROL << 8, ifnum);
    if (uvc_transfer(ps, control, probe, sizeof(probe), mp, timer)) goto out;
    psdPipeSetup(control, URTF_IN|URTF_CLASS|URTF_INTERFACE,
                 UVUDR_GET_CUR, UVVSCS_PROBE_CONTROL << 8, ifnum);
    if (uvc_transfer(ps, control, probe, sizeof(probe), mp, timer) ||
        psdGetPipeActual(control) != sizeof(probe) ||
        probe[2] != nch->ncu_FormatIndex || probe[3] != nch->ncu_FrameIndex) goto out;
    stage = "probe-validate";
    maxpayload = probe[22] | ((ULONG)probe[23]<<8) |
                 ((ULONG)probe[24]<<16) | ((ULONG)probe[25]<<24);
    if (maxpayload < 2 || maxpayload > 1024*1024 ||
        (!nch->ncu_IsBulk && maxpayload > nch->ncu_StreamMaxPkt)) goto out;
    psdPipeSetup(control, URTF_OUT|URTF_CLASS|URTF_INTERFACE,
                 UVUDR_SET_CUR, UVVSCS_COMMIT_CONTROL << 8, ifnum);
    if (uvc_transfer(ps, control, probe, sizeof(probe), mp, timer)) goto out;
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
        /* SET_INTERFACE can succeed even if its confirming GET times out. */
        active = TRUE;
        if (!uvc_alt(ps, control, streamalt, mp, timer)) goto out;
    }
    size = nch->ncu_IsBulk ? maxpayload : streamsize;
    raw = AllocVec(nch->ncu_MaxFrameBuf, MEMF_PUBLIC);
    slots = nch->ncu_IsBulk ? 2 : 64;
    payload = AllocVec(size * slots, MEMF_PUBLIC);
    if (!raw || !payload) goto out;
    for (i=0; i<slots; ++i) {
        pipes[i] = psdAllocPipe(nch->ncu_Device, mp, streamep);
        if (!pipes[i]) goto out;
        psdSetAttrs(PGA_PIPE, pipes[i], PPA_AllowRuntPackets, TRUE,
                    PPA_NakTimeout, TRUE, PPA_NakTimeoutTime, 1000, TAG_END);
        psdSendPipe(pipes[i], payload + i*size, size);
        pending[i] = TRUE;
    }
    stage = "payload";
    frame.data = raw; frame.capacity = nch->ncu_MaxFrameBuf;
    while (!CheckIO((struct IORequest *)timer)) {
        ULONG actual;
        LONG error;
        while (!psdCheckPipe(pipes[slot]) && !CheckIO((struct IORequest *)timer))
            Wait((1UL << mp->mp_SigBit) | (1UL << tp->mp_SigBit));
        if (!psdCheckPipe(pipes[slot])) break;
        error = psdWaitPipe(pipes[slot]);
        pending[slot] = FALSE;
        if (error) {
            bug("uvc.class: payload error=%ld\n", (IPTR)error);
            break;
        }
        packets++;
        actual = psdGetPipeActual(pipes[slot]);
        if (actual > size) break;
        if (actual >= 2) {
            UBYTE *p = payload + slot*size;
            if (p[1] & 2) eof++;
            if (p[1] & 0x40) damaged++;
        }
        if (frame.used > maxframe) maxframe = frame.used;
        if (actual && uvc_payload_feed(&frame, payload + slot*size, actual)) {
            uvc_yuy2_rgb(raw, rgb, nch->ncu_MaxFrameBuf);
            result = (ULONG)nch->ncu_Width*nch->ncu_Height*3;
            break;
        }
        psdSendPipe(pipes[slot], payload + slot*size, size);
        pending[slot] = TRUE;
        slot = (slot + 1) % slots;
    }

out:
    if (!result) bug("uvc.class: grab id=%lu failed stage=%s packets=%lu bytes=%lu maxpayload=%lu\n",
        (IPTR)nch->ncu_Id, stage, (IPTR)packets, (IPTR)frame.used, (IPTR)maxpayload);
    if (!result) bug("uvc.class: EOF=%lu ERR=%lu maxframe=%lu expected=%lu\n",
        (IPTR)eof, (IPTR)damaged, (IPTR)maxframe, (IPTR)nch->ncu_MaxFrameBuf);
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
        if (!uvc_alt(ps, control, nch->ncu_VSInterface, mp, timer))
            bug("uvc.class: failed to restore idle alternate setting\n");
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
    FreeVec(payload); FreeVec(raw);
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
            bug("uvc.class: bound id=%lu YUY2 %lux%lu %s\n",
                (IPTR)nch->ncu_Id, (IPTR)nch->ncu_Width,
                (IPTR)nch->ncu_Height, devname);
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
    if (methodid >= UVCM_COUNT && methodid <= UVCM_GRAB)
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
