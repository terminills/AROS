/*
 * WPA Supplicant - Layer2 packet handling for SANA-II devices
 * Copyright (c) 2003-2005, Jouni Malinen <j@w1.fi>
 * Copyright (c) 2026, Neil Cafferkey
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 *
 * The SANA-II driver backend already owns the device and receives EAPOL
 * frames on its CMD_READ request, reporting them with EVENT_EAPOL_RX. This
 * shim only has to hand outgoing EAPOL frames to that backend: there is no
 * socket, so nothing is registered with the event loop.
 */

#include "includes.h"

#include "common.h"
#include "eloop.h"
#include "l2_packet.h"
#include "drivers/driver_sana2.h"


struct l2_packet_data {
	char ifname[64];
	u8 own_addr[ETH_ALEN];
	void (*rx_callback)(void *ctx, const u8 *src_addr,
			    const u8 *buf, size_t len);
	void *rx_callback_ctx;
	int l2_hdr;
};


int l2_packet_get_own_addr(struct l2_packet_data *l2, u8 *addr)
{
	os_memcpy(addr, l2->own_addr, ETH_ALEN);
	return 0;
}


int l2_packet_send(struct l2_packet_data *l2, const u8 *dst_addr, u16 proto,
		   const u8 *buf, size_t len)
{
	if (l2 == NULL)
		return -1;

	if (l2->l2_hdr) {
		const struct l2_ethhdr *eth;

		if (len < sizeof(*eth))
			return -1;
		eth = (const struct l2_ethhdr *) buf;
		return wpa_driver_sana2_l2_send(l2->ifname, eth->h_dest,
						be_to_host16(eth->h_proto),
						buf + sizeof(*eth),
						len - sizeof(*eth));
	}

	return wpa_driver_sana2_l2_send(l2->ifname, dst_addr, proto, buf, len);
}


struct l2_packet_data * l2_packet_init(
	const char *ifname, const u8 *own_addr, unsigned short protocol,
	void (*rx_callback)(void *ctx, const u8 *src_addr,
			    const u8 *buf, size_t len),
	void *rx_callback_ctx, int l2_hdr)
{
	struct l2_packet_data *l2;

	l2 = os_zalloc(sizeof(struct l2_packet_data));
	if (l2 == NULL)
		return NULL;
	os_strlcpy(l2->ifname, ifname, sizeof(l2->ifname));
	l2->rx_callback = rx_callback;
	l2->rx_callback_ctx = rx_callback_ctx;
	l2->l2_hdr = l2_hdr;
	if (own_addr)
		os_memcpy(l2->own_addr, own_addr, ETH_ALEN);

	return l2;
}


struct l2_packet_data * l2_packet_init_bridge(
	const char *br_ifname, const char *ifname, const u8 *own_addr,
	unsigned short protocol,
	void (*rx_callback)(void *ctx, const u8 *src_addr,
			    const u8 *buf, size_t len),
	void *rx_callback_ctx, int l2_hdr)
{
	return l2_packet_init(br_ifname, own_addr, protocol, rx_callback,
			      rx_callback_ctx, l2_hdr);
}


void l2_packet_deinit(struct l2_packet_data *l2)
{
	if (l2 == NULL)
		return;

	os_free(l2);
}


int l2_packet_get_ip_addr(struct l2_packet_data *l2, char *buf, size_t len)
{
	return -1;
}


void l2_packet_notify_auth_start(struct l2_packet_data *l2)
{
}


int l2_packet_set_packet_filter(struct l2_packet_data *l2,
				enum l2_packet_filter_type type)
{
	return -1;
}
