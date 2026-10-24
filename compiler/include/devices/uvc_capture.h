/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef DEVICES_UVC_CAPTURE_H
#define DEVICES_UVC_CAPTURE_H
#include <exec/types.h>

/* Private UVC class protocol, dispatched through the existing usbDoMethodA
 * vector. No Poseidon or VHI library vector numbers change. */
#define UVCM_COUNT 0x55564300UL
#define UVCM_INFO  (UVCM_COUNT + 1)
#define UVCM_GRAB  (UVCM_COUNT + 2)
#define UVCM_MODECOUNT (UVCM_COUNT + 3)
#define UVCM_MODEINFO  (UVCM_COUNT + 4)
#define UVCM_GRABMODE  (UVCM_COUNT + 5)

#define UVC_MODE_YUY2 1
struct UVCModeInfo {
    ULONG width, height;
    ULONG format_index, frame_index;
    ULONG pixel_format;
    ULONG default_interval;   /* 100 ns units, descriptor default */
    ULONG capture_interval;   /* 100 ns units, proposed capture rate */
    ULONG max_frame_bytes;    /* descriptor buffer bound */
};
/* MODECOUNT: binding id. MODEINFO: binding id, zero-based mode index,
 * UVCModeInfo*, buffer size. Returns FALSE on stale id/index or short buffer.
 * Modes describe supported YUY2 descriptors, not guaranteed bandwidth or
 * successful negotiation. Existing INFO and GRAB layouts remain unchanged. */
struct UVCCameraInfo {
    ULONG id;                 /* binding generation; never a borrowed pointer */
    ULONG width, height;
    char name[128];
};
/* INFO arguments: index, UVCCameraInfo*. GRAB: id, RGB24 buffer, capacity.
 * GRABMODE: id, RGB24 buffer, capacity, zero-based advertised mode index.
 * The selected mode applies to this grab only, at its capture_interval.
 * No fallback to another mode occurs if negotiation fails.
 * GRAB and GRABMODE return bytes, or zero on invalid arguments, timeout,
 * unplug, negotiation failure or invalid frame. */
#endif
