/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Standard IPv6 address constants (POSIX / RFC 2553).

    Kept in their own object so that a program which only needs the
    constants does not pull in the bsdsocket.library stubs and, through
    their SocketBase reference, the autoinit that refuses to start without
    a running TCP/IP stack.
*/

#include <sys/types.h>
#include <netinet/in.h>

const struct in6_addr in6addr_any      = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;
