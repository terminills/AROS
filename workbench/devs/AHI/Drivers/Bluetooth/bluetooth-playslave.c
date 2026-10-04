/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Play slave of the Bluetooth A2DP audio driver: finds the audio sink,
    runs AVDTP signalling, SBC encodes the mixed output and sends it as RTP
    packets on the AVDTP transport channel, paced by timer.device.
*/

#include <config.h>

#include <devices/ahi.h>
#include <devices/timer.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <libraries/ahi_sub.h>
#include <libraries/bluetooth.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bluetooth.h>
#include <proto/timer.h>

#include <string.h>

#include "DriverData.h"
#include "library.h"

#define dd ((struct BluetoothAudioData*) AudioCtrl->ahiac_DriverData)

/* The sink to use: an address like "cc:bf:0c:01:41:4c". Without it the
   first known device offering the Audio Sink service is used. */
#define BTA_ENV_DEVICE      "Bluetooth/AudioDevice"
#define BTA_AUDIO_SINK      0x110b
#define BTA_PREBUFFER       3       /* packets sent ahead when streaming starts */
#define BTA_CONNECT_TRIES   40      /* x 500 ms waiting for services */
/* Our L2CAP receive MTU on the AVDTP channels; the peer's is not known,
   so media packets are kept within the default size. */
#define BTA_MEDIA_MTU       672
#define BTA_MEDIA_SETTLE_TICKS 10   /* ~145 ms at 640 samples per tick */

/******************************************************************************
** A2DP source callbacks ******************************************************
******************************************************************************/

static bt_status_t bta_send_signal(void *context, const uint8_t *pdu, size_t len)
{
    struct AHIAudioCtrlDrv *AudioCtrl = context;
    struct DriverBase *AHIsubBase = (struct DriverBase *) dd->ahisubbase;
    struct Library *BluetoothBase = dd->btbase;
    LONG err;

    (void) AHIsubBase;
    err = btDoChannel(dd->sigwrite, (APTR) pdu, len);
    return err ? BT_ERR_IO : BT_OK;
}

static void bta_open_media(void *context)
{
    struct AHIAudioCtrlDrv *AudioCtrl = context;

    dd->wantmedia = TRUE;
}

static void bta_state_changed(void *context, enum bt_a2dp_source_state state)
{
    struct AHIAudioCtrlDrv *AudioCtrl = context;
    struct DriverBase *AHIsubBase = (struct DriverBase *) dd->ahisubbase;

    KPrintF("[bta] state %ld\n", (IPTR) state);
    if(state == BT_A2DP_STREAMING) {
        struct bt_sbc_config *c = &dd->source.config;
        size_t mtu = BTA_MEDIA_MTU;

        if(bt_sbc_encoder_init(&dd->encoder, c)) {
            dd->framelen = bt_sbc_frame_length(c);
            dd->samplesperframe = (ULONG) c->blocks * c->subbands;
            dd->framesperpacket = bt_a2dp_frames_per_packet(mtu, dd->framelen);
            if(dd->framesperpacket > BTA_MAX_FRAMES) {
                dd->framesperpacket = BTA_MAX_FRAMES;
            }
            dd->encoderready = (dd->framesperpacket > 0);
            KPrintF("[bta] streaming: freq %ld mode %ld blocks %ld subbands %ld bitpool %ld, %ld frames of %ld bytes per packet\n",
                    (IPTR) c->frequency, (IPTR) c->mode, (IPTR) c->blocks, (IPTR) c->subbands,
                    (IPTR) c->bitpool, (IPTR) dd->framesperpacket, (IPTR) dd->framelen);
        }
    } else if(state == BT_A2DP_FAILED) {
        KPrintF("[bta] A2DP setup failed: signal %ld error 0x%02lx\n",
                (IPTR) dd->source.fail_signal, (IPTR) dd->source.fail_error);
        dd->encoderready = FALSE;
    } else if(state != BT_A2DP_STARTING) {
        dd->encoderready = FALSE;
    }
}

