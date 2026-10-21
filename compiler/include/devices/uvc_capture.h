/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef DEVICES_UVC_CAPTURE_H
#define DEVICES_UVC_CAPTURE_H
#include <exec/types.h>

/* Private UVC class protocol, dispatched through the existing usbDoMethodA
 * vector. No Poseidon or VHI library vector numbers change. */
#define UVCM_COUNT 0x55564300UL
#define UVCM_INFO  (UVCM_COUNT + 1)
#define UVCM_GRAB  (UVCM_COUNT + 2)
struct UVCCameraInfo {
    ULONG id;                 /* binding generation; never a borrowed pointer */
    ULONG width, height;
    char name[128];
};
/* INFO arguments: index, UVCCameraInfo*. GRAB: id, RGB24 buffer, capacity.
 * GRAB returns the byte count, or zero on timeout, unplug, or invalid frame. */
#endif
