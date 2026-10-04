/*
** BTAudioModes -- adds one AHI audio mode per paired Bluetooth audio sink
** (speaker, headphones), named after the device, so programs and the AHI
** preferences can choose between sinks by name. The mode carries the
** device address for the bluetooth.audio driver. With WATCH it keeps
** running and adds or removes modes as devices are paired or unpaired.
*/

#include <aros/isoascii.h>

#include <stdio.h>
#include <string.h>

#include <exec/exec.h>
#include <dos/dosextens.h>
#include <devices/ahi.h>
#include <libraries/bluetooth.h>
#include <proto/bluetooth.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/ahi.h>

#define ARGS_WATCH   0
#define ARGS_QUIET   1
#define ARGS_SIZEOF  2

static const char *template = "WATCH/S,QUIET/S";
const char *version = "$VER: BTAudioModes 1.0 (02.10.2026) " ISOASCII_COPYRIGHT " The AROS Development Team";
static IPTR ArgsArray[ARGS_SIZEOF];

struct Library *BluetoothBase = NULL;
struct Library *AHIBase = NULL;

#define AUDIO_SINK_UUID16   0x110b
#define MODE_ID_BASE        0x00460000
/* IDs below this are the static modes of the driver's mode file. */
#define MODE_ID_FIRST_DEVICE 0x0100
#define MAX_MODES           32

/* AHIDB_Data of a device mode, read by bluetooth.audio's AllocAudio. The
   first ULONG is the size of the whole block, as AHI requires. */
struct BTAudioModeData
{
    ULONG bmd_Size;
    UBYTE bmd_Address[18];  /* "xx:xx:xx:xx:xx:xx" */
};

static ULONG added[MAX_MODES];
static ULONG addedcount;

/* CRC-16/CCITT of the address string: a stable mode ID per device, so a
   choice saved in the AHI preferences keeps naming the same sink. */
static ULONG ModeIDFor(CONST_STRPTR addr)
{
    UWORD crc = 0xffff;
    const UBYTE *p;

    for(p = (const UBYTE *) addr; *p; p++)
    {
        UBYTE c = *p;
        int b;

        if(c >= 'A' && c <= 'Z')
        {
            c += 'a' - 'A';
        }
        crc ^= (UWORD) (c << 8);
        for(b = 0; b < 8; b++)
        {
            crc = (crc & 0x8000) ? (UWORD) ((crc << 1) ^ 0x1021) : (UWORD) (crc << 1);
        }
    }
    if(crc < MODE_ID_FIRST_DEVICE)
    {
        crc += MODE_ID_FIRST_DEVICE;
    }
    return MODE_ID_BASE | crc;
}

static BOOL IsAdded(ULONG id)
{
    ULONG i;

    for(i = 0; i < addedcount; i++)
    {
        if(added[i] == id)
        {
            return TRUE;
        }
    }
    return FALSE;
}

/* (Re)register the modes of all paired audio sinks. With present set, also
   remove modes this run added for devices that are no longer paired. */
