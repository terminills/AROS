/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Link process of the Bluetooth A2DP audio driver: keeps the audio sink
    connected, runs AVDTP signalling, SBC encodes the output of the playing
    AudioCtrl and sends it as RTP packets on the AVDTP transport channel,
    paced by timer.device.
*/

#include <config.h>

#include <devices/ahi.h>
#include <devices/timer.h>
#include <dos/dostags.h>
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

#define dd(ctrl) ((struct BluetoothAudioData *) (ctrl)->ahiac_DriverData)

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
#define BTA_IDLE_SECS       300     /* close the link after this long without playback */
#define BTA_IDLE_PERIOD_US  100000  /* tick while nothing plays */

#define LINK_BASES(lk) \
    struct DriverBase *AHIsubBase = (lk)->ahisubbase; \
    struct BluetoothAudioBase *BluetoothAudioBase = (struct BluetoothAudioBase *) AHIsubBase; \
    struct Library *BluetoothBase = (lk)->btbase; \
    (void) AHIsubBase; (void) BluetoothAudioBase; (void) BluetoothBase

#include <aros/asmcall.h>

AROS_UFP3(void, SlaveEntry,
          AROS_UFPA(STRPTR, argPtr, A0),
          AROS_UFPA(ULONG, argSize, D0),
          AROS_UFPA(struct ExecBase *, SysBase, A6));

#ifdef PROCGW
PROCGW(static, void,  slaveentry, SlaveEntry);
#else
#define slaveentry SlaveEntry
#endif

/******************************************************************************
** A2DP source callbacks ******************************************************
******************************************************************************/

static bt_status_t bta_send_signal(void *context, const uint8_t *pdu, size_t len)
{
    struct BtaLink *lk = context;
    LINK_BASES(lk);

    return btDoChannel(lk->sigwrite, (APTR) pdu, len) ? BT_ERR_IO : BT_OK;
}

static void bta_open_media(void *context)
{
    struct BtaLink *lk = context;

    lk->wantmedia = TRUE;
}

