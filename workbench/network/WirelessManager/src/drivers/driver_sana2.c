/*
 * WPA Supplicant - AmigaOS/MorphOS/AROS SANA-II driver interface
 * Copyright (c) 2010-2026, Neil Cafferkey
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 *
 * The device is a hard-MAC SANA-II wireless device (it lists S2_GETNETWORKS
 * in NSCMD_DEVICEQUERY): it scans, authenticates and associates itself; the
 * supplicant selects the network, hands it the RSN element and the keys and
 * runs the EAPOL handshake over CMD_READ/CMD_WRITE frames of type 0x888e.
 *
 * Exchange with the device, in order:
 *   OpenDevice, NSCMD_DEVICEQUERY, S2_GETSTATIONADDRESS, S2_CONFIGINTERFACE
 *   (S2WERR_IS_CONFIGURED -> S2_ONLINE), CMD_READ type 0x888e queued,
 *   S2_ONEVENT(CONNECT|DISCONNECT|OFFLINE).
 *   scan:      S2_GETNETWORKS (async) -> EVENT_SCAN_RESULTS.
 *   associate: 4x S2_SETKEY(S2ENC_NONE/WEP, index 0..3), then S2_SETOPTIONS
 *              {SSID, BSSID, PortType, Channel, WPAInfo, AuthTypes}.
 *   S2EVENT_CONNECT -> S2_GETNETWORKINFO (WPAInfo = the IEs sent in the
 *              association request) -> EVENT_ASSOC.
 *   EAPOL:     CMD_WRITE type 0x888e to the AP; received frames arrive on
 *              the CMD_READ request -> EVENT_EAPOL_RX.
 *   keys:      S2_SETKEY cipher/index/key/RSC, DstAddr = AP for the pairwise
 *              key, broadcast for group keys.
 *   S2EVENT_DISCONNECT -> EVENT_DISASSOC.
 *   leave:     S2_SETOPTIONS{S2INFO_Disassociate}.
 *   keep-alive: an empty CMD_WRITE to the AP every NULL_INTERVAL seconds
 *              while associated.
 *
 * Management frame protection and SAE need more from the device; those
 * extensions are only used when the device advertises them (see the
 * S2ENC_BIP_CMAC128 / S2_EXTAUTHSTATUS notes below).
 */

#include "includes.h"

#include "common.h"
#include "common/ieee802_11_defs.h"
#include "common/ieee802_11_common.h"
#include "driver.h"
#include "driver_sana2.h"
#include "eloop.h"

#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/sana2.h>
#include <devices/sana2wireless.h>
#include <devices/newstyle.h>

#include <proto/exec.h>
#include <proto/utility.h>
#include <proto/dos.h>

#ifndef UPINT
#ifdef __AROS__
typedef IPTR UPINT;
typedef SIPTR PINT;
#else
typedef ULONG UPINT;
typedef LONG PINT;
#endif
#endif

/*
 * Extensions to <devices/sana2wireless.h> for PMF and SAE. A device that
 * implements them must list S2ENC_BIP_CMAC128 in S2_GETCRYPTTYPES, and
 * S2_EXTAUTHSTATUS in NSCMD_DEVICEQUERY; until the header carries them they
 * are defined here so that the supplicant can be built against the current
 * SDK.
 *
 * S2ENC_BIP_CMAC128: S2_SETKEY cipher for the IGTK (key index 4 or 5,
 *   DstAddr broadcast, ios2_StatData = the 6-byte IPN).
 * S2EVENT_EXTAUTH: the device wants the supplicant to run SAE
 *   authentication for the association started with S2_SETOPTIONS (the
 *   AuthTypes tag carried S2AUTH_SAE). Authentication frames from the AP are
 *   delivered on S2_READMGMT requests; the supplicant's are sent with
 *   S2_WRITEMGMT. Both carry a complete IEEE 802.11 management frame.
 * S2_EXTAUTHSTATUS: the supplicant's result, ios2_WireError = IEEE 802.11
 *   status code (0 = success, the device then associates), ios2_DstAddr =
 *   BSSID.
 * S2AUTH_SAE: bit in S2INFO_AuthTypes, as WPA_AUTH_ALG_SAE.
 */
#ifndef S2ENC_BIP_CMAC128
#define S2ENC_BIP_CMAC128 4
#endif
#ifndef S2EVENT_EXTAUTH
#define S2EVENT_EXTAUTH (1 << 11)
#endif
#ifndef S2_EXTAUTHSTATUS
#define S2_EXTAUTHSTATUS 0xc019
#endif
#define S2AUTH_OPEN   0x01
#define S2AUTH_SHARED 0x02
#define S2AUTH_LEAP   0x04
#define S2AUTH_SAE    0x10

#define ETH_MTU 1500
#define MAX_SSID_LEN 32

#define PUDDLE_SIZE 2000
#define MLME_REQ_COUNT 5
#define NULL_INTERVAL 30
#define MGMT_FRAME_SIZE 2048

static const char device_dir[] = "Networks/";