static const struct bt_a2dp_source_ops bta_ops = {
    bta_send_signal,
    bta_open_media,
    bta_state_changed
};

/******************************************************************************
** PCM ************************************************************************
******************************************************************************/

/* Append one mixer buffer to the FIFO as interleaved stereo 16 bit. */
static void bta_take_mix(struct AHIAudioCtrlDrv *AudioCtrl)
{
    ULONG n = AudioCtrl->ahiac_BuffSamples;
    ULONG room = BTA_FIFO_FRAMES - dd->fifofill;
    WORD *out = &dd->fifo[dd->fifofill * 2];
    ULONG i;

    if(n > room) {
        n = room; /* the sink fell behind: drop the newest samples */
    }
    switch(AudioCtrl->ahiac_BuffType) {
    case AHIST_S16S: {
        const WORD *in = dd->mixbuffer;
        memcpy(out, in, n * 2 * sizeof(WORD));
        break;
    }
    case AHIST_M16S: {
        const WORD *in = dd->mixbuffer;
        for(i = 0; i < n; i++) {
            out[2 * i] = out[2 * i + 1] = in[i];
        }
        break;
    }
    case AHIST_S32S: {
        const LONG *in = dd->mixbuffer;
        for(i = 0; i < n * 2; i++) {
            out[i] = (WORD) (in[i] >> 16);
        }
        break;
    }
    case AHIST_M32S: {
        const LONG *in = dd->mixbuffer;
        for(i = 0; i < n; i++) {
            out[2 * i] = out[2 * i + 1] = (WORD) (in[i] >> 16);
        }
        break;
    }
    default:
        memset(out, 0, n * 2 * sizeof(WORD));
        break;
    }
    if(dd->volume < 0x10000) {
        for(i = 0; i < n * 2; i++) {
            out[i] = (WORD) (((LONG) out[i] * (LONG) dd->volume) >> 16);
        }
    }
    dd->fifofill += n;
}

/* Run the AHI mixer until the FIFO holds at least frames stereo frames. */
static void bta_fill(struct AHIAudioCtrlDrv *AudioCtrl, ULONG frames)
{
    struct DriverBase *AHIsubBase = (struct DriverBase *) dd->ahisubbase;

    while(dd->fifofill < frames && dd->fifofill + AudioCtrl->ahiac_BuffSamples <= BTA_FIFO_FRAMES) {
        CallHookPkt(AudioCtrl->ahiac_PlayerFunc, AudioCtrl, NULL);
        CallHookPkt(AudioCtrl->ahiac_MixerFunc, AudioCtrl, dd->mixbuffer);
        bta_take_mix(AudioCtrl);
    }
}

static void bta_consume(struct AHIAudioCtrlDrv *AudioCtrl, ULONG frames)
{
    if(frames > dd->fifofill) {
        frames = dd->fifofill;
    }
    memmove(dd->fifo, &dd->fifo[frames * 2], (dd->fifofill - frames) * 2 * sizeof(WORD));
    dd->fifofill -= frames;
}

/*
 * Produce one packet's worth of audio. When streaming it is encoded and
 * sent; otherwise it is discarded at the same pace so applications keep
 * running while the sink is (re)connecting.
 */