static void bta_state_changed(void *context, enum bt_a2dp_source_state state)
{
    struct BtaLink *lk = context;
    LINK_BASES(lk);

    KPrintF("[bta] state %ld\n", (IPTR) state);
    if(state == BT_A2DP_STREAMING) {
        struct bt_sbc_config *c = &lk->source.config;
        size_t mtu = BTA_MEDIA_MTU;

        if(bt_sbc_encoder_init(&lk->encoder, c)) {
            lk->framelen = bt_sbc_frame_length(c);
            lk->samplesperframe = (ULONG) c->blocks * c->subbands;
            lk->framesperpacket = bt_a2dp_frames_per_packet(mtu, lk->framelen);
            if(lk->framesperpacket > BTA_MAX_FRAMES) {
                lk->framesperpacket = BTA_MAX_FRAMES;
            }
            lk->encoderready = (lk->framesperpacket > 0);
            KPrintF("[bta] streaming: freq %ld mode %ld blocks %ld subbands %ld bitpool %ld, %ld frames of %ld bytes per packet\n",
                    (IPTR) c->frequency, (IPTR) c->mode, (IPTR) c->blocks, (IPTR) c->subbands,
                    (IPTR) c->bitpool, (IPTR) lk->framesperpacket, (IPTR) lk->framelen);
        }
    } else if(state == BT_A2DP_FAILED) {
        KPrintF("[bta] A2DP setup failed: signal %ld error 0x%02lx\n",
                (IPTR) lk->source.fail_signal, (IPTR) lk->source.fail_error);
        lk->encoderready = FALSE;
        lk->broken = TRUE;
    } else if(state != BT_A2DP_STARTING) {
        lk->encoderready = FALSE;
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
static void bta_take_mix(struct BtaLink *lk, struct AHIAudioCtrlDrv *AudioCtrl)
{
    ULONG n = AudioCtrl->ahiac_BuffSamples;
    ULONG room = BTA_FIFO_FRAMES - lk->fifofill;
    WORD *out = &lk->fifo[lk->fifofill * 2];
    ULONG i;

    if(n > room) {
        n = room; /* the sink fell behind: drop the newest samples */
    }
    switch(AudioCtrl->ahiac_BuffType) {
    case AHIST_S16S: {
        const WORD *in = dd(AudioCtrl)->mixbuffer;
        memcpy(out, in, n * 2 * sizeof(WORD));
        break;
    }
    case AHIST_M16S: {
        const WORD *in = dd(AudioCtrl)->mixbuffer;
        for(i = 0; i < n; i++) {
            out[2 * i] = out[2 * i + 1] = in[i];
        }
        break;
    }
    case AHIST_S32S: {
        const LONG *in = dd(AudioCtrl)->mixbuffer;
        for(i = 0; i < n * 2; i++) {
            out[i] = (WORD) (in[i] >> 16);
        }
        break;
    }
    case AHIST_M32S: {
        const LONG *in = dd(AudioCtrl)->mixbuffer;
        for(i = 0; i < n; i++) {
            out[2 * i] = out[2 * i + 1] = (WORD) (in[i] >> 16);
        }
        break;
    }
    default:
        memset(out, 0, n * 2 * sizeof(WORD));
        break;
    }
    if(dd(AudioCtrl)->volume < 0x10000) {
        for(i = 0; i < n * 2; i++) {
            out[i] = (WORD) (((LONG) out[i] * (LONG) dd(AudioCtrl)->volume) >> 16);
        }
    }
    lk->fifofill += n;
}

/* Run the AHI mixer until the FIFO holds at least frames stereo frames. */
static void bta_fill(struct BtaLink *lk, struct AHIAudioCtrlDrv *AudioCtrl, ULONG frames)
{
    LINK_BASES(lk);

    while(lk->fifofill < frames && lk->fifofill + AudioCtrl->ahiac_BuffSamples <= BTA_FIFO_FRAMES) {
        CallHookPkt(AudioCtrl->ahiac_PlayerFunc, AudioCtrl, NULL);
        CallHookPkt(AudioCtrl->ahiac_MixerFunc, AudioCtrl, dd(AudioCtrl)->mixbuffer);
        bta_take_mix(lk, AudioCtrl);
    }
}

static void bta_consume(struct BtaLink *lk, ULONG frames)
{
    if(frames > lk->fifofill) {
        frames = lk->fifofill;
    }
    memmove(lk->fifo, &lk->fifo[frames * 2], (lk->fifofill - frames) * 2 * sizeof(WORD));
    lk->fifofill -= frames;
}

/*
 * Produce one packet's worth of audio. When streaming it is encoded and
 * sent; otherwise it is discarded at the same pace so applications keep
 * running while the sink is (re)connecting.
 */
static void bta_tick(struct BtaLink *lk, struct AHIAudioCtrlDrv *AudioCtrl, ULONG packet_frames)
{
    LINK_BASES(lk);
    ULONG f, i;

    bta_fill(lk, AudioCtrl, packet_frames);
    if(!lk->encoderready || lk->source.state != BT_A2DP_STREAMING) {
        bta_consume(lk, packet_frames);
        return;
    }
    if(lk->mediawritepending) {
        /* previous packet still queued: drop this one rather than stall */
        lk->packetsdropped++;
        bta_consume(lk, packet_frames);
        return;
    }
    for(f = 0; f < lk->framesperpacket; f++) {
        const WORD *pcm = &lk->fifo[f * lk->samplesperframe * 2];
        const int16_t *in = (const int16_t *) pcm;

        if(lk->encoder.channels == 1) {
            for(i = 0; i < lk->samplesperframe; i++) {
                lk->framepcm[i] = (WORD) (((LONG) pcm[2 * i] + pcm[2 * i + 1]) / 2);
            }
            in = (const int16_t *) lk->framepcm;
        }
        if(bt_sbc_encode(&lk->encoder, in, &lk->sbcframes[f * lk->framelen], lk->framelen) != lk->framelen) {
            bta_consume(lk, packet_frames);
            return;
        }
    }
    bta_consume(lk, packet_frames);
    {
        size_t len = bt_a2dp_build_media_packet(&lk->packetizer, lk->sbcframes, lk->framelen,
                                                lk->framesperpacket, lk->samplesperframe,
                                                lk->packet, sizeof(lk->packet));
        if(len) {
            btSendChannel(lk->mediawrite, lk->packet, len);
            lk->mediawritepending = TRUE;
            lk->packetssent++;
        }
    }
}

/******************************************************************************
** Connection *****************************************************************
******************************************************************************/

static APTR bta_find_sink(struct BtaLink *lk)
{
    LINK_BASES(lk);
    UBYTE addr[32];
    APTR bd = NULL;

    btLockReadBase();
    if(lk->address[0]) {
        bd = btFindDevice(NULL, BDA_AddressString, (IPTR) lk->address, TAG_END);
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
static BOOL bta_find_endpoints(struct BtaLink *lk)
{
    LINK_BASES(lk);
    ULONG tries;

    for(tries = 0; tries < BTA_CONNECT_TRIES; tries++) {
        APTR bsv;

        btLockReadBase();
        bsv = btFindService(lk->device, NULL, BSVA_UUID16, BTA_AUDIO_SINK, TAG_END);
        if(bsv) {
            lk->sigep = btFindEndpoint(bsv, NULL, BEA_PSM, BT_AVDTP_PSM, TAG_END);
            lk->mediaep = lk->sigep ? btFindEndpoint(bsv, lk->sigep, BEA_PSM, BT_AVDTP_PSM, TAG_END) : NULL;
        }
        btUnlockBase();
        if(lk->sigep && lk->mediaep) {
            return TRUE;
        }
        if(tries == 0) {
            btConnectDevice(lk->device);
            btEnumerateServices(lk->device);
        }
        Delay(25);
    }
    return FALSE;
}

static APTR bta_channel(struct BtaLink *lk, APTR ep, UWORD request)
{
    LINK_BASES(lk);
    APTR ch = btAllocChannel(lk->device, lk->port, ep);

    if(ch) {
        btSetAttrs(BGA_CHANNEL, ch, BCHA_AutoConnect, TRUE, TAG_END);
        btChannelSetup(ch, request, 0, 0);
    }
    return ch;
}

static void bta_post_sigread(struct BtaLink *lk)
{
    LINK_BASES(lk);

    if(lk->sigread && !lk->sigreadpending) {
        btSendChannel(lk->sigread, lk->sigbuf, sizeof(lk->sigbuf));
        lk->sigreadpending = TRUE;
    }
}

static void bta_handle_port(struct BtaLink *lk)
{
    LINK_BASES(lk);
    struct Message *msg;

    while((msg = GetMsg(lk->port))) {
        APTR ch = (APTR) msg;

        if(ch == lk->sigread) {
            LONG err = btGetChannelError(ch);
            ULONG actual = btGetChannelActual(ch);

            lk->sigreadpending = FALSE;
            if(!err && actual) {
                bt_a2dp_source_on_signal(&lk->source, lk->sigbuf, actual);
                bta_post_sigread(lk);
            } else if(err) {
                /* the sink went away: the link reconnects on demand */
                KPrintF("[bta] signalling read error %ld\n", (IPTR) err);
                lk->broken = TRUE;
            } else {
                bta_post_sigread(lk);
            }
        } else if(ch == lk->mediawrite) {
            lk->mediawritepending = FALSE;
        } else if(ch == lk->mediaread) {
            /* nothing is expected on the transport channel; keep a read
               posted so the channel stays open */
            lk->mediareadpending = FALSE;
            if(btGetChannelError(ch) == 0) {
                btSendChannel(lk->mediaread, lk->mediarxbuf, sizeof(lk->mediarxbuf));
                lk->mediareadpending = TRUE;
            }
        }
    }
}

/* OPEN was accepted: bring the transport channel up and tell the source
   once it is open. */
static void bta_service_media(struct BtaLink *lk)
{
    LINK_BASES(lk);
    IPTR isopen = FALSE;

    if(!lk->wantmedia) {
        return;
    }
    if(!lk->mediaread) {
        lk->mediaread = bta_channel(lk, lk->mediaep, BTPR_READ);
        lk->mediawrite = bta_channel(lk, lk->mediaep, BTPR_WRITE);
        if(!lk->mediaread || !lk->mediawrite) {
            KPrintF("[bta] cannot allocate transport channels\n");
            lk->wantmedia = FALSE;
            return;
        }
        /* posting a read opens the L2CAP channel */
        btSendChannel(lk->mediaread, lk->mediarxbuf, sizeof(lk->mediarxbuf));
        lk->mediareadpending = TRUE;
    }
    btGetAttrs(BGA_ENDPOINT, lk->mediaep, BEA_IsOpen, (IPTR) &isopen, TAG_END);
    if(!isopen) {
        lk->openticks = 0;
        return;
    }
    /* The endpoint reports open once the connection is accepted, which can
       be before the sink has finished configuring the channel; a START
       sent then is rejected with BAD_STATE. Give it a moment. */
    if(++lk->openticks < BTA_MEDIA_SETTLE_TICKS) {
        return;
    }
    lk->wantmedia = FALSE;
    KPrintF("[bta] transport channel open\n");
    bt_a2dp_source_media_open(&lk->source);
}

static void bta_close(struct BtaLink *lk)
{
    LINK_BASES(lk);
    APTR *chans[4];
    ULONG i;

    chans[0] = &lk->sigread;
    chans[1] = &lk->sigwrite;
    chans[2] = &lk->mediaread;
    chans[3] = &lk->mediawrite;
    for(i = 0; i < 4; i++) {
        if(*chans[i]) {
            btAbortChannel(*chans[i]);
            btWaitChannel(*chans[i]);
            btFreeChannel(*chans[i]);
            *chans[i] = NULL;
        }
    }
    lk->sigreadpending = lk->mediareadpending = lk->mediawritepending = FALSE;
}

/* Connect to the sink and start A2DP signalling. */
static BOOL bta_begin_session(struct BtaLink *lk, const UBYTE *address, ULONG freq)
{
    LINK_BASES(lk);

    CopyMem((APTR) address, lk->address, sizeof(lk->address));
    lk->freq = freq;
    lk->broken = FALSE;
    lk->fifofill = 0;
    lk->packetssent = lk->packetsdropped = 0;

    bt_a2dp_source_init(&lk->source, &bta_ops, lk);
    bt_a2dp_packetizer_init(&lk->packetizer, 0x41524f53);
    switch(freq) {
    case 16000: lk->source.prefer_frequency = BT_SBC_FREQ_16000; break;
    case 32000: lk->source.prefer_frequency = BT_SBC_FREQ_32000; break;
    case 48000: lk->source.prefer_frequency = BT_SBC_FREQ_48000; break;
    default:    lk->source.prefer_frequency = BT_SBC_FREQ_44100; break;
    }

    lk->device = bta_find_sink(lk);
    if(!lk->device) {
        KPrintF("[bta] no Bluetooth audio sink known (mode address '%s'; or set ENV:" BTA_ENV_DEVICE ")\n",
                (IPTR) lk->address);
        return FALSE;
    }
    if(!bta_find_endpoints(lk)) {
        KPrintF("[bta] the device offers no A2DP sink endpoints\n");
        return FALSE;
    }
    lk->sigread = bta_channel(lk, lk->sigep, BTPR_READ);
    lk->sigwrite = bta_channel(lk, lk->sigep, BTPR_WRITE);
    if(!lk->sigread || !lk->sigwrite) {
        bta_close(lk);
        return FALSE;
    }
    bta_post_sigread(lk);
    KPrintF("[bta] connecting to the audio sink\n");
    bt_a2dp_source_connect(&lk->source);
    lk->session = TRUE;
    return TRUE;
}

static void bta_end_session(struct BtaLink *lk)
{
    LINK_BASES(lk);

    if(!lk->broken && lk->source.state == BT_A2DP_STREAMING) {
        bt_a2dp_source_suspend(&lk->source);
    }
    KPrintF("[bta] link closed: %ld packets sent, %ld dropped\n",
            (IPTR) lk->packetssent, (IPTR) lk->packetsdropped);
    bta_close(lk);
    lk->session = FALSE;
    lk->encoderready = FALSE;
    lk->wantmedia = FALSE;
    lk->openticks = 0;
    lk->fifofill = 0;
    lk->device = lk->sigep = lk->mediaep = NULL;
}

/******************************************************************************
** Called by Start() and Stop() ***********************************************
******************************************************************************/

/* Make AudioCtrl the one being played, starting the link process if it is
   not running. Returns FALSE if the process could not be started. */
BOOL bta_link_play(struct BluetoothAudioBase *BluetoothAudioBase, struct AHIAudioCtrlDrv *AudioCtrl)
{
    struct DriverBase *AHIsubBase = (struct DriverBase *) BluetoothAudioBase;
    struct BtaLink *lk = BluetoothAudioBase->link;
    BOOL ok = TRUE;

    if(lk == NULL) {
        return FALSE;
    }
    ObtainSemaphore(&lk->lock);
    CopyMem(dd(AudioCtrl)->address, lk->want_address, sizeof(lk->want_address));
    lk->want_freq = AudioCtrl->ahiac_MixFreq;
    lk->ctrl = AudioCtrl;
    if(lk->task != NULL) {
        Signal((struct Task *) lk->task, 1L << lk->wakesig);
        ReleaseSemaphore(&lk->lock);
        return TRUE;
    }

    {
        struct TagItem proctags[] = {
            { NP_Entry,    (IPTR) &slaveentry },
            { NP_Name,     (IPTR) LibName     },
            { NP_Priority, -1                 },
            { NP_UserData, (IPTR) lk          },
            { TAG_DONE,    0                  }
        };

        lk->ahisubbase = AHIsubBase;
        lk->creator = FindTask(NULL);
        lk->creatorsig = dd(AudioCtrl)->mastersignal;
        lk->quit = FALSE;
        SetSignal(0, 1L << lk->creatorsig);

        lk->task = CreateNewProc(proctags);
    }
    ReleaseSemaphore(&lk->lock);

    if(lk->task != NULL) {
        Wait(1L << lk->creatorsig);
    }

    ObtainSemaphore(&lk->lock);
    if(lk->task == NULL) {
        lk->ctrl = NULL;
        ok = FALSE;
    }
    ReleaseSemaphore(&lk->lock);
    return ok;
}

/* AudioCtrl stops playing. On return the link process no longer uses it,
   so its mix buffer may be freed. The sink stays connected. */
void bta_link_release(struct BluetoothAudioBase *BluetoothAudioBase, struct AHIAudioCtrlDrv *AudioCtrl)
{
    struct DriverBase *AHIsubBase = (struct DriverBase *) BluetoothAudioBase;
    struct BtaLink *lk = BluetoothAudioBase->link;

    if(lk == NULL) {
        return;
    }
    ObtainSemaphore(&lk->lock);
    if(lk->ctrl == AudioCtrl) {
        lk->ctrl = NULL;
        if(lk->task != NULL) {
            Signal((struct Task *) lk->task, 1L << lk->wakesig);
        }
    }
    ReleaseSemaphore(&lk->lock);
}

/******************************************************************************
** The link process ***********************************************************
******************************************************************************/

#undef SysBase

void Slave(struct ExecBase *SysBase);

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
    struct BtaLink         *lk = (struct BtaLink *) FindTask(NULL)->tc_UserData;
    struct DriverBase      *AHIsubBase = lk->ahisubbase;
    struct BluetoothAudioBase *BluetoothAudioBase = (struct BluetoothAudioBase *) AHIsubBase;
    struct Library         *BluetoothBase;
    struct Device          *TimerBase;
    struct AHIAudioCtrlDrv *ctrl, *lastctrl = NULL, *failedctrl = NULL;
    struct timeval          next, now;
    ULONG                   packet_frames = 640, period_us, prebuffer = 0, idle_us = 0;
    BOOL                    timerpending = FALSE, running = TRUE, started = FALSE;

    (void) BluetoothAudioBase;

    /* The process runs the library's code: keep it from being expunged. */
    Forbid();
    AHIsubBase->library.lib_OpenCnt++;
    Permit();

    lk->wakesig = AllocSignal(-1);
    lk->port = CreateMsgPort();
    lk->timerport = CreateMsgPort();
    lk->timereq = lk->timerport ? (struct timerequest *) CreateIORequest(lk->timerport, sizeof(struct timerequest)) : NULL;
    if(lk->timereq && !OpenDevice(TIMERNAME, UNIT_MICROHZ, (struct IORequest *) lk->timereq, 0)) {
        lk->timeropen = TRUE;
    }
    BluetoothBase = OpenLibrary("bluetooth.library", 1);
    lk->btbase = BluetoothBase;
    TimerBase = lk->timeropen ? lk->timereq->tr_node.io_Device : NULL;

    if(BluetoothBase == NULL) {
        KPrintF("[bta] bluetooth.library not available\n");
    }
    if(lk->wakesig == -1 || !lk->port || !lk->timeropen || !BluetoothBase) {
        ObtainSemaphore(&lk->lock);
        running = FALSE;
    } else {
        /* Tell the creator we are alive. */
        started = TRUE;
        Signal(lk->creator, 1L << lk->creatorsig);
    }

    GetSysTime(&next);
    while(running) {
        UBYTE address[sizeof(lk->want_address)];
        ULONG freq;
        BOOL quit;
        ULONG sigs;

        ObtainSemaphore(&lk->lock);
        ctrl = lk->ctrl;
        CopyMem(lk->want_address, address, sizeof(address));
        freq = lk->want_freq;
        quit = lk->quit;
        ReleaseSemaphore(&lk->lock);

        if(quit) {
            ObtainSemaphore(&lk->lock);
            break;
        }
        if(lk->session && (lk->broken ||
                           (ctrl && (strcmp((char *) address, (char *) lk->address) || freq != lk->freq)))) {
            if(lk->broken) {
                failedctrl = ctrl;
            }
            bta_end_session(lk);
        }
        if(ctrl && ctrl != failedctrl && !lk->session) {
            if(!bta_begin_session(lk, address, freq)) {
                failedctrl = ctrl;
                bta_close(lk);
            }
        }

        if(!timerpending) {
            if(ctrl || lk->wantmedia || (lk->session && lk->source.state < BT_A2DP_STREAMING)) {
                period_us = (ULONG) (((UQUAD) packet_frames * 1000000) / (lk->freq ? lk->freq : 44100));
            } else {
                period_us = BTA_IDLE_PERIOD_US;
            }
            /* schedule the next tick on an absolute timeline */
            next.tv_micro += period_us;
            while(next.tv_micro >= 1000000) {
                next.tv_micro -= 1000000;
                next.tv_secs++;
            }
            GetSysTime(&now);
            lk->timereq->tr_node.io_Command = TR_ADDREQUEST;
            if(CmpTime(&now, &next) <= 0) {
                /* already late: catch up immediately, but not forever */
                lk->timereq->tr_time.tv_secs = 0;
                lk->timereq->tr_time.tv_micro = 1;
                if((now.tv_secs - next.tv_secs) > 1) {
                    next = now;
                }
            } else {
                struct timeval d = next;
                SubTime(&d, &now);
                lk->timereq->tr_time = d;
            }
            SendIO((struct IORequest *) lk->timereq);
            timerpending = TRUE;
        }

        sigs = Wait((1L << lk->wakesig) | (1L << lk->port->mp_SigBit) |
                    (1L << lk->timerport->mp_SigBit) | SIGBREAKF_CTRL_C);

        if(sigs & SIGBREAKF_CTRL_C) {
            ObtainSemaphore(&lk->lock);
            break;
        }
        if(sigs & (1L << lk->port->mp_SigBit)) {
            bta_handle_port(lk);
        }
        if((sigs & (1L << lk->timerport->mp_SigBit)) && GetMsg(lk->timerport)) {
            timerpending = FALSE;
            if(lk->session) {
                bta_service_media(lk);
            }

            ObtainSemaphore(&lk->lock);
            ctrl = lk->ctrl;
            if(ctrl != lastctrl) {
                lk->fifofill = 0;
                prebuffer = 0;
                lastctrl = ctrl;
                if(ctrl != failedctrl) {
                    failedctrl = NULL;
                }
            }
            if(ctrl) {
                idle_us = 0;
                if(lk->session && lk->source.state == BT_A2DP_SUSPENDED) {
                    bt_a2dp_source_start(&lk->source);
                }
                if(lk->encoderready && lk->source.state == BT_A2DP_STREAMING) {
                    packet_frames = lk->framesperpacket * lk->samplesperframe;
                    if(prebuffer < BTA_PREBUFFER) {
                        /* give the sink a little buffer up front */
                        prebuffer++;
                        bta_tick(lk, ctrl, packet_frames);
                    }
                } else {
                    prebuffer = 0;
                }
                bta_tick(lk, ctrl, packet_frames);
            } else {
                if(lk->session && lk->source.state == BT_A2DP_STREAMING) {
                    bt_a2dp_source_suspend(&lk->source);
                }
                idle_us += period_us;
                if(idle_us >= (ULONG) BTA_IDLE_SECS * 1000000) {
                    /* keep the lock: nothing may start playing while the
                       link is being closed */
                    break;
                }
            }
            ReleaseSemaphore(&lk->lock);
        }
    }

    /* The lock is held from here on. */
    if(timerpending) {
        AbortIO((struct IORequest *) lk->timereq);
        WaitIO((struct IORequest *) lk->timereq);
    }
    if(lk->session) {
        bta_end_session(lk);
    }
    if(BluetoothBase) {
        CloseLibrary(BluetoothBase);
        lk->btbase = NULL;
    }
    if(lk->timeropen) {
        CloseDevice((struct IORequest *) lk->timereq);
        lk->timeropen = FALSE;
    }
    if(lk->timereq) {
        DeleteIORequest((struct IORequest *) lk->timereq);
        lk->timereq = NULL;
    }
    if(lk->timerport) {
        DeleteMsgPort(lk->timerport);
        lk->timerport = NULL;
    }
    if(lk->port) {
        DeleteMsgPort(lk->port);
        lk->port = NULL;
    }
    if(lk->wakesig != -1) {
        FreeSignal(lk->wakesig);
    }
    lk->wakesig = -1;

    Forbid();
    lk->task = NULL;
    if(!started) {
        /* the creator is waiting for the start-up handshake */
        Signal(lk->creator, 1L << lk->creatorsig);
    }
    ReleaseSemaphore(&lk->lock);
    AHIsubBase->library.lib_OpenCnt--;
    /* multitasking resumes once we are gone */
}
