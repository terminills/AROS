/*
 *----------------------------------------------------------------------------
 *                  uvc (USB Video Class) class for poseidon
 *----------------------------------------------------------------------------
 *
 * Poseidon class that binds USB Video Class devices (interface class
 * VIDEO_CLASSCODE 0x0E, VideoControl subclass 0x01) — webcams and HDMI-capture
 * dongles. SKELETON: it recognises and claims the device and creates the
 * per-binding context (struct NepClassUVC); streaming setup (endpoint
 * negotiation, MJPEG/YUYV decode) is the labeled next step. Once streaming
 * lands, decoded frames flow from here into uvc.library's uvc_grab() seam,
 * replacing that driver's built-in test pattern.
 *
 * Clean-room from the USB Video Class spec and AROS <devices/usb_video.h>; not
 * derived from any GPL source.
 */

#include "debug.h"

#include "uvc.class.h"

/* /// "Lib Stuff" */
static const STRPTR libname = MOD_NAME_STRING;

static int libInit(LIBBASETYPEPTR nh)
{
    struct NepUVCBase *ret = NULL;

    KPRINTF(10, ("libInit nh: 0x%08lx SysBase: 0x%08lx\n", nh, SysBase));

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

/* /// "uvc_parse_streaming()" */
/* Streaming foundation, part 1: from the device's REAL descriptors, find the
 * VideoStreaming interface, its isochronous IN endpoint, and the default
 * format+frame (MJPEG preferred, else uncompressed). Fills nch->ncu_Stream*.
 * Pure descriptor reads — no transfers, safe to run during bind. Field offsets
 * are the byte-exact UVC wire layouts from <devices/usb_video.h>. */
static BOOL uvc_parse_streaming(struct NepClassUVC *nch)
{
    struct Library *ps;
    struct PsdInterface *vsif;
    APTR dd;
    UBYTE deffidx = 0;      /* format's bDefaultFrameIndex (0 = take first frame) */
    BOOL haveformat = FALSE;

    if(!(ps = OpenLibrary("poseidon.library", 4)))
        return(FALSE);

    vsif = psdFindInterface(nch->ncu_Device, NULL,
                            IFA_Class, VIDEO_CLASSCODE,
                            IFA_SubClass, VIDEO_STREAM_SUBCLASS,
                            TAG_END);
    nch->ncu_VSInterface = vsif;
    if(vsif)
    {
        /* The isochronous IN endpoint that carries video payloads lives on the
         * VideoStreaming ALT settings (1..N), not alt 0 (which is the zero-
         * bandwidth idle setting with no endpoints). Walk the alternate-interface
         * list to find the first alt setting that exposes an isoc IN endpoint.
         * (pif_Node is the first member of a PsdInterface, so each list node IS
         * an alt-setting interface — same idiom as lan78xx.class.) */
        {
            struct List *altlist = NULL;
            struct Node *node;
            psdGetAttrs(PGA_INTERFACE, vsif, IFA_AlternateIfList, &altlist, TAG_END);
            if(altlist)
            {
                for(node = altlist->lh_Head; node->ln_Succ; node = node->ln_Succ)
                {
                    APTR ep = psdFindEndpoint((APTR) node, NULL,
                                              EA_IsIn, TRUE,
                                              EA_TransferType, USEAF_ISOCHRONOUS,
                                              TAG_END);
                    if(ep)
                    {
                        IPTR mps = 0;
                        psdGetAttrs(PGA_ENDPOINT, ep, EA_MaxPktSize, &mps, TAG_END);
                        /* keep the highest-bandwidth alt setting — it must carry
                         * at least the negotiated maxpayload to stream. */
                        if((UWORD) mps > nch->ncu_StreamMaxPkt)
                        {
                            nch->ncu_StreamEP = ep;
                            nch->ncu_StreamAltIf = (APTR) node;
                            nch->ncu_StreamMaxPkt = (UWORD) mps;
                        }
                    }
                }
            }
            /* Capture dongles (e.g. HDMI grabbers) often stream over a BULK
             * IN endpoint on alt 0 rather than isochronous alt settings. Fall
             * back to that so both webcams and capture cards are covered. */
            if(!nch->ncu_StreamEP)
            {
                nch->ncu_StreamEP = psdFindEndpoint(vsif, NULL,
                                                    EA_IsIn, TRUE,
                                                    EA_TransferType, USEAF_BULK,
                                                    TAG_END);
                nch->ncu_IsBulk = (nch->ncu_StreamEP != NULL);
            }
        }

        dd = NULL;
        while((dd = psdFindDescriptor(nch->ncu_Device, dd,
                                      DDA_DescriptorType, UDT_CS_INTERFACE,
                                      DDA_Interface, vsif,
                                      TAG_END)))
        {
            UBYTE *raw = NULL;
            UBYTE subtype;
            psdGetAttrs(PGA_DESCRIPTOR, dd, DDA_DescriptorData, &raw, TAG_END);
            if(!raw)
                continue;
            subtype = raw[2];   /* bDescriptorSubType */

            if(!nch->ncu_HaveStream &&
               (subtype == UDST_VIDEO_STREAM_FORMAT_MJPEG ||
                subtype == UDST_VIDEO_STREAM_FORMAT_UNCOMPRESSED))
            {
                /* Lock the format identity together with the frame below: once a
                 * frame is captured (ncu_HaveStream), a LATER format descriptor
                 * (e.g. the uncompressed format after MJPEG) must not overwrite
                 * IsMJPEG/FormatIndex, or we'd staple one format's frame onto
                 * another format's identity. */
                nch->ncu_IsMJPEG = (subtype == UDST_VIDEO_STREAM_FORMAT_MJPEG);
                nch->ncu_FormatIndex = raw[3];              /* bFormatIndex */
                /* MJPEG format: bDefaultFrameIndex at offset 6. Uncompressed
                 * carries a 16-byte GUID first, so its default index sits
                 * later; be conservative and take the first frame there. */
                deffidx = nch->ncu_IsMJPEG ? raw[6] : 0;
                haveformat = TRUE;
            }
            else if(haveformat && !nch->ncu_HaveStream &&
                    (subtype == UDST_VIDEO_STREAM_FRAME_MJPEG ||
                     subtype == UDST_VIDEO_STREAM_FRAME_UNCOMPRESSED) &&
                    (deffidx == 0 || raw[3] == deffidx))
            {
                /* UVC frame descriptor: wWidth@5, wHeight@7,
                 * dwMaxVideoFrameBuffer@17 (all little-endian). */
                nch->ncu_FrameIndex = raw[3];               /* bFrameIndex */
                nch->ncu_Width  = raw[5] | (raw[6] << 8);
                nch->ncu_Height = raw[7] | (raw[8] << 8);
                nch->ncu_MaxFrameBuf = raw[17] | (raw[18] << 8) |
                                       (raw[19] << 16) | (raw[20] << 24);
                nch->ncu_HaveStream = TRUE;
            }
        }
    }
    CloseLibrary(ps);
    return(nch->ncu_HaveStream);
}
/* \\\ */

/* /// "uvc_negotiate()" */
/* Streaming foundation, part 2: the UVC Probe/Commit negotiation over EP0
 * (SET_CUR PROBE -> GET_CUR PROBE -> SET_CUR COMMIT), which fixes the format,
 * frame and payload size before any isochronous transfer. This is the
 * scaffolding; opening the isoc pipe, selecting the streaming alt setting, and
 * pulling+decoding payloads is the remaining step (see NOTE at the tail). */
static void uvc_negotiate(struct NepClassUVC *nch)
{
    struct Library *ps;
    struct MsgPort *mp;
    APTR ep0;
    IPTR vsifnum = 0;
    struct uvc_streaming_control probe;

    if(!nch->ncu_HaveStream)
        return;
    if(!(ps = OpenLibrary("poseidon.library", 4)))
        return;

    psdGetAttrs(PGA_INTERFACE, nch->ncu_VSInterface,
                IFA_InterfaceNum, &vsifnum, TAG_END);

    if((mp = CreateMsgPort()))
    {
        if((ep0 = psdAllocPipe(nch->ncu_Device, mp, NULL)))
        {
            LONG ioerr;
            ULONG maxpayload = 0;

            /* GET_CUR(PROBE) FIRST to seed the struct with the device's own valid
             * defaults. A mostly-zero SET_CUR makes some devices return a zeroed
             * payload size (maxpayload=0); seeding from the device avoids that.
             * Then override only the format+frame we want. */
            memset(&probe, 0, sizeof(probe));
            psdPipeSetup(ep0, URTF_IN | URTF_CLASS | URTF_INTERFACE,
                         UVUDR_GET_CUR, UVVSCS_PROBE_CONTROL << 8, (UWORD)vsifnum);
            psdDoPipe(ep0, &probe, sizeof(probe));

            probe.bFormatIndex = nch->ncu_FormatIndex;
            probe.bFrameIndex = nch->ncu_FrameIndex;

            /* SET_CUR(PROBE) — propose our format/frame */
            psdPipeSetup(ep0, URTF_OUT | URTF_CLASS | URTF_INTERFACE,
                         UVUDR_SET_CUR, UVVSCS_PROBE_CONTROL << 8, (UWORD)vsifnum);
            psdDoPipe(ep0, &probe, sizeof(probe));

            /* GET_CUR(PROBE) — read the negotiated result (payload size, interval) */
            psdPipeSetup(ep0, URTF_IN | URTF_CLASS | URTF_INTERFACE,
                         UVUDR_GET_CUR, UVVSCS_PROBE_CONTROL << 8, (UWORD)vsifnum);
            ioerr = psdDoPipe(ep0, &probe, sizeof(probe));

            if(!ioerr)
            {
                maxpayload = probe.dwMaxPayloadTransferSize[0] |
                             (probe.dwMaxPayloadTransferSize[1] << 8) |
                             (probe.dwMaxPayloadTransferSize[2] << 16) |
                             (probe.dwMaxPayloadTransferSize[3] << 24);
                /* SET_CUR(COMMIT) — lock it in */
                psdPipeSetup(ep0, URTF_OUT | URTF_CLASS | URTF_INTERFACE,
                             UVUDR_SET_CUR, UVVSCS_COMMIT_CONTROL << 8,
                             (UWORD)vsifnum);
                psdDoPipe(ep0, &probe, sizeof(probe));
            }
            psdAddErrorMsg(ioerr ? RETURN_WARN : RETURN_OK, (STRPTR) libname,
                           "UVC negotiated: %s %ldx%ld fmt=%ld frame=%ld maxpayload=%ld (ioerr %ld)",
                           nch->ncu_IsMJPEG ? "MJPEG" : "uncompressed",
                           (IPTR) nch->ncu_Width, (IPTR) nch->ncu_Height,
                           (IPTR) probe.bFormatIndex, (IPTR) probe.bFrameIndex,
                           (IPTR) maxpayload, (IPTR) ioerr);
            /* headless-observable mirror (QEMU serial/debug log) */
            bug("uvc.class: UVC negotiated %s %ldx%ld fmt=%ld frame=%ld maxpayload=%ld ioerr=%ld\n",
                nch->ncu_IsMJPEG ? "MJPEG" : "uncompressed",
                (IPTR) nch->ncu_Width, (IPTR) nch->ncu_Height,
                (IPTR) probe.bFormatIndex, (IPTR) probe.bFrameIndex,
                (IPTR) maxpayload, (IPTR) ioerr);
            psdFreePipe(ep0);
        }
        DeleteMsgPort(mp);
    }
    CloseLibrary(ps);

    /* NOTE (remaining streaming step): with the format committed, uvc_grab_probe
     * exercises the isoc pipe; full frame reassembly + decode + the uvc.library
     * per-input grab handoff are the follow-on. */
}
/* \\\ */

/* /// "uvc_grab_probe()" */
/* Streaming transfer, increment 1: activate the highest-bandwidth streaming alt
 * setting, open the isochronous IN pipe, pull ONE transfer, and verify a real
 * UVC payload arrived (header length + bmHeader + END_OF_FRAME bit). De-risks
 * the isoc mechanism before full frame reassembly/decode. Bounded to one
 * transfer, then restores the idle alt 0 to release the isoc bandwidth. */
static void uvc_grab_probe(struct NepClassUVC *nch)
{
    struct Library *ps;
    struct MsgPort *mp;
    APTR ep0, isocpipe;
    ULONG bufsize;

    if(!nch->ncu_StreamEP || !nch->ncu_StreamAltIf || nch->ncu_IsBulk)
        return;
    if(!(ps = OpenLibrary("poseidon.library", 4)))
        return;
    bufsize = nch->ncu_StreamMaxPkt ? nch->ncu_StreamMaxPkt : 1024;

    if((mp = CreateMsgPort()))
    {
        if((ep0 = psdAllocPipe(nch->ncu_Device, mp, NULL)))
        {
            /* switch VideoStreaming to the streaming alt setting (commits isoc
             * bandwidth) */
            if(psdSetAltInterface(ep0, nch->ncu_StreamAltIf))
            {
                if((isocpipe = psdAllocPipe(nch->ncu_Device, mp, nch->ncu_StreamEP)))
                {
                    UBYTE *buf = psdAllocVec(bufsize);
                    if(buf)
                    {
                        LONG ioerr;
                        ULONG actual;
                        memset(buf, 0, bufsize);
                        ioerr = psdDoPipe(isocpipe, buf, bufsize);
                        actual = psdGetPipeActual(isocpipe);
                        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                       "isoc probe: maxpkt=%ld ioerr=%ld actual=%ld hdrlen=%ld bmHeader=0x%02lx",
                                       (IPTR) nch->ncu_StreamMaxPkt, (IPTR) ioerr,
                                       (IPTR) actual, (IPTR) buf[0], (IPTR) buf[1]);
                        bug("uvc.class: isoc probe maxpkt=%ld ioerr=%ld actual=%ld hdrlen=%ld bmHeader=0x%02lx eof=%ld\n",
                            (IPTR) nch->ncu_StreamMaxPkt, (IPTR) ioerr, (IPTR) actual,
                            (IPTR) buf[0], (IPTR) buf[1],
                            (IPTR) ((buf[1] & UVMJHF_END_OF_FRAME) ? 1 : 0));
                        psdFreeVec(buf);
                    }
                    psdFreePipe(isocpipe);
                }
                /* release the isoc bandwidth: back to the idle alt 0 */
                psdSetAltInterface(ep0, nch->ncu_VSInterface);
            }
            psdFreePipe(ep0);
        }
        DeleteMsgPort(mp);
    }
    CloseLibrary(ps);
}
/* \\\ */

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
            nch->ncu_ClsBase = nh;
            nch->ncu_Device = pd;
            nch->ncu_Hub = hubpd;
            nch->ncu_Config = pc;
            nch->ncu_Interface = pif;
            nch->ncu_HubPort = hubport;
            nch->ncu_IfNum = ifnum;
            nch->ncu_DevIDString = devidstr;
            nch->ncu_IfIDString = ifidstr;

            /* TODO(streaming): walk the class-specific VideoStreaming
             * descriptors (UDST_VIDEO_STREAM_FORMAT_MJPEG /
             * _FORMAT_UNCOMPRESSED), pick a format+frame, open the isochronous
             * or bulk streaming pipe, and expose decoded frames to
             * uvc.library's uh_UvcBackend seam. */

            psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                           "Bound USB Video device '%s'", devname);
            /* Streaming foundation: parse the device's real VideoStreaming
             * descriptors, then negotiate format/frame over EP0. Non-fatal —
             * the binding stands regardless of streaming readiness. */
            if(uvc_parse_streaming(nch))
            {
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                               "  VideoStreaming: %s %ldx%ld (fmt %ld frame %ld), %s-ep %s",
                               nch->ncu_IsMJPEG ? "MJPEG" : "uncompressed",
                               (IPTR) nch->ncu_Width, (IPTR) nch->ncu_Height,
                               (IPTR) nch->ncu_FormatIndex, (IPTR) nch->ncu_FrameIndex,
                               nch->ncu_IsBulk ? "bulk" : "isoc",
                               nch->ncu_StreamEP ? "found" : "MISSING");
                /* headless-observable mirror (captured in the QEMU debug log) */
                bug("uvc.class: VideoStreaming %s %ldx%ld fmt=%ld frame=%ld %s-ep=%s\n",
                    nch->ncu_IsMJPEG ? "MJPEG" : "uncompressed",
                    (IPTR) nch->ncu_Width, (IPTR) nch->ncu_Height,
                    (IPTR) nch->ncu_FormatIndex, (IPTR) nch->ncu_FrameIndex,
                    nch->ncu_IsBulk ? "bulk" : "isoc",
                    nch->ncu_StreamEP ? "found" : "MISSING");
                uvc_negotiate(nch);
                uvc_grab_probe(nch);
            }
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
        psdFreeVec(nch);
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
