/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#ifndef FP_MDNS_H
#define FP_MDNS_H

#include <exec/types.h>

#define NAME_MAX        256
#define TXT_MAX         1300    /* a TXT record as large as one mDNS packet carries (RFC 6763 6.2) */

/* A printer that answered on the local link. */
struct MDNSPrinter {
    char mp_Name[NAME_MAX];             /* the instance's first label */
    char mp_URI[NAME_MAX];              /* ipp://host:port/rp or socket://host:port */
    char mp_Formats[TXT_MAX];           /* TXT pdl: the document formats it takes */
    char mp_UUID[64];                   /* TXT UUID */
    BOOL mp_IPP;
};

/* Asks the link for printers (_ipp._tcp, then _pdl-datastream._tcp) and listens waitms.
   One query from an ordinary port: responders answer the asker directly (RFC 6762 6.7),
   so neither port 5353 nor group membership is needed. SocketBase must be open.
   Returns how many went to printers, -1 without a socket. */
LONG MDNS_FindPrinters(LONG waitms, struct MDNSPrinter *printers, LONG max);

#endif
