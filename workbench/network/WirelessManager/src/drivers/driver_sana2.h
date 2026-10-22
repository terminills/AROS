/*
 * WPA Supplicant - AmigaOS/MorphOS/AROS SANA-II driver interface
 * Copyright (c) 2026, Neil Cafferkey
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#ifndef DRIVER_SANA2_H
#define DRIVER_SANA2_H

/* Used by l2_packet_sana2.c: send a frame on the device the backend opened
 * for @ifname. Returns 0 on success, -1 if no such interface is open or the
 * device refused the frame. */
int wpa_driver_sana2_l2_send(const char *ifname, const u8 *dst, u16 proto,
			     const u8 *buf, size_t len);

#endif /* DRIVER_SANA2_H */