static void bta_tick(struct AHIAudioCtrlDrv *AudioCtrl, ULONG packet_frames)
{
    struct DriverBase *AHIsubBase = (struct DriverBase *) dd->ahisubbase;
    struct Library *BluetoothBase = dd->btbase;
    ULONG f, i;

    (void) AHIsubBase;
    bta_fill(AudioCtrl, packet_frames);
    if(!dd->encoderready || dd->source.state != BT_A2DP_STREAMING) {
        bta_consume(AudioCtrl, packet_frames);
        return;
    }
    if(dd->mediawritepending) {
        /* previous packet still queued: drop this one rather than stall */
        dd->packetsdropped++;
        bta_consume(AudioCtrl, packet_frames);
        return;
    }
    for(f = 0; f < dd->framesperpacket; f++) {
        const WORD *pcm = &dd->fifo[f * dd->samplesperframe * 2];
        const int16_t *in = (const int16_t *) pcm;

        if(dd->encoder.channels == 1) {
            for(i = 0; i < dd->samplesperframe; i++) {
                dd->framepcm[i] = (WORD) (((LONG) pcm[2 * i] + pcm[2 * i + 1]) / 2);
            }
            in = (const int16_t *) dd->framepcm;
        }
        if(bt_sbc_encode(&dd->encoder, in, &dd->sbcframes[f * dd->framelen], dd->framelen) != dd->framelen) {
            bta_consume(AudioCtrl, packet_frames);
            return;
        }
    }
    bta_consume(AudioCtrl, packet_frames);
    {
        size_t len = bt_a2dp_build_media_packet(&dd->packetizer, dd->sbcframes, dd->framelen,
                                                dd->framesperpacket, dd->samplesperframe,
                                                dd->packet, sizeof(dd->packet));
        if(len) {
            btSendChannel(dd->mediawrite, dd->packet, len);
            dd->mediawritepending = TRUE;
            dd->packetssent++;
        }
    }
}

/******************************************************************************
** Connection *****************************************************************
******************************************************************************/

static APTR bta_find_sink(struct AHIAudioCtrlDrv *AudioCtrl)
{
    struct DriverBase *AHIsubBase = (struct DriverBase *) dd->ahisubbase;
    struct BluetoothAudioBase *BluetoothAudioBase = (struct BluetoothAudioBase *) AHIsubBase;
    struct Library *BluetoothBase = dd->btbase;
    UBYTE addr[32];
    APTR bd = NULL;

    btLockReadBase();
    if(dd->address[0]) {
        bd = btFindDevice(NULL, BDA_AddressString, (IPTR) dd->address, TAG_END);
    } else if(GetVar(BTA_ENV_DEVICE, addr, sizeof(addr), GVF_GLOBAL_ONLY) > 0) {
        bd = btFindDevice(NULL, BDA_AddressString, (IPTR) addr, TAG_END);
    } else {
        for(bd = btFindDevice(NULL, TAG_END); bd; bd = btFindDevice(bd, TAG_END)) {
            if(btFindService(bd, NULL, BSVA_UUID16, BTA_AUDIO_SINK, TAG_END)) {
                break;
            }
        }
    }
    btUnlockBase();
    return bd;
}

/* Find the sink's signalling and transport endpoints, connecting and
   enumerating its services first when needed. */
static BOOL bta_find_endpoints(struct AHIAudioCtrlDrv *AudioCtrl)
{
    struct DriverBase *AHIsubBase = (struct DriverBase *) dd->ahisubbase;
    struct BluetoothAudioBase *BluetoothAudioBase = (struct BluetoothAudioBase *) AHIsubBase;
    struct Library *BluetoothBase = dd->btbase;
    ULONG tries;

    for(tries = 0; tries < BTA_CONNECT_TRIES; tries++) {
        APTR bsv;

        btLockReadBase();
        bsv = btFindService(dd->device, NULL, BSVA_UUID16, BTA_AUDIO_SINK, TAG_END);
        if(bsv) {
            dd->sigep = btFindEndpoint(bsv, NULL, BEA_PSM, BT_AVDTP_PSM, TAG_END);
            dd->mediaep = dd->sigep ? btFindEndpoint(bsv, dd->sigep, BEA_PSM, BT_AVDTP_PSM, TAG_END) : NULL;
        }
        btUnlockBase();
        if(dd->sigep && dd->mediaep) {
            return TRUE;
        }
        if(tries == 0) {
            btConnectDevice(dd->device);
            btEnumerateServices(dd->device);
        }
        Delay(25);
    }
    return FALSE;
}

static APTR bta_channel(struct AHIAudioCtrlDrv *AudioCtrl, APTR ep, UWORD request)
{
    struct Library *BluetoothBase = dd->btbase;
    APTR ch = btAllocChannel(dd->device, dd->port, ep);

    if(ch) {
        btSetAttrs(BGA_CHANNEL, ch, BCHA_AutoConnect, TRUE, TAG_END);
        btChannelSetup(ch, request, 0, 0);
    }
    return ch;
}