static const u8 broadcast_addr[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

#ifdef  __AROS__
static BOOL wpa_driver_sana2_buffer_hook(UBYTE *dest, UBYTE *src,
	ULONG size)
{
	CopyMem(src, dest, size);
	return TRUE;
}
#else
/* m68k code to do the same as above */
static const ULONG wpa_driver_sana2_buffer_hook[] =
	{0xC1492C78, 0x00044EAE, 0xFD907001, 0x4E754E71};
#endif

static const struct TagItem buffer_tags[] = {
	{S2_CopyToBuff, (UPINT)wpa_driver_sana2_buffer_hook},
	{S2_CopyFromBuff, (UPINT)wpa_driver_sana2_buffer_hook},
	{TAG_END, 0}
};

struct wpa_driver_sana2_data {
	struct wpa_driver_sana2_data *next;
	void *ctx;
	char ifname[64];
	char *device_name;
	struct MsgPort *port;
	struct IOSana2Req *request;	/* synchronous commands (DoIO) */
	struct IOSana2Req *scan_request;	/* S2_GETNETWORKS in flight */
	struct IOSana2Req *event_request;
	struct IOSana2Req *eapol_request;
	struct IOSana2Req *mlme_requests[MLME_REQ_COUNT];
	ULONG event_mask;
	u8 addr[ETH_ALEN];
	u8 bssid[ETH_ALEN];
	u8 ssid[MAX_SSID_LEN];
	size_t ssid_len;
	int freq;
	int device_opened;
	int hard_mac;
	int scanning;
	int online;
	int associated;
	/* device-advertised extensions */
	int has_bip;
	int has_extauth;
	/* the association in progress, for EVENT_EXTERNAL_AUTH */
	unsigned int key_mgmt_suite;
	u8 pending_bssid[ETH_ALEN];
	int have_pending_bssid;
};

/* l2_packet_sana2.c finds the backend for an interface here */
static struct wpa_driver_sana2_data *wpa_driver_sana2_list;

static void wpa_driver_sana2_stay_alive(void *eloop_ctx, void *timeout_ctx);


static int wpa_driver_sana2_chan_to_freq(int band, int chan)
{
	if (band == S2BAND_A) {
		if (chan >= 1 && chan <= 233 && chan != 2 && chan != 4)
			return 5000 + chan * 5;
		return 0;
	}
	if (chan == 14)
		return 2484;
	if (chan >= 1 && chan <= 13)
		return 2407 + chan * 5;
	/* No band given: anything past the 2.4 GHz channels is 5 GHz */
	if (chan >= 32 && chan <= 177)
		return 5000 + chan * 5;
	return 0;
}


static int wpa_driver_sana2_freq_to_chan(int freq)
{
	u8 chan;

	if (freq == 2484)
		return 14;
	if (freq >= 2412 && freq <= 2472)
		return (freq - 2407) / 5;
	if (freq >= 5000 && freq <= 5920)
		return (freq - 5000) / 5;
	if (ieee80211_freq_to_chan(freq, &chan) != NUM_HOSTAPD_MODES)
		return chan;
	return 0;
}


static int wpa_driver_sana2_get_ssid(void *priv, u8 *ssid)
{
	struct wpa_driver_sana2_data *drv = priv;

	os_memcpy(ssid, drv->ssid, drv->ssid_len);

	return drv->ssid_len;
}


static int wpa_driver_sana2_get_bssid(void *priv, u8 *bssid)
{
	struct wpa_driver_sana2_data *drv = priv;
	u8 *cur_bssid = NULL;
	struct TagItem *tag_list;
	struct IOSana2Req *request = drv->request;
	APTR pool;

	pool = CreatePool(MEMF_PUBLIC | MEMF_CLEAR, PUDDLE_SIZE, PUDDLE_SIZE);
	if (pool == NULL)
		return -1;

	request->ios2_Req.io_Command = S2_GETNETWORKINFO;
	request->ios2_Data = pool;
	if (DoIO((APTR)request) == 0) {
		tag_list = request->ios2_StatData;
		cur_bssid = (u8 *)GetTagData(S2INFO_BSSID, (UPINT)NULL,
			tag_list);
	}
	if (cur_bssid != NULL) {
		os_memcpy(bssid, cur_bssid, ETH_ALEN);
		os_memcpy(drv->bssid, cur_bssid, ETH_ALEN);
	}
	DeletePool(pool);

	return (cur_bssid != NULL) ? 0 : -1;
}


static int wpa_driver_sana2_scan(void *priv,
	struct wpa_driver_scan_params *params)
{
	struct wpa_driver_sana2_data *drv = priv;
	int err = 0;
	struct IOSana2Req *request = drv->scan_request;
	const u8 *ssid = NULL;
	size_t ssid_len = 0;
	TEXT *ssid_str = NULL;
	struct TagItem *tag_list;
	APTR pool;

	if (drv->scanning) {
		wpa_printf(MSG_DEBUG, "sana2: scan already in progress");
		return -EBUSY;
	}

	pool = CreatePool(MEMF_PUBLIC | MEMF_CLEAR, PUDDLE_SIZE, PUDDLE_SIZE);
	if (pool == NULL)
		return -1;

	/* The device takes one SSID for a directed scan; a wildcard scan
	 * sees broadcast SSIDs only */
	if (params->num_ssids > 0) {
		ssid = params->ssids[0].ssid;
		ssid_len = params->ssids[0].ssid_len;
	}
	if (ssid != NULL && ssid_len > 0) {
		ssid_str = AllocPooled(pool, ssid_len + 1);
		if (ssid_str != NULL) {
			CopyMem((APTR)ssid, ssid_str, ssid_len);
			ssid_str[ssid_len] = '\0';
		} else
			err = 1;
	}

	if (err == 0) {
		tag_list = AllocPooled(pool, sizeof(struct TagItem) * 2);
		if (tag_list == NULL)
			err = 1;
	}

	if (err == 0) {
		tag_list[0].ti_Tag = (ssid_str != NULL) ?
			S2INFO_SSID : TAG_IGNORE;
		tag_list[0].ti_Data = (UPINT)ssid_str;
		tag_list[1].ti_Tag = TAG_END;

		request->ios2_Req.io_Command = S2_GETNETWORKS;
		request->ios2_Req.io_Error = 0;
		request->ios2_Data = pool;
		request->ios2_StatData = tag_list;
		request->ios2_DataLength = 0;
		SendIO((APTR)request);
		drv->scanning = 1;
	} else
		DeletePool(pool);

	return err ? -1 : 0;
}


static struct wpa_scan_results * wpa_driver_sana2_get_scan_results(void *priv)
{
	struct wpa_driver_sana2_data *drv = priv;
	int err = 0;
	struct IOSana2Req *request = drv->scan_request;
	struct wpa_scan_results *results;
	size_t i, count;
	struct TagItem **tag_lists, *tag;
	TEXT *str;
	UPINT num, band;
	u8 *ie;
	u16 ie_len, ssid_len, *data;

	if (drv->scanning || request->ios2_Data == NULL)
		return NULL;

	count = (request->ios2_Req.io_Error == 0) ?
		request->ios2_DataLength : 0;
	tag_lists = request->ios2_StatData;

	results = os_zalloc(sizeof(struct wpa_scan_results));
	if (results == NULL) {
		err = 1;
	}

	if (err == 0 && count > 0) {
		results->res =
			os_calloc(count, sizeof(struct wpa_scan_res *));
		if (results->res == NULL)
			err = 1;
	}

	for (i = 0; i < count && err == 0; i++) {
		struct wpa_scan_res *res;
		data = (u16 *)GetTagData(S2INFO_InfoElements,
			(UPINT)NULL, tag_lists[i]);
		if (data != NULL)
			ie_len = data[0];
		else
			ie_len = MAX_SSID_LEN + 2;
		res = os_zalloc(sizeof(struct wpa_scan_res) + ie_len);
		if (res == NULL) {
			err = 1;
			break;
		}
		results->res[results->num++] = res;

		tag = FindTagItem(S2INFO_BSSID, tag_lists[i]);
		if (tag != NULL)
			os_memcpy(res->bssid, (void *)tag->ti_Data, ETH_ALEN);

		num = GetTagData(S2INFO_Channel, 0, tag_lists[i]);
		band = GetTagData(S2INFO_Band, (UPINT)-1, tag_lists[i]);
		res->freq = wpa_driver_sana2_chan_to_freq(band, num);

		res->beacon_int = GetTagData(S2INFO_BeaconInterval, 0,
			tag_lists[i]);
		res->caps = GetTagData(S2INFO_Capabilities, 0, tag_lists[i]);
		res->level = (LONG)GetTagData(S2INFO_Signal, 0, tag_lists[i]);
		res->noise = (LONG)GetTagData(S2INFO_Noise, 0, tag_lists[i]);
		res->flags = WPA_SCAN_LEVEL_DBM | WPA_SCAN_NOISE_INVALID;
		if (res->level > res->noise)
			res->qual = res->level - res->noise;
		res->age = 0;

		/* Copy all IEs if available, or make fake IEs containing
		 * just the SSID */
		ie = (u8 *)(res + 1);
		if (data != NULL) {
			res->ie_len = ie_len;
			os_memcpy(ie, data + 1, ie_len);
		} else {
			str = (TEXT *)GetTagData(S2INFO_SSID, (UPINT)NULL,
				tag_lists[i]);
			if (str != NULL) {
				ssid_len = strlen((char *)str);
				if (ssid_len > MAX_SSID_LEN)
					ssid_len = MAX_SSID_LEN;
				res->ie_len = ssid_len + 2;
				ie[0] = WLAN_EID_SSID;
				ie[1] = ssid_len;
				os_memcpy(ie + 2, str, ssid_len);
			}
		}
	}
	DeletePool(request->ios2_Data);
	request->ios2_Data = NULL;

	if (err != 0) {
		wpa_scan_results_free(results);
		results = NULL;
	}

	return results;
}


static int wpa_driver_sana2_cipher(enum wpa_alg alg)
{
	switch (alg) {
	case WPA_ALG_NONE:
		return S2ENC_NONE;
	case WPA_ALG_WEP:
		return S2ENC_WEP;
	case WPA_ALG_TKIP:
		return S2ENC_TKIP;
	case WPA_ALG_CCMP:
		return S2ENC_CCMP;
	case WPA_ALG_BIP_CMAC_128:
		return S2ENC_BIP_CMAC128;
	default:
		return -1;
	}
}


static int wpa_driver_sana2_set_key(void *priv,
	struct wpa_driver_set_key_params *params)
{
	struct wpa_driver_sana2_data *drv = priv;
	struct IOSana2Req *request = drv->request;
	int cipher = wpa_driver_sana2_cipher(params->alg);

	if (cipher < 0) {
		wpa_printf(MSG_INFO, "sana2: cipher %d is not supported by "
			   "SANA-II", params->alg);
		return -1;
	}
	if (cipher == S2ENC_BIP_CMAC128 && !drv->has_bip) {
		wpa_printf(MSG_INFO, "sana2: the device does not install "
			   "BIP keys");
		return -1;
	}
	/* Key indexes 4 and 5 are the IGTK's; a device without BIP has
	 * nothing there to clear */
	if (params->key_idx > 3 && !drv->has_bip)
		return 0;

	wpa_printf(MSG_DEBUG, "sana2: set_key alg %d idx %d len %zu %s",
		   params->alg, params->key_idx, params->key_len,
		   params->addr ? "with address" : "without address");

	request->ios2_Req.io_Command = S2_SETKEY;
	request->ios2_WireError = params->key_idx;
	request->ios2_PacketType = cipher;
	request->ios2_DataLength = params->key_len;
	request->ios2_Data = (u8 *)params->key;
	request->ios2_StatData = (u8 *)params->seq;
	/* Which key this is - pairwise or group - travels in the address:
	 * the peer for a pairwise key, broadcast or all-zero for a group key */
	if (params->addr != NULL)
		CopyMem((APTR)params->addr, request->ios2_DstAddr, ETH_ALEN);
	else
		os_memset(request->ios2_DstAddr, 0, ETH_ALEN);

	if (DoIO((APTR)request) != 0) {
		wpa_printf(MSG_INFO, "sana2: S2_SETKEY idx %d failed "
			   "(io_Error %d, WireError %lu)", params->key_idx,
			   request->ios2_Req.io_Error,
			   (unsigned long)request->ios2_WireError);
		return -1;
	}

	return 0;
}


static int wpa_driver_sana2_set_wep_keys(struct wpa_driver_sana2_data *drv,
	struct wpa_driver_associate_params *params)
{
	struct wpa_driver_set_key_params kp;
	int i;

	/* Set or clear the four WEP keys; this also wipes keys left from a
	 * previous association */
	for (i = 0; i < 4; i++) {
		os_memset(&kp, 0, sizeof(kp));
		kp.alg = params->wep_key_len[i] > 0 ?
			WPA_ALG_WEP : WPA_ALG_NONE;
		kp.addr = broadcast_addr;
		kp.key_idx = i;
		kp.set_tx = i == params->wep_tx_keyidx;
		kp.key = params->wep_key[i];
		kp.key_len = params->wep_key_len[i];
		kp.key_flag = kp.alg == WPA_ALG_NONE ? 0 :
			(kp.set_tx ? KEY_FLAG_GROUP_RX_TX_DEFAULT :
			 KEY_FLAG_GROUP_RX_TX);
		wpa_driver_sana2_set_key(drv, &kp);
	}

	return 0;
}


static int wpa_driver_sana2_associate(
	void *priv, struct wpa_driver_associate_params *params)
{
	struct wpa_driver_sana2_data *drv = priv;
	int err = 0;
	struct IOSana2Req *request = drv->request;
	struct TagItem *tag_list2 = NULL;
	char ssid[MAX_SSID_LEN + 1];
	UPINT auth_types = 0;
	int chan = wpa_driver_sana2_freq_to_chan(params->freq.freq);
	struct TagItem tag_list[] =
		{{S2INFO_SSID, (UPINT)ssid},
		{(params->bssid != NULL) ? S2INFO_BSSID : TAG_IGNORE,
			(UPINT)params->bssid},
		{S2INFO_PortType, (params->mode == IEEE80211_MODE_IBSS) ?
			S2PORT_ADHOC : S2PORT_MANAGED},
		{chan ? S2INFO_Channel : TAG_IGNORE, chan},
		{S2INFO_WPAInfo, (params->wpa_ie_len > 0) ?
			(UPINT)params->wpa_ie : (UPINT)NULL},
		{S2INFO_AuthTypes, 0},
		/* Only sent if the driver claimed the 4-way offload; when we
		 * run the handshake ourselves wpa_supplicant leaves these
		 * NULL. */
		{(params->passphrase != NULL) ? S2INFO_Passphrase : TAG_IGNORE,
			(UPINT)params->passphrase},
		{TAG_END, 0}};

	if (params->ssid_len > MAX_SSID_LEN)
		return -1;
	os_memcpy(ssid, params->ssid, params->ssid_len);
	ssid[params->ssid_len] = '\0';

	if (params->auth_alg & WPA_AUTH_ALG_OPEN)
		auth_types |= S2AUTH_OPEN;
	if (params->auth_alg & WPA_AUTH_ALG_SHARED)
		auth_types |= S2AUTH_SHARED;
	if (params->auth_alg & WPA_AUTH_ALG_LEAP)
		auth_types |= S2AUTH_LEAP;
	if ((params->auth_alg & WPA_AUTH_ALG_SAE) ||
	    wpa_key_mgmt_sae(params->key_mgmt_suite)) {
		if (!drv->has_extauth) {
			wpa_printf(MSG_INFO, "sana2: SAE needs the device's "
				   "external authentication support");
			return -1;
		}
		auth_types = S2AUTH_SAE;
	}
	if (auth_types == 0)
		auth_types = S2AUTH_OPEN;
	tag_list[5].ti_Data = auth_types;

	wpa_printf(MSG_DEBUG, "sana2: associate \"%s\" freq %d (channel %d) "
		   "auth %#lx%s, %zu-byte IE, key_mgmt %#x",
		   wpa_ssid_txt(params->ssid, params->ssid_len),
		   params->freq.freq, chan, (unsigned long)auth_types,
		   params->bssid ? " to the given BSSID" : "",
		   params->wpa_ie_len, params->key_mgmt_suite);

	tag_list2 = CloneTagItems(tag_list);
	if (tag_list2 == NULL)
		return -1;

	wpa_driver_sana2_set_wep_keys(drv, params);

	drv->key_mgmt_suite = params->key_mgmt_suite;
	if (params->bssid != NULL) {
		os_memcpy(drv->pending_bssid, params->bssid, ETH_ALEN);
		drv->have_pending_bssid = 1;
	} else
		drv->have_pending_bssid = 0;

	request->ios2_Req.io_Command = S2_SETOPTIONS;
	request->ios2_Data = tag_list2;
	if (DoIO((APTR)request) != 0) {
		wpa_printf(MSG_INFO, "sana2: S2_SETOPTIONS failed (io_Error "
			   "%d, WireError %lu)", request->ios2_Req.io_Error,
			   (unsigned long)request->ios2_WireError);
		err = -1;
	}
	FreeTagItems(tag_list2);

	if (err == 0) {
		os_memcpy(drv->ssid, params->ssid, params->ssid_len);
		drv->ssid_len = params->ssid_len;
		drv->freq = params->freq.freq;
	}

	return err;
}


static int wpa_driver_sana2_deauthenticate(void *priv, const u8 *addr,
	u16 reason_code)
{
	struct wpa_driver_sana2_data *drv = priv;
	struct IOSana2Req *request = drv->request;
	struct TagItem tag_list[] =
		{{S2INFO_Disassociate, reason_code ? reason_code :
			WLAN_REASON_DEAUTH_LEAVING},
		{TAG_END, 0}};

	wpa_printf(MSG_DEBUG, "sana2: leaving the network (reason %u)",
		   reason_code);

	eloop_cancel_timeout(wpa_driver_sana2_stay_alive, drv->ctx, drv);
	drv->associated = 0;

	request->ios2_Req.io_Command = S2_SETOPTIONS;
	request->ios2_Data = tag_list;
	if (DoIO((APTR)request) != 0)
		return -1;

	drv->ssid_len = 0;
	return 0;
}


static int wpa_driver_sana2_get_capa(void *priv, struct wpa_driver_capa *capa)
{
	struct wpa_driver_sana2_data *drv = priv;
	u8 *enc_list = NULL;
	int i;
	struct IOSana2Req *request = drv->request;
	APTR pool;

	os_memset(capa, 0, sizeof(*capa));

	drv->has_bip = 0;
	request->ios2_Req.io_Command = S2_GETCRYPTTYPES;
	pool = CreatePool(MEMF_PUBLIC | MEMF_CLEAR, PUDDLE_SIZE, PUDDLE_SIZE);
	request->ios2_Data = pool;
	if (pool != NULL && DoIO((APTR)request) == 0) {
		enc_list = request->ios2_StatData;
		for (i = 0; i < request->ios2_DataLength; i++)
		{
			switch (enc_list[i]) {
			case S2ENC_WEP:
				capa->enc |= WPA_DRIVER_CAPA_ENC_WEP40
					| WPA_DRIVER_CAPA_ENC_WEP104;
				break;
			case S2ENC_TKIP:
				capa->enc |= WPA_DRIVER_CAPA_ENC_TKIP;
				break;
			case S2ENC_CCMP:
				capa->enc |= WPA_DRIVER_CAPA_ENC_CCMP;
				break;
			case S2ENC_BIP_CMAC128:
				capa->enc |= WPA_DRIVER_CAPA_ENC_BIP;
				drv->has_bip = 1;
				break;
			}
		}
	} else
		capa->enc = WPA_DRIVER_CAPA_ENC_WEP40 |
			WPA_DRIVER_CAPA_ENC_WEP104 |
			WPA_DRIVER_CAPA_ENC_TKIP |
			WPA_DRIVER_CAPA_ENC_CCMP;
	DeletePool(pool);

	if (capa->enc & WPA_DRIVER_CAPA_ENC_TKIP) {
		capa->key_mgmt |= WPA_DRIVER_CAPA_KEY_MGMT_WPA |
			WPA_DRIVER_CAPA_KEY_MGMT_WPA_PSK;
	}
	if (capa->enc & WPA_DRIVER_CAPA_ENC_CCMP) {
		capa->key_mgmt |= WPA_DRIVER_CAPA_KEY_MGMT_WPA2 |
			WPA_DRIVER_CAPA_KEY_MGMT_WPA2_PSK |
			WPA_DRIVER_CAPA_KEY_MGMT_PSK_SHA256 |
			WPA_DRIVER_CAPA_KEY_MGMT_802_1X_SHA256;
	}
	for (i = 0; i < WPA_IF_MAX; i++)
		capa->key_mgmt_iftype[i] = capa->key_mgmt;

	capa->auth = WPA_DRIVER_AUTH_OPEN | WPA_DRIVER_AUTH_SHARED |
		WPA_DRIVER_AUTH_LEAP;

	/* The device runs the MLME; we are only told the outcome */
	capa->flags = 0;
	if (drv->has_extauth && drv->has_bip) {
		/* SAE as nl80211's "external authentication": the device
		 * starts it, the SAE exchange happens here over
		 * S2_READMGMT/S2_WRITEMGMT */
		capa->flags |= WPA_DRIVER_FLAGS_SAE;
		capa->key_mgmt |= WPA_DRIVER_CAPA_KEY_MGMT_SAE;
		for (i = 0; i < WPA_IF_MAX; i++)
			capa->key_mgmt_iftype[i] = capa->key_mgmt;
	}

	capa->max_scan_ssids = 1;
	capa->max_sched_scan_ssids = 0;
	capa->max_remain_on_chan = 0;
	capa->max_stations = 0;

	wpa_printf(MSG_DEBUG, "sana2: capabilities enc %#x key_mgmt %#x "
		   "flags %#llx (BIP %d, external auth %d)", capa->enc,
		   capa->key_mgmt, (unsigned long long)capa->flags,
		   drv->has_bip, drv->has_extauth);

	return 0;
}


static int wpa_driver_sana2_send_frame(struct wpa_driver_sana2_data *drv,
	const u8 *dest, u16 proto, const u8 *data, size_t data_len)
{
	struct IOSana2Req *request = drv->request;

	request->ios2_Req.io_Command = CMD_WRITE;
	request->ios2_Req.io_Flags = 0;
	CopyMem((APTR)dest, request->ios2_DstAddr, ETH_ALEN);
	request->ios2_PacketType = proto;
	request->ios2_DataLength = data_len;
	request->ios2_Data = (APTR)data;

	if (DoIO((APTR)request) != 0) {
		wpa_printf(MSG_DEBUG, "sana2: CMD_WRITE type %#x failed "
			   "(io_Error %d, WireError %lu)", proto,
			   request->ios2_Req.io_Error,
			   (unsigned long)request->ios2_WireError);
		return -1;
	}
	return 0;
}


int wpa_driver_sana2_l2_send(const char *ifname, const u8 *dst, u16 proto,
			     const u8 *buf, size_t len)
{
	struct wpa_driver_sana2_data *drv;

	for (drv = wpa_driver_sana2_list; drv != NULL; drv = drv->next) {
		if (os_strcmp(drv->ifname, ifname) == 0)
			return wpa_driver_sana2_send_frame(drv, dst, proto,
							   buf, len);
	}

	wpa_printf(MSG_INFO, "sana2: no open device for %s", ifname);
	return -1;
}


static void wpa_driver_sana2_stay_alive(void *eloop_ctx, void *timeout_ctx)
{
	struct wpa_driver_sana2_data *drv = timeout_ctx;

	wpa_driver_sana2_send_frame(drv, drv->bssid, 0, NULL, 0);
	eloop_register_timeout(NULL_INTERVAL, 0, wpa_driver_sana2_stay_alive,
			       drv->ctx, drv);
}


static const u8 *wpa_driver_sana2_get_mac_addr(void *priv)
{
	struct wpa_driver_sana2_data *drv = priv;

	return drv->addr;
}


static int wpa_driver_sana2_send_mlme(void *priv, const u8 *data,
	size_t data_len, int noack, unsigned int freq, const u16 *csa_offs,
	size_t csa_offs_len, int no_encrypt, unsigned int wait, int link_id)
{
	struct wpa_driver_sana2_data *drv = priv;
	struct IOSana2Req *request = drv->request;

	if (!drv->has_extauth)
		return -1;

	wpa_printf(MSG_DEBUG, "sana2: sending a %zu-byte management frame",
		   data_len);
	request->ios2_Req.io_Command = S2_WRITEMGMT;
	request->ios2_Req.io_Flags = 0;
	request->ios2_DataLength = data_len;
	request->ios2_Data = (APTR)data;

	return DoIO((APTR)request) == 0 ? 0 : -1;
}


static int wpa_driver_sana2_send_external_auth_status(void *priv,
	struct external_auth *params)
{
	struct wpa_driver_sana2_data *drv = priv;
	struct IOSana2Req *request = drv->request;

	if (!drv->has_extauth)
		return -1;

	wpa_printf(MSG_DEBUG, "sana2: external authentication status %u",
		   params->status);
	request->ios2_Req.io_Command = S2_EXTAUTHSTATUS;
	request->ios2_WireError = params->status;
	if (params->bssid != NULL)
		CopyMem((APTR)params->bssid, request->ios2_DstAddr, ETH_ALEN);
	else
		os_memset(request->ios2_DstAddr, 0, ETH_ALEN);
	request->ios2_Data = NULL;
	request->ios2_DataLength = 0;

	return DoIO((APTR)request) == 0 ? 0 : -1;
}


static void wpa_driver_sana2_event_connect(struct wpa_driver_sana2_data *drv)
{
	struct IOSana2Req *request = drv->request;
	union wpa_event_data event_data;
	struct TagItem *tag_list;
	u8 *ie = NULL;
	APTR pool;

	os_memset(&event_data, 0, sizeof(event_data));

	pool = CreatePool(MEMF_PUBLIC | MEMF_CLEAR, PUDDLE_SIZE, PUDDLE_SIZE);
	request->ios2_Req.io_Command = S2_GETNETWORKINFO;
	request->ios2_Data = pool;
	if (pool != NULL && DoIO((APTR)request) == 0) {
		const u8 *bssid;

		tag_list = request->ios2_StatData;
		ie = (u8 *)GetTagData(S2INFO_WPAInfo, (UPINT)NULL, tag_list);
		bssid = (const u8 *)GetTagData(S2INFO_BSSID, (UPINT)NULL,
			tag_list);
		if (bssid != NULL)
			os_memcpy(drv->bssid, bssid, ETH_ALEN);
	}
	if (ie != NULL) {
		event_data.assoc_info.req_ies = ie;
		event_data.assoc_info.req_ies_len = ie[1] + 2;
	}
	event_data.assoc_info.freq = drv->freq;

	drv->associated = 1;
	wpa_supplicant_event(drv->ctx, EVENT_ASSOC, &event_data);

	DeletePool(pool);

	/* Start periodic sending of null-data frames */
	eloop_cancel_timeout(wpa_driver_sana2_stay_alive, drv->ctx, drv);
	eloop_register_timeout(NULL_INTERVAL, 0, wpa_driver_sana2_stay_alive,
			       drv->ctx, drv);
}


static void wpa_driver_sana2_event_extauth(struct wpa_driver_sana2_data *drv)
{
	union wpa_event_data event_data;

	if (!drv->have_pending_bssid) {
		wpa_printf(MSG_INFO, "sana2: external authentication "
			   "requested without a BSSID");
		return;
	}

	os_memset(&event_data, 0, sizeof(event_data));
	event_data.external_auth.action = EXT_AUTH_START;
	event_data.external_auth.bssid = drv->pending_bssid;
	event_data.external_auth.ssid = drv->ssid;
	event_data.external_auth.ssid_len = drv->ssid_len;
	event_data.external_auth.key_mgmt_suite = drv->key_mgmt_suite;
	wpa_supplicant_event(drv->ctx, EVENT_EXTERNAL_AUTH, &event_data);
}


static void wpa_driver_sana2_event_handler(int sig, void *sig_ctx)
{
	struct wpa_driver_sana2_data *drv = sig_ctx;
	int i;
	ULONG events;
	struct IOSana2Req *request;
	union wpa_event_data event_data;

	/* Propagate events */
	while ((request = (APTR)GetMsg(drv->port)) != NULL)
	{
		switch (request->ios2_Req.io_Command)
		{
		case CMD_READ:
			if (request->ios2_Req.io_Error == 0) {
				/* Deliver frame to supplicant */
				drv_event_eapol_rx(drv->ctx,
					request->ios2_SrcAddr,
					request->ios2_Data,
					request->ios2_DataLength);
			}
			if (request->ios2_Req.io_Error != S2ERR_OUTOFSERVICE) {
				/* Send request back for next frame */
				request->ios2_DataLength = ETH_MTU;
				SendIO((APTR)request);
			}
			break;

		case S2_READMGMT:
			if (request->ios2_Req.io_Error == 0) {
				/* Deliver frame to supplicant */
				os_memset(&event_data, 0, sizeof(event_data));
				event_data.rx_mgmt.frame = request->ios2_Data;
				event_data.rx_mgmt.frame_len =
					request->ios2_DataLength;
				event_data.rx_mgmt.freq = drv->freq;
				event_data.rx_mgmt.link_id = -1;
				wpa_supplicant_event(drv->ctx, EVENT_RX_MGMT,
					&event_data);
			}
			if (request->ios2_Req.io_Error != S2ERR_OUTOFSERVICE) {
				/* Send request back for next frame */
				request->ios2_DataLength = MGMT_FRAME_SIZE;
				SendIO((APTR)request);
			}
			break;

		case S2_ONEVENT:
			events = request->ios2_WireError;
			if (request->ios2_Req.io_Error != 0)
				events = 0;

			if (events & S2EVENT_EXTAUTH)
				wpa_driver_sana2_event_extauth(drv);

			if (events & S2EVENT_CONNECT)
				wpa_driver_sana2_event_connect(drv);

			if (events & S2EVENT_DISCONNECT) {
				/* Stop periodic sending of null-data frames */
				eloop_cancel_timeout(
					wpa_driver_sana2_stay_alive,
					drv->ctx, drv);

				os_memset(&event_data, 0, sizeof(event_data));
				if (!is_zero_ether_addr(drv->bssid))
					event_data.disassoc_info.addr =
						drv->bssid;
				if (drv->associated) {
					drv->associated = 0;
					wpa_supplicant_event(drv->ctx,
						EVENT_DISASSOC, &event_data);
				} else if (drv->ssid_len > 0) {
					/* The association attempt failed */
					event_data.assoc_reject.bssid =
						drv->have_pending_bssid ?
						drv->pending_bssid : NULL;
					event_data.assoc_reject.status_code =
						WLAN_STATUS_UNSPECIFIED_FAILURE;
					wpa_supplicant_event(drv->ctx,
						EVENT_ASSOC_REJECT,
						&event_data);
				}
			}

			if (events & S2EVENT_ONLINE) {
				drv->event_mask &= ~S2EVENT_ONLINE;
				drv->event_mask |= S2EVENT_OFFLINE;

				/* Send read requests back for next frame */
				drv->eapol_request->ios2_DataLength = ETH_MTU;
				SendIO((APTR)drv->eapol_request);
				if (drv->has_extauth)
					for (i = 0; i < MLME_REQ_COUNT; i++)
						SendIO((APTR)
							drv->mlme_requests[i]);
			}

			if (events & S2EVENT_OFFLINE) {
				drv->event_mask &= ~S2EVENT_OFFLINE;
				drv->event_mask |= S2EVENT_ONLINE;
			}

			/* Send request back to receive next event */
			request->ios2_Req.io_Command = S2_ONEVENT;
			request->ios2_WireError = drv->event_mask;
			SendIO((APTR)request);
			break;

		case S2_GETNETWORKS:
			drv->scanning = 0;
			if (request->ios2_Req.io_Error != 0)
				wpa_printf(MSG_INFO, "sana2: scan failed "
					   "(io_Error %d, WireError %lu)",
					   request->ios2_Req.io_Error,
					   (unsigned long)
					   request->ios2_WireError);
			wpa_supplicant_event(drv->ctx, EVENT_SCAN_RESULTS,
				NULL);
			/* Results that were never fetched */
			if (request->ios2_Data != NULL) {
				DeletePool(request->ios2_Data);
				request->ios2_Data = NULL;
			}
			break;
		}
	}
}


static void wpa_driver_sana2_deinit(void *priv)
{
	struct wpa_driver_sana2_data *drv = priv;
	struct wpa_driver_sana2_data **p;
	int i;

	for (p = &wpa_driver_sana2_list; *p != NULL; p = &(*p)->next) {
		if (*p == drv) {
			*p = drv->next;
			break;
		}
	}

	eloop_cancel_timeout(wpa_driver_sana2_stay_alive, drv->ctx, drv);

	/* Abort outstanding I/O requests */
	if (drv->eapol_request != NULL) {
		AbortIO((APTR)drv->eapol_request);
		WaitIO((APTR)drv->eapol_request);
		FreeVec(drv->eapol_request);
	}

	for (i = 0; i < MLME_REQ_COUNT; i++) {
		if (drv->mlme_requests[i] != NULL) {
			AbortIO((APTR)drv->mlme_requests[i]);
			WaitIO((APTR)drv->mlme_requests[i]);
			FreeVec(drv->mlme_requests[i]);
		}
	}

	if (drv->event_request != NULL) {
		AbortIO((APTR)drv->event_request);
		WaitIO((APTR)drv->event_request);
		FreeVec(drv->event_request);
	}

	if (drv->scan_request != NULL) {
		if (drv->scanning) {
			AbortIO((APTR)drv->scan_request);
			WaitIO((APTR)drv->scan_request);
		}
		if (drv->scan_request->ios2_Data != NULL)
			DeletePool(drv->scan_request->ios2_Data);
		FreeVec(drv->scan_request);
	}

	/* Close device */
	if (drv->device_opened)
		CloseDevice((APTR)drv->request);
	if (drv->request != NULL)
		DeleteIORequest((APTR)drv->request);
	if (drv->port != NULL) {
		/* Replies that arrived after the aborts */
		while (GetMsg(drv->port) != NULL)
			;
		DeleteMsgPort(drv->port);
	}

	FreeVec(drv->device_name);

	os_free(drv);
}


static void * wpa_driver_sana2_init(void *ctx, const char *ifname)
{
	struct wpa_driver_sana2_data *drv;
	int err = 0, i;
	const char *end, *p;
	ULONG unit_no;
	struct IOSana2Req *request, *eapol_request, *mlme_request;
	struct NSDeviceQueryResult device_info =
		{0, sizeof(struct NSDeviceQueryResult)};

	drv = os_zalloc(sizeof(*drv));
	if (drv == NULL)
		return NULL;
	drv->ctx = ctx;
	os_strlcpy(drv->ifname, ifname, sizeof(drv->ifname));

	/* Split fake interface name into device and unit */
	end = strrchr(ifname, ':');
	if (end == NULL) {
		wpa_printf(MSG_ERROR, "sana2: interface name \"%s\" is not "
			   "device:unit", ifname);
		os_free(drv);
		return NULL;
	}
	drv->device_name = AllocVec(strlen(device_dir) + strlen(ifname) + 1,
		MEMF_PUBLIC | MEMF_CLEAR);
	if (drv->device_name == NULL)
		err = 1;

	if (err == 0) {
		char *q;

		if (strchr(ifname, ':') == end && strchr(ifname, '/') == NULL) {
			strcpy(drv->device_name, device_dir);
			q = drv->device_name + strlen(device_dir);
		} else
			q = drv->device_name;
		p = ifname;
		strncpy(q, p, end - p);
		q[end - p] = '\0';

		unit_no = atoi(end + 1);

		drv->port = CreateMsgPort();
		if (drv->port != NULL)
			drv->request = (APTR)CreateIORequest(drv->port,
				sizeof(struct IOSana2Req));
		if (drv->request == NULL)
			err = 1;
	}

	if (err == 0) {
		/* Open device */
		drv->request->ios2_BufferManagement = (APTR)buffer_tags;
		if (OpenDevice((TEXT *)drv->device_name, unit_no,
			(APTR)drv->request, 0) != 0) {
			wpa_printf(MSG_ERROR, "sana2: could not open %s "
				   "unit %lu", drv->device_name,
				   (unsigned long)unit_no);
			err = 1;
		}
	}

	/* Register a handler for the port's signal */
	if (err == 0) {
		drv->device_opened = 1;

		eloop_register_signal(drv->port->mp_SigBit,
			wpa_driver_sana2_event_handler, drv);
	}

	/* Check if this is a hard-MAC device, and which extensions it has */
	if (err == 0) {
		request = drv->request;
		((struct IOStdReq *)request)->io_Command = NSCMD_DEVICEQUERY;
		((struct IOStdReq *)request)->io_Data = &device_info;
		if (DoIO((APTR)request) != 0)
			err = 1;
		else {
			int has_readmgmt = 0, has_writemgmt = 0,
				has_extauthstatus = 0;

			for (i = 0; device_info.SupportedCommands[i] != 0; i++)
			{
				switch (device_info.SupportedCommands[i]) {
				case S2_GETNETWORKS:
					drv->hard_mac = 1;
					break;
				case S2_READMGMT:
					has_readmgmt = 1;
					break;
				case S2_WRITEMGMT:
					has_writemgmt = 1;
					break;
				case S2_EXTAUTHSTATUS:
					has_extauthstatus = 1;
					break;
				}
			}
			drv->has_extauth = has_readmgmt && has_writemgmt &&
				has_extauthstatus;
		}
	}

	if (err == 0 && !drv->hard_mac) {
		wpa_printf(MSG_ERROR, "sana2: %s does not scan itself "
			   "(no S2_GETNETWORKS); soft-MAC devices are not "
			   "supported", drv->device_name);
		err = 1;
	}

	/* Put device online */
	if (err == 0) {
		request = drv->request;
		request->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
		if (DoIO((APTR)request) != 0)
			err = 1;
	}

	if (err == 0) {
		CopyMem(request->ios2_DstAddr, drv->addr, ETH_ALEN);

		request->ios2_Req.io_Command = S2_CONFIGINTERFACE;
		CopyMem(drv->addr, request->ios2_SrcAddr, ETH_ALEN);

		if (DoIO((APTR)request) != 0) {
			if (request->ios2_WireError == S2WERR_IS_CONFIGURED) {
				request->ios2_Req.io_Command = S2_ONLINE;
				DoIO((APTR)request);
			} else
				err = 1;
		}
		drv->online = 1;
	}

	/* Create and queue up EAPOL frame read request */
	if (err == 0) {
		request = drv->request;
		request->ios2_Req.io_Command = CMD_READ;
		request->ios2_PacketType = ETH_P_EAPOL;
		request->ios2_DataLength = ETH_MTU;

		eapol_request = AllocVec(sizeof(struct IOSana2Req) + ETH_MTU,
			MEMF_PUBLIC | MEMF_CLEAR);
		if (eapol_request == NULL)
			err = 1;
	}

	if (err == 0) {
		CopyMem(drv->request, eapol_request,
			sizeof(struct IOSana2Req));
		eapol_request->ios2_Data = eapol_request + 1;
		drv->eapol_request = eapol_request;

		SendIO((APTR)eapol_request);
	}

	/* Create and queue up management frame read requests (SAE
	 * authentication frames) */
	if (err == 0 && drv->has_extauth) {
		request = drv->request;
		request->ios2_Req.io_Command = S2_READMGMT;
		request->ios2_DataLength = MGMT_FRAME_SIZE;
		for (i = 0; i < MLME_REQ_COUNT && err == 0; i++) {
			mlme_request = AllocVec(
				sizeof(struct IOSana2Req) + MGMT_FRAME_SIZE,
				MEMF_PUBLIC | MEMF_CLEAR);
			if (mlme_request == NULL) {
				err = 1;
				break;
			}

			CopyMem(drv->request, mlme_request,
				sizeof(struct IOSana2Req));
			mlme_request->ios2_Data = mlme_request + 1;
			drv->mlme_requests[i] = mlme_request;

			SendIO((APTR)mlme_request);
		}
	}

	/* The scan request lives apart from the synchronous one: a scan is
	 * in flight while other commands are issued */
	if (err == 0) {
		drv->scan_request = AllocVec(sizeof(struct IOSana2Req),
			MEMF_PUBLIC | MEMF_CLEAR);
		if (drv->scan_request == NULL)
			err = 1;
		else
			CopyMem(drv->request, drv->scan_request,
				sizeof(struct IOSana2Req));
	}

	if (err == 0) {
		/* Create and send event-notification request */
		request = drv->request;
		request->ios2_Req.io_Command = S2_ONEVENT;
		drv->event_mask = S2EVENT_CONNECT | S2EVENT_DISCONNECT
			| S2EVENT_OFFLINE;
		if (drv->has_extauth)
			drv->event_mask |= S2EVENT_EXTAUTH;
		request->ios2_WireError = drv->event_mask;

		drv->event_request = AllocVec(sizeof(struct IOSana2Req),
			MEMF_PUBLIC | MEMF_CLEAR);
		if (drv->event_request == NULL)
			err = 1;
	}

	if (err == 0) {
		CopyMem(drv->request, drv->event_request,
			sizeof(struct IOSana2Req));

		SendIO((APTR)drv->event_request);
	}

	if (err != 0) {
		wpa_driver_sana2_deinit(drv);
		return NULL;
	}

	drv->next = wpa_driver_sana2_list;
	wpa_driver_sana2_list = drv;

	wpa_printf(MSG_DEBUG, "sana2: %s unit %lu open, " MACSTR
		   ", external auth %d", drv->device_name,
		   (unsigned long)unit_no, MAC2STR(drv->addr),
		   drv->has_extauth);

	return drv;
}


const struct wpa_driver_ops wpa_driver_sana2_ops = {
	.name = "sana2",
	.desc = "SANA-II driver",
	.get_bssid = wpa_driver_sana2_get_bssid,
	.get_ssid = wpa_driver_sana2_get_ssid,
	.set_key = wpa_driver_sana2_set_key,
	.init = wpa_driver_sana2_init,
	.deinit = wpa_driver_sana2_deinit,
	.scan2 = wpa_driver_sana2_scan,
	.associate = wpa_driver_sana2_associate,
	.deauthenticate = wpa_driver_sana2_deauthenticate,
	.get_capa = wpa_driver_sana2_get_capa,
	.get_mac_addr = wpa_driver_sana2_get_mac_addr,
	.get_scan_results2 = wpa_driver_sana2_get_scan_results,
	.send_mlme = wpa_driver_sana2_send_mlme,
	.send_external_auth_status =
		wpa_driver_sana2_send_external_auth_status,
};