static void Scan(BOOL quiet)
{
    ULONG seen[MAX_MODES];
    ULONG seencount = 0;
    APTR bd;
    ULONG i;

    btLockReadBase();
    for(bd = btFindDevice(NULL, BDA_IsRegistered, TRUE, TAG_END); bd && seencount < MAX_MODES;
        bd = btFindDevice(bd, BDA_IsRegistered, TRUE, TAG_END))
    {
        STRPTR name = NULL, addr = NULL;
        struct BTAudioModeData data;
        UBYTE label[64];
        ULONG id;
        ULONG ok;

        if(!btFindService(bd, NULL, BSVA_UUID16, AUDIO_SINK_UUID16, TAG_END))
        {
            continue;
        }
        btGetAttrs(BGA_DEVICE, bd, BDA_Name, &name, BDA_AddressString, &addr, TAG_END);
        if(!addr)
        {
            continue;
        }
        id = ModeIDFor(addr);
        seen[seencount++] = id;

        memset(&data, 0, sizeof(data));
        data.bmd_Size = sizeof(data);
        strncpy((char *) data.bmd_Address, (const char *) addr, sizeof(data.bmd_Address) - 1);
        /* Named like usbaudio.class modes: the device first. */
        snprintf((char *) label, sizeof(label), "%.20s: 16 bit stereo++",
                 (name && *name) ? (const char *) name : (const char *) addr);
        {
            struct TagItem tags[] = {
                { AHIDB_AudioID,   id },
                { AHIDB_Driver,    (IPTR) "bluetooth" },
                { AHIDB_Name,      (IPTR) label },
                { AHIDB_Volume,    TRUE },
                { AHIDB_Panning,   TRUE },
                { AHIDB_Stereo,    TRUE },
                { AHIDB_HiFi,      FALSE },
                { AHIDB_MultTable, FALSE },
                /* copied into the audio database, so it outlives this command */
                { AHIDB_Data,      (IPTR) &data },
                { TAG_DONE,        0 }
            };
            ok = AHI_AddAudioMode(tags);
        }
        if(ok)
        {
            if(!IsAdded(id) && addedcount < MAX_MODES)
            {
                added[addedcount++] = id;
                if(!quiet)
                {
                    Printf("Added audio mode 0x%08lx: %s (%s)\n", id, label, addr);
                }
            }
        }
        else if(!quiet)
        {
            Printf("Could not add an audio mode for %s\n", addr);
        }
    }
    btUnlockBase();

    /* drop modes of sinks that are no longer paired */
    for(i = 0; i < addedcount; )
    {
        ULONG j;
        BOOL still = FALSE;

        for(j = 0; j < seencount; j++)
        {
            if(seen[j] == added[i])
            {
                still = TRUE;
                break;
            }
        }
        if(still)
        {
            i++;
            continue;
        }
        AHI_RemoveAudioMode(added[i]);
        if(!quiet)
        {
            Printf("Removed audio mode 0x%08lx\n", added[i]);
        }
        added[i] = added[--addedcount];
    }
}

int main(int argc, char *argv[])
{
    struct RDArgs *rda;
    struct MsgPort *ahiport = NULL;
    struct AHIRequest *ahireq = NULL;
    LONG rc = RETURN_FAIL;
    BOOL quiet;

    if(!(rda = ReadArgs(template, ArgsArray, NULL)))
    {
        PrintFault(IoErr(), "BTAudioModes");
        return RETURN_FAIL;
    }
    quiet = ArgsArray[ARGS_QUIET] ? TRUE : FALSE;

    if(!(BluetoothBase = OpenLibrary("bluetooth.library", 1)))
    {
        PutStr("BTAudioModes: cannot open bluetooth.library\n");
        goto out;
    }
    if(!(ahiport = CreateMsgPort()) ||
       !(ahireq = (struct AHIRequest *) CreateIORequest(ahiport, sizeof(struct AHIRequest))))
    {
        goto out;
    }
    ahireq->ahir_Version = 4;
    if(OpenDevice((CONST_STRPTR) AHINAME, AHI_NO_UNIT, (struct IORequest *) ahireq, 0))
    {
        PutStr("BTAudioModes: cannot open ahi.device\n");
        DeleteIORequest((struct IORequest *) ahireq);
        ahireq = NULL;
        goto out;
    }
    AHIBase = (struct Library *) ahireq->ahir_Std.io_Device;

    Scan(quiet);
    rc = RETURN_OK;

    if(ArgsArray[ARGS_WATCH])
    {
        struct MsgPort *mp = CreateMsgPort();
        APTR eh = mp ? btAddEventHandler(mp, BEHMF_DEVICEREGISTERED | BEHMF_DEVICEUNREGISTERED |
                                             BEHMF_SERVICESCHG | BEHMF_DEVICEUPDATE) : NULL;
        if(eh)
        {
            BOOL running = TRUE;

            while(running)
            {
                ULONG sigs = Wait((1UL << mp->mp_SigBit) | SIGBREAKF_CTRL_C);
                struct Message *msg;
                BOOL rescan = FALSE;

                while((msg = GetMsg(mp)))
                {
                    rescan = TRUE;
                    ReplyMsg(msg);
                }
                if(rescan)
                {
                    Scan(quiet);
                }
                if(sigs & SIGBREAKF_CTRL_C)
                {
                    running = FALSE;
                }
            }
            btRemEventHandler(eh);
        }
        else
        {
            PutStr("BTAudioModes: cannot watch for devices\n");
            rc = RETURN_WARN;
        }
        if(mp)
        {
            DeleteMsgPort(mp);
        }
    }

out:
    if(ahireq)
    {
        CloseDevice((struct IORequest *) ahireq);
        DeleteIORequest((struct IORequest *) ahireq);
    }
    if(ahiport)
    {
        DeleteMsgPort(ahiport);
    }
    if(BluetoothBase)
    {
        CloseLibrary(BluetoothBase);
    }
    FreeArgs(rda);
    return rc;
}
