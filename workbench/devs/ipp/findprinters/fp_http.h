/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#ifndef FP_HTTP_H
#define FP_HTTP_H

#include <exec/types.h>

/* One request to http[s]:// or ipp:// (http on port 631). The whole answer goes to buf;
   body/bodylen point at the body (de-chunked). Returns the HTTP status, -1 when there
   was no answer. A redirect's target goes to location when it is not NULL. */
LONG HTTP_Request(const char *method, const char *url, const char *type, const UBYTE *data, LONG datalen,
    UBYTE *buf, LONG bufmax, UBYTE **body, LONG *bodylen, char *location, LONG locmax);

/* GET, following up to three redirects (a printer sends http to https). */
LONG HTTP_Get(const char *url, UBYTE *buf, LONG bufmax, UBYTE **body, LONG *bodylen);

/* Why the last request had no answer: an HTTP_ERR_ value, or an mbedTLS
   handshake error (negative). */
extern LONG HTTP_LastError;
#define HTTP_ERR_URL        1
#define HTTP_ERR_HOST       2
#define HTTP_ERR_CONNECT    3
#define HTTP_ERR_TLS_SETUP  4
#define HTTP_ERR_SEND       5
#define HTTP_ERR_REPLY      6

#endif