static void bta_post_sigread(struct AHIAudioCtrlDrv *AudioCtrl)
{
    struct Library *BluetoothBase = dd->btbase;

    if(dd->sigread && !dd->sigreadpending) {
        btSendChannel(dd->sigread, dd->sigbuf, sizeof(dd->sigbuf));
        dd->sigreadpending = TRUE;
    }
}

static void bta_handle_port(struct AHIAudioCtrlDrv *AudioCtrl)
{
    struct DriverBase *AHIsubBase = (struct DriverBase *) dd->ahisubbase;
    struct BluetoothAudioBase *BluetoothAudioBase = (struct BluetoothAudioBase *) AHIsubBase;
    struct Library *BluetoothBase = dd->btbase;
    struct Message *msg;

    while((msg = GetMsg(dd->port))) {
        APTR ch = (APTR) msg;

        if(ch == dd->sigread) {
            LONG err = btGetChannelError(ch);
            ULONG actual = btGetChannelActual(ch);

            dd->sigreadpending = FALSE;
            if(!err && actual) {
                bt_a2dp_source_on_signal(&dd->source, dd->sigbuf, actual);
            } else if(err) {
                KPrintF("[bta] signalling read error %ld\n", (IPTR) err);
                Delay(10);
            }
            bta_post_sigread(AudioCtrl);
        } else if(ch == dd->mediawrite) {
            dd->mediawritepending = FALSE;
        } else if(ch == dd->mediaread) {
            /* nothing is expected on the transport channel; keep a read
               posted so the channel stays open */
            dd->mediareadpending = FALSE;
            if(btGetChannelError(ch) == 0) {
                btSendChannel(dd->mediaread, dd->mediarxbuf, sizeof(dd->mediarxbuf));
                dd->mediareadpending = TRUE;
            }
        }
    }
}

/* OPEN was accepted: bring the transport channel up and tell the source
   once it is open. */
static void bta_service_media(struct AHIAudioCtrlDrv *AudioCtrl)
{
    struct DriverBase *AHIsubBase = (struct DriverBase *) dd->ahisubbase;
    struct Library *BluetoothBase = dd->btbase;
    IPTR isopen = FALSE;

    if(!dd->wantmedia) {
        return;
    }
    if(!dd->mediaread) {
        dd->mediaread = bta_channel(AudioCtrl, dd->mediaep, BTPR_READ);
        dd->mediawrite = bta_channel(AudioCtrl, dd->mediaep, BTPR_WRITE);
        if(!dd->mediaread || !dd->mediawrite) {
            KPrintF("[bta] cannot allocate transport channels\n");
            dd->wantmedia = FALSE;
            return;
        }
        /* posting a read opens the L2CAP channel */
        btSendChannel(dd->mediaread, dd->mediarxbuf, sizeof(dd->mediarxbuf));
        dd->mediareadpending = TRUE;
    }
    btGetAttrs(BGA_ENDPOINT, dd->mediaep, BEA_IsOpen, (IPTR) &isopen, TAG_END);
    if(!isopen) {
        dd->openticks = 0;
        return;
    }
    /* The endpoint reports open once the connection is accepted, which can
       be before the sink has finished configuring the channel; a START
       sent then is rejected with BAD_STATE. Give it a moment. */
    if(++dd->openticks < BTA_MEDIA_SETTLE_TICKS) {
        return;
    }
    dd->wantmedia = FALSE;
    KPrintF("[bta] transport channel open\n");
    bt_a2dp_source_media_open(&dd->source);
}

