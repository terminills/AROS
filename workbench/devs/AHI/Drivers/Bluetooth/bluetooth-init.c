#include <config.h>

#include "library.h"
#include "DriverData.h"

/******************************************************************************
** Custom driver init *********************************************************
******************************************************************************/

BOOL
DriverInit(struct DriverBase *AHIsubBase)
{
    struct BluetoothAudioBase *BluetoothAudioBase = (struct BluetoothAudioBase *) AHIsubBase;

    BluetoothAudioBase->dosbase = (struct DosLibrary *)OpenLibrary(DOSNAME, 37);
    if(BluetoothAudioBase->dosbase == NULL) {
        Req("Unable to open 'dos.library' version 37.\n");
        return FALSE;
    }
    BluetoothAudioBase->link = AllocVec(sizeof(struct BtaLink), MEMF_CLEAR | MEMF_PUBLIC);
    if(BluetoothAudioBase->link == NULL) {
        CloseLibrary((struct Library *) DOSBase);
        return FALSE;
    }
    InitSemaphore(&BluetoothAudioBase->link->lock);
    BluetoothAudioBase->link->wakesig = -1;
    /* bluetooth.library is opened by the link process when audio starts, so
       the modes stay available while no radio is attached. */
    return TRUE;
}

/******************************************************************************
** Custom driver clean-up *****************************************************
******************************************************************************/

VOID
DriverCleanup(struct DriverBase *AHIsubBase)
{
    struct BluetoothAudioBase *BluetoothAudioBase = (struct BluetoothAudioBase *) AHIsubBase;

    FreeVec(BluetoothAudioBase->link);
    CloseLibrary((struct Library *) DOSBase);
}
