#ifndef AHI_Drivers_Bluetooth_DriverData_h
#define AHI_Drivers_Bluetooth_DriverData_h

#include <exec/libraries.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/dos.h>

#include <btcore/sbc.h>
#include <btcore/a2dp_source.h>

#include "DriverBase.h"

struct BluetoothAudioBase {
    struct DriverBase driverbase;
    struct DosLibrary *dosbase;
};

#define DRIVERBASE_SIZEOF (sizeof (struct BluetoothAudioBase))

#define DOSBase (*(struct DosLibrary**) &BluetoothAudioBase->dosbase)

/* PCM waiting to be encoded: interleaved stereo, 16 bit. */
#define BTA_FIFO_FRAMES   8192
/* SBC frames per media packet; 5 frames of 119 bytes plus headers stay
   below the default 672 byte L2CAP MTU. */
#define BTA_MAX_FRAMES    15

struct BluetoothAudioData {
    struct DriverData   driverdata;
    BYTE                mastersignal;
    BYTE                slavesignal;
    struct Process     *mastertask;
    struct Process     *slavetask;
    struct BluetoothAudioBase *ahisubbase;
    APTR                mixbuffer;
    UBYTE               address[18];   /* sink chosen by the audio mode, "" = default */
    ULONG               volume;        /* output volume, Fixed (0x10000 = 0 dB) */

    /* owned by the slave process */
    struct Library     *btbase;
    struct MsgPort     *port;          /* channel replies */
    struct MsgPort     *timerport;
    struct timerequest *timereq;
    BOOL                timeropen;
    APTR                device;
    APTR                sigep;
    APTR                mediaep;
    APTR                sigread;
    APTR                sigwrite;
    APTR                mediaread;
    APTR                mediawrite;
    BOOL                sigreadpending;
    BOOL                mediareadpending;
    BOOL                mediawritepending;
    BOOL                wantmedia;     /* OPEN accepted: bring the transport channel up */
    ULONG               openticks;     /* ticks the transport channel has been open */
    UBYTE               sigbuf[1024];
    UBYTE               mediarxbuf[64];
    struct bt_a2dp_source source;
    struct bt_sbc_encoder encoder;
    struct bt_a2dp_packetizer packetizer;
    BOOL                encoderready;
    ULONG               framesperpacket;
    ULONG               samplesperframe;
    ULONG               framelen;
    UBYTE               packet[BTA_MAX_FRAMES * 512 + 16];
    UBYTE               sbcframes[BTA_MAX_FRAMES * 512];
    WORD                fifo[BTA_FIFO_FRAMES * 2];
    ULONG               fifofill;      /* in stereo frames */
    WORD                framepcm[16 * 8 * 2];
    ULONG               packetssent;
    ULONG               packetsdropped;
};

/* AHIDB_Data of a per-device audio mode (added by C:BTAudioModes): the
   first ULONG is the size of the block, as AHI requires. */
struct BTAudioModeData
{
    ULONG bmd_Size;
    UBYTE bmd_Address[18];  /* "xx:xx:xx:xx:xx:xx" */
};

#endif /* AHI_Drivers_Bluetooth_DriverData_h */
