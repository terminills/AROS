/*
 * WPA Supplicant / main() function for Amiga-like OSes
 * Copyright (c) 2003-2007, Jouni Malinen <j@w1.fi>
 * Copyright (c) 2010-2026, Neil Cafferkey
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "includes.h"

#include "common.h"
#include "common/version.h"
#include "wpa_supplicant_i.h"

#include <exec/types.h>
#include <dos/dos.h>

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

/* DEVICE is the SANA-II device name (DEVS:Networks/ is prepended when it
 * holds no path), UNIT its unit, CONFIG the wpa_supplicant configuration
 * file. NOGUI is accepted for callers written against earlier versions. */
static const TEXT template[] =
	"DEVICE,UNIT/K/N,CONFIG/K,VERBOSE/S,NOGUI/S,VERSION/S";
const TEXT version_string[] = "$VER: WirelessManager 2.11 (22.10.2026)";
static const TEXT config_file_name[] = "ENV:Sys/Wireless.prefs";


int main(int argc, char *argv[])
{
	struct RDArgs *read_args;
	LONG error = 0, unit_no = 0;
	struct
	{
		const TEXT *device;
		LONG *unit;
		const TEXT *config;
		PINT verbose;
		PINT no_gui;
		PINT version;
	}
	args = {NULL, &unit_no, config_file_name, FALSE, FALSE, FALSE};
	struct wpa_interface *iface;
	int exitcode = RETURN_FAIL;
	struct wpa_params params;
	struct wpa_global *global;
	char *ifname = NULL;

	if (os_program_init())
		return RETURN_FAIL;

	os_memset(&params, 0, sizeof(params));
	params.wpa_debug_level = MSG_INFO;

	iface = os_zalloc(sizeof(struct wpa_interface));
	if (iface == NULL) {
		os_program_deinit();
		return RETURN_FAIL;
	}

	/* Parse arguments */
	read_args = ReadArgs(template, (PINT *) &args, NULL);
	if (read_args == NULL) {
		error = IoErr();
		goto out;
	}

	if (args.version) {
		printf("WirelessManager (wpa_supplicant) %s\n%s\n",
		       VERSION_STR, wpa_supplicant_license);
		exitcode = RETURN_OK;
		goto out;
	}

	if (args.device == NULL) {
		error = ERROR_REQUIRED_ARG_MISSING;
		goto out;
	}

	ifname = os_malloc(strlen((const char *) args.device) + 1 + 10 + 1);
	if (ifname == NULL) {
		error = ERROR_NO_FREE_STORE;
		goto out;
	}
	sprintf(ifname, "%s:%ld", (const char *) args.device,
		(long) *args.unit);

	iface->ifname = ifname;
	iface->confname = (const char *) args.config;
	iface->driver = "sana2";

	if (args.verbose)
		params.wpa_debug_level = MSG_MSGDUMP;

	global = wpa_supplicant_init(&params);
	if (global == NULL) {
		wpa_printf(MSG_ERROR, "Failed to initialize WirelessManager");
		goto out;
	}

	if (wpa_supplicant_add_iface(global, iface, NULL) == NULL) {
		wpa_printf(MSG_ERROR, "Failed to add interface %s", ifname);
		wpa_supplicant_deinit(global);
		goto out;
	}

	exitcode = wpa_supplicant_run(global) == 0 ? RETURN_OK : RETURN_FAIL;

	wpa_supplicant_deinit(global);

out:
	os_free(ifname);
	os_free(iface);

	if (read_args != NULL)
		FreeArgs(read_args);

	if (error != 0) {
		PrintFault(error, (CONST_STRPTR) "WirelessManager");
		exitcode = RETURN_FAIL;
	}

	os_program_deinit();

	return exitcode;
}
