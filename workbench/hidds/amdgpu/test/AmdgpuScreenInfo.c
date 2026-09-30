/* Copyright 2026 The AROS Development Team. SPDX-License-Identifier: MIT
 * Read-only native screen identity snapshot, JOB-0084. No driver loading,
 * rendering, preferences or mode changes. KVM capture size is not mode proof.
 */
#define DEBUG 1
#include <aros/debug.h>
#include <dos/dos.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

int main(void)
{
    struct Screen *screen = LockPubScreen(NULL);
    struct NameInfo name = {0};
    ULONG mode, bytes;
    if (!screen) {
        bug("[AmdgpuScreenInfo] FAIL no default public screen\n");
        return RETURN_FAIL;
    }
    mode = GetVPModeID(&screen->ViewPort);
    bytes = GetDisplayInfoData(NULL, (UBYTE *)&name, sizeof(name), DTAG_NAME, mode);
    name.Name[sizeof(name.Name) - 1] = 0;
    bug("[AmdgpuScreenInfo] v1 mode=%08lx name=%s name_bytes=%lu "
        "screen=%dx%d depth=%lu bitmap=%lux%lu\n",
        (unsigned long)mode, bytes ? (char *)name.Name : "<unavailable>",
        (unsigned long)bytes, screen->Width, screen->Height,
        (unsigned long)GetBitMapAttr(screen->RastPort.BitMap, BMA_DEPTH),
        (unsigned long)GetBitMapAttr(screen->RastPort.BitMap, BMA_WIDTH),
        (unsigned long)GetBitMapAttr(screen->RastPort.BitMap, BMA_HEIGHT));
    UnlockPubScreen(NULL, screen);
    bug("[AmdgpuScreenInfo] END read-only snapshot; no scanout or acceleration claim\n");
    return RETURN_OK;
}