static void bta_close(struct AHIAudioCtrlDrv *AudioCtrl)
{
    struct Library *BluetoothBase = dd->btbase;
    APTR *chans[4];
    ULONG i;

    chans[0] = &dd->sigread;
    chans[1] = &dd->sigwrite;
    chans[2] = &dd->mediaread;
    chans[3] = &dd->mediawrite;
    for(i = 0; i < 4; i++) {
        if(*chans[i]) {
            btAbortChannel(*chans[i]);
            btWaitChannel(*chans[i]);
            btFreeChannel(*chans[i]);
            *chans[i] = NULL;
        }
    }
    dd->sigreadpending = dd->mediareadpending = dd->mediawritepending = FALSE;
}

/******************************************************************************
** The slave process **********************************************************
******************************************************************************/

#undef SysBase

void Slave(struct ExecBase *SysBase);

#include <aros/asmcall.h>

AROS_UFH3(void, SlaveEntry,
          AROS_UFHA(STRPTR, argPtr, A0),
          AROS_UFHA(ULONG, argSize, D0),
          AROS_UFHA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT
    Slave(SysBase);
    AROS_USERFUNC_EXIT
}

void
Slave(struct ExecBase *SysBase)
{
    struct AHIAudioCtrlDrv *AudioCtrl;
    struct DriverBase      *AHIsubBase;
    struct Library         *BluetoothBase;
    struct Device          *TimerBase;
    struct timeval          next, now;
    ULONG                   packet_frames, period_us, prebuffer = 0;
    BOOL                    running, timerpending = FALSE;

    AudioCtrl  = (struct AHIAudioCtrlDrv *) FindTask(NULL)->tc_UserData;
    AHIsubBase = (struct DriverBase *) dd->ahisubbase;

    dd->slavesignal = AllocSignal(-1);
    dd->port = CreateMsgPort();
    dd->timerport = CreateMsgPort();
    dd->timereq = dd->timerport ? (struct timerequest *) CreateIORequest(dd->timerport, sizeof(struct timerequest)) : NULL;
    if(dd->timereq && !OpenDevice(TIMERNAME, UNIT_MICROHZ, (struct IORequest *) dd->timereq, 0)) {
        dd->timeropen = TRUE;
    }
    BluetoothBase = OpenLibrary("bluetooth.library", 1);
    dd->btbase = BluetoothBase;
    TimerBase = dd->timeropen ? dd->timereq->tr_node.io_Device : NULL;

    if(dd->slavesignal == -1 || !dd->port || !dd->timeropen) {
        goto out;
    }

    /* Tell the master we are alive. */
    Signal((struct Task *) dd->mastertask, 1L << dd->mastersignal);

    /* One packet's worth of audio per tick; frames are 128 samples for the
       usual 16 blocks x 8 subbands, 5 frames per packet. */
    packet_frames = 640;
    period_us = (ULONG) (((UQUAD) packet_frames * 1000000) / AudioCtrl->ahiac_MixFreq);

    bt_a2dp_source_init(&dd->source, &bta_ops, AudioCtrl);
    bt_a2dp_packetizer_init(&dd->packetizer, 0x41524f53);
    switch(AudioCtrl->ahiac_MixFreq) {
    case 16000: dd->source.prefer_frequency = BT_SBC_FREQ_16000; break;
    case 32000: dd->source.prefer_frequency = BT_SBC_FREQ_32000; break;
    case 48000: dd->source.prefer_frequency = BT_SBC_FREQ_48000; break;
    default:    dd->source.prefer_frequency = BT_SBC_FREQ_44100; break;
    }

    if(BluetoothBase) {
        dd->device = bta_find_sink(AudioCtrl);
        if(!dd->device) {
            KPrintF("[bta] no Bluetooth audio sink known (mode address '%s'; or set ENV:" BTA_ENV_DEVICE ")\n",
                    (IPTR) dd->address);
        } else if(!bta_find_endpoints(AudioCtrl)) {
            KPrintF("[bta] the device offers no A2DP sink endpoints\n");
        } else {
            dd->sigread = bta_channel(AudioCtrl, dd->sigep, BTPR_READ);
            dd->sigwrite = bta_channel(AudioCtrl, dd->sigep, BTPR_WRITE);
            if(dd->sigread && dd->sigwrite) {
                bta_post_sigread(AudioCtrl);
                KPrintF("[bta] connecting to the audio sink\n");
                bt_a2dp_source_connect(&dd->source);
            }
        }
    } else {
        KPrintF("[bta] bluetooth.library not available\n");
    }

    GetSysTime(&next);
    running = TRUE;
    while(running) {
        ULONG sigs;

        if(!timerpending) {
            /* schedule the next tick on an absolute timeline */
            next.tv_micro += period_us;
            while(next.tv_micro >= 1000000) {
                next.tv_micro -= 1000000;
                next.tv_secs++;
            }
            GetSysTime(&now);
            dd->timereq->tr_node.io_Command = TR_ADDREQUEST;
            if(CmpTime(&now, &next) <= 0) {
                /* already late: catch up immediately, but not forever */
                dd->timereq->tr_time.tv_secs = 0;
                dd->timereq->tr_time.tv_micro = 1;
                if((now.tv_secs - next.tv_secs) > 1) {
                    next = now;
                }
            } else {
                struct timeval d = next;
                SubTime(&d, &now);
                dd->timereq->tr_time = d;
            }
            SendIO((struct IORequest *) dd->timereq);
            timerpending = TRUE;
        }

        sigs = Wait((1L << dd->slavesignal) | (1L << dd->port->mp_SigBit) |
                    (1L << dd->timerport->mp_SigBit) | SIGBREAKF_CTRL_C);

        if(sigs & ((1L << dd->slavesignal) | SIGBREAKF_CTRL_C)) {
            running = FALSE;
        }
        if(BluetoothBase && (sigs & (1L << dd->port->mp_SigBit))) {
            bta_handle_port(AudioCtrl);
        }
        if(sigs & (1L << dd->timerport->mp_SigBit)) {
            if(GetMsg(dd->timerport)) {
                timerpending = FALSE;
                if(BluetoothBase) {
                    bta_service_media(AudioCtrl);
                }
                if(dd->encoderready && dd->source.state == BT_A2DP_STREAMING) {
                    packet_frames = dd->framesperpacket * dd->samplesperframe;
                    period_us = (ULONG) (((UQUAD) packet_frames * 1000000) / AudioCtrl->ahiac_MixFreq);
                    if(prebuffer < BTA_PREBUFFER) {
                        /* give the sink a little buffer up front */
                        prebuffer++;
                        bta_tick(AudioCtrl, packet_frames);
                    }
                } else {
                    prebuffer = 0;
                }
                bta_tick(AudioCtrl, packet_frames);
            }
        }
    }

    if(timerpending) {
        AbortIO((struct IORequest *) dd->timereq);
        WaitIO((struct IORequest *) dd->timereq);
    }
    if(BluetoothBase) {
        if(dd->source.state == BT_A2DP_STREAMING) {
            bt_a2dp_source_suspend(&dd->source);
        }
        KPrintF("[bta] stopped: %ld packets sent, %ld dropped\n",
                (IPTR) dd->packetssent, (IPTR) dd->packetsdropped);
        bta_close(AudioCtrl);
    }

out:
    if(BluetoothBase) {
        CloseLibrary(BluetoothBase);
        dd->btbase = NULL;
    }
    if(dd->timeropen) {
        CloseDevice((struct IORequest *) dd->timereq);
        dd->timeropen = FALSE;
    }
    if(dd->timereq) {
        DeleteIORequest((struct IORequest *) dd->timereq);
        dd->timereq = NULL;
    }
    if(dd->timerport) {
        DeleteMsgPort(dd->timerport);
        dd->timerport = NULL;
    }
    if(dd->port) {
        DeleteMsgPort(dd->port);
        dd->port = NULL;
    }
    if(dd->slavesignal != -1) {
        FreeSignal(dd->slavesignal);
    }
    dd->slavesignal = -1;

    Forbid();
    /* Tell the master we are dying; multitasking resumes once we are gone. */
    Signal((struct Task *) dd->mastertask, 1L << dd->mastersignal);
    dd->slavetask = NULL;
}
