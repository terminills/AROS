
/*
 *  Bluetooth A2DP audio driver: the mixed output is SBC encoded and sent
 *  to a Bluetooth audio sink (speaker, headphones) over AVDTP. Recording
 *  is not supported.
 */

#include <config.h>

#include <devices/ahi.h>
#include <dos/dostags.h>
#include <exec/memory.h>
#include <libraries/ahi_sub.h>
#include <proto/ahi_sub.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include <stddef.h>

#include "library.h"
#include "DriverData.h"

#define dd ((struct BluetoothAudioData*) AudioCtrl->ahiac_DriverData)

void
SlaveEntry(void);

#ifdef PROCGW
PROCGW(static, void,  slaveentry, SlaveEntry);
#else
#define slaveentry SlaveEntry
#endif

/*  There is probably no reason to support all these frequencies. If,
 *  for example, your hardware is locked at 48 kHz, it's ok to only
 *  present one single mixing/recording frequency to the user. If your
 *  hardware has internal resamples and accept any frequency, select a
 *  few common ones.
 */

static const LONG frequencies[] = {
    16000,
    32000,
    44100,    // CD; supported by every A2DP sink
    48000     // supported by every A2DP sink
};

#define FREQUENCIES (sizeof frequencies / sizeof frequencies[ 0 ])

/******************************************************************************
** AHIsub_AllocAudio **********************************************************
******************************************************************************/

ULONG
_AHIsub_AllocAudio(struct TagItem         *taglist,
                   struct AHIAudioCtrlDrv *AudioCtrl,
                   struct DriverBase      *AHIsubBase)
{
    struct BluetoothAudioBase *BluetoothAudioBase = (struct BluetoothAudioBase *) AHIsubBase;

    // NOTE! A Real sound card driver would allocate *and lock* the
    // audio hardware here. If this function gets called a second time,
    // and the hardware is not capable of handling several audio streams
    // at the same time, return AHISF_ERROR now!

    AudioCtrl->ahiac_DriverData = AllocVec(sizeof(struct BluetoothAudioData),
                                           MEMF_CLEAR | MEMF_PUBLIC);

    if(dd != NULL) {
        dd->slavesignal      = -1;
        dd->mastersignal     = AllocSignal(-1);
        dd->mastertask       = (struct Process *) FindTask(NULL);
        dd->ahisubbase       = BluetoothAudioBase;
        dd->volume           = 0x10000;
        {
            /* A per-device mode names its sink; the static modes do not. */
            struct BTAudioModeData *bmd = (struct BTAudioModeData *) GetTagData(AHIDB_Data, 0, taglist);

            if(bmd != NULL && bmd->bmd_Size >= sizeof(struct BTAudioModeData)) {
                CopyMem(bmd->bmd_Address, dd->address, sizeof(dd->address) - 1);
            }
        }
    } else {
        return AHISF_ERROR;
    }

    if(dd->mastersignal == -1) {
        return AHISF_ERROR;
    }

    // 16 bit mixing only: SBC takes 16 bit samples.
    return (AHISF_KNOWSTEREO |
            AHISF_MIXING |
            AHISF_TIMING);
}


/******************************************************************************
** AHIsub_FreeAudio ***********************************************************
******************************************************************************/

void
_AHIsub_FreeAudio(struct AHIAudioCtrlDrv *AudioCtrl,
                  struct DriverBase      *AHIsubBase)
{
    if(AudioCtrl->ahiac_DriverData != NULL) {
        FreeSignal(dd->mastersignal);
        FreeVec(AudioCtrl->ahiac_DriverData);
        AudioCtrl->ahiac_DriverData = NULL;
    }
}


/******************************************************************************
** AHIsub_Disable *************************************************************
******************************************************************************/

void
_AHIsub_Disable(struct AHIAudioCtrlDrv *AudioCtrl,
                struct DriverBase      *AHIsubBase)
{
    // V6 drivers do not have to preserve all registers

    Forbid();
}


/******************************************************************************
** AHIsub_Enable **************************************************************
******************************************************************************/

void
_AHIsub_Enable(struct AHIAudioCtrlDrv *AudioCtrl,
               struct DriverBase      *AHIsubBase)
{
    // V6 drivers do not have to preserve all registers

    Permit();
}


/******************************************************************************
** AHIsub_Start ***************************************************************
******************************************************************************/

ULONG
_AHIsub_Start(ULONG                   flags,
              struct AHIAudioCtrlDrv *AudioCtrl,
              struct DriverBase      *AHIsubBase)
{
    struct BluetoothAudioBase *BluetoothAudioBase = (struct BluetoothAudioBase *) AHIsubBase;

    AHIsub_Stop(flags, AudioCtrl);

