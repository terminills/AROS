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
    /* bluetooth.library is opened by the play slave when audio starts, so
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

    CloseLibrary((struct Library *) DOSBase);
}