    if(flags & AHISF_PLAY) {
        struct TagItem proctags[] = {
            { NP_Entry, (IPTR) &slaveentry },
            { NP_Name, (IPTR) LibName     },
            { NP_Priority,  -1                  },
            { TAG_DONE,     0                   }
        };

        dd->mixbuffer = AllocVec(AudioCtrl->ahiac_BuffSize,
                                 MEMF_ANY | MEMF_PUBLIC);

        if(dd->mixbuffer == NULL) return AHIE_NOMEM;

        Forbid();

        dd->slavetask = CreateNewProc(proctags);

        if(dd->slavetask != NULL) {
            dd->slavetask->pr_Task.tc_UserData = AudioCtrl;
        }

        Permit();

        if(dd->slavetask != NULL) {
            Wait(1L << dd->mastersignal);    // Wait for slave to come alive

            if(dd->slavetask == NULL) {      // Is slave alive or dead?
                return AHIE_UNKNOWN;
            }
        } else {
            return AHIE_NOMEM;                 // Well, out of memory or whatever...
        }
    }

    if(flags & AHISF_RECORD) {
        return AHIE_UNKNOWN;
    }

    return AHIE_OK;
}


/******************************************************************************
** AHIsub_Update **************************************************************
******************************************************************************/

void
_AHIsub_Update(ULONG                   flags,
               struct AHIAudioCtrlDrv *AudioCtrl,
               struct DriverBase      *AHIsubBase)
{
    // Empty function
}


/******************************************************************************
** AHIsub_Stop ****************************************************************
******************************************************************************/

void
_AHIsub_Stop(ULONG                   flags,
             struct AHIAudioCtrlDrv *AudioCtrl,
             struct DriverBase      *AHIsubBase)
{
    if(flags & AHISF_PLAY) {
        if(dd->slavetask != NULL) {
            if(dd->slavesignal != -1) {
                Signal((struct Task *) dd->slavetask,
                       1L << dd->slavesignal);          // Kill him!
            }

            Wait(1L << dd->mastersignal);              // Wait for slave to die
        }

        FreeVec(dd->mixbuffer);
        dd->mixbuffer = NULL;
    }

    if(flags & AHISF_RECORD) {
        // Do nothing
    }
}


/******************************************************************************
** AHIsub_GetAttr *************************************************************
******************************************************************************/

IPTR
_AHIsub_GetAttr(ULONG                   attribute,
                LONG                    argument,
                IPTR                    def,
                struct TagItem         *taglist,
                struct AHIAudioCtrlDrv *AudioCtrl,
                struct DriverBase      *AHIsubBase)
{
    size_t i;

    switch(attribute) {
    case AHIDB_Bits:
        return 16;

    case AHIDB_Frequencies:
        return FREQUENCIES;

    case AHIDB_Frequency: // Index->Frequency
        return (LONG) frequencies[ argument ];

    case AHIDB_Index: // Frequency->Index
        if(argument <= frequencies[ 0 ]) {
            return 0;
        }

        if(argument >= frequencies[ FREQUENCIES - 1 ]) {
            return FREQUENCIES - 1;
        }

        for(i = 1; i < FREQUENCIES; i++) {
            if(frequencies[ i ] > argument) {
                if((argument - frequencies[ i - 1 ]) <
                        (frequencies[ i ] - argument)) {
                    return i - 1;
                } else {
                    return i;
                }
            }
        }

        return 0;  // Will not happen

    case AHIDB_Author:
        return (IPTR) "The AROS Development Team";

    case AHIDB_Copyright:
        return (IPTR) "(C) 2026 The AROS Development Team";

    case AHIDB_Version:
        return (IPTR) LibIDString;

    case AHIDB_Record:
        return FALSE;

    case AHIDB_Realtime:
        return TRUE;

    case AHIDB_MinOutputVolume:
        return 0x41;              // about -60 dB
    case AHIDB_MaxOutputVolume:
        return 0x10000;           // 0 dB: SBC has no headroom above full scale

    case AHIDB_Outputs:
        return 1;

    case AHIDB_Output:
        return (IPTR) "Bluetooth";

    default:
        return def;
    }
}


/******************************************************************************
** AHIsub_HardwareControl *****************************************************
******************************************************************************/

ULONG
_AHIsub_HardwareControl(ULONG                   attribute,
                        LONG                    argument,
                        struct AHIAudioCtrlDrv *AudioCtrl,
                        struct DriverBase      *AHIsubBase)
{
    if(AudioCtrl->ahiac_DriverData == NULL) {
        return 0;
    }
    switch(attribute) {
    case AHIC_OutputVolume:
        // applied to the mixed samples before SBC encoding
        dd->volume = (argument > 0x10000) ? 0x10000 : (ULONG) argument;
        return TRUE;

    case AHIC_OutputVolume_Query:
        return dd->volume;
    }
    return 0;
}
