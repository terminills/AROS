/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Network printers on the local link, by DNS-SD over multicast DNS
          (RFC 6762, 6763): IPP printers and raw socket printers.
*/

#include <exec/types.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>

#include <string.h>

#include "fp_mdns.h"

#define MDNS_PORT       5353
#define TYPE_A          1
#define TYPE_PTR        12
#define TYPE_TXT        16
#define TYPE_SRV        33
#define MAX_RECORDS     96

static const char *services[] = { "_ipp._tcp.local", "_pdl-datastream._tcp.local" };

struct Record {
    UWORD r_Type;
    char r_Name[NAME_MAX];
    char r_Target[NAME_MAX];            /* PTR, SRV: the name pointed to */
    UWORD r_Port;                       /* SRV */
    ULONG r_Addr;                       /* A, network order */
    UBYTE r_Txt[TXT_MAX];               /* TXT as sent: length-prefixed strings */
    UWORD r_TxtLen;
};

static struct Record records[MAX_RECORDS];
static LONG nrecords;

/* ------------------------------------------------------------------ DNS wire format */

static LONG putName(UBYTE *p, const char *name)
{
    UBYTE *start = p;

    while (*name) {
        const char *dot = strchr(name, '.');
        LONG n = dot ? dot - name : (LONG)strlen(name);
        *p++ = n;
        memcpy(p, name, n);
        p += n;
        name += n;
        if (*name == '.')
            name++;
    }
    *p++ = 0;
    return p - start;
}

/* A possibly compressed name at off in pkt; returns the offset after it in the record, or -1. */
static LONG getName(const UBYTE *pkt, LONG len, LONG off, char *out)
{
    LONG end = -1, n = 0, hops = 0;

    for (;;) {
        UBYTE l;
        if (off >= len)
            return -1;
        l = pkt[off];
        if ((l & 0xc0) == 0xc0) {
            if (off + 1 >= len || ++hops > 16)
                return -1;
            if (end < 0)
                end = off + 2;
            off = ((l & 0x3f) << 8) | pkt[off + 1];
            continue;
        }
        if (l == 0)
            break;
        if (off + 1 + l > len || n + l + 2 > NAME_MAX)
            return -1;
        if (n)
            out[n++] = '.';
        memcpy(out + n, pkt + off + 1, l);
        n += l;
        off += 1 + l;
    }
    out[n] = 0;
    return end >= 0 ? end : off + 1;
}

static UWORD get16(const UBYTE *p) { return (p[0] << 8) | p[1]; }

static void addRecord(struct Record *r)
{
    LONG i;

    for (i = 0; i < nrecords; i++) {
        struct Record *o = &records[i];
        if (o->r_Type == r->r_Type && strcasecmp(o->r_Name, r->r_Name) == 0
            && (r->r_Type != TYPE_PTR || strcasecmp(o->r_Target, r->r_Target) == 0)) {
            *o = *r;
            return;
        }
    }
    if (nrecords < MAX_RECORDS)
        records[nrecords++] = *r;
}

static void parsePacket(const UBYTE *pkt, LONG len)
{
    LONG off = 12, i, count;
    struct Record r;

    if (len < 12 || !(get16(pkt + 2) & 0x8000))
        return;
    for (i = get16(pkt + 4); i > 0; i--) {
        char skip[NAME_MAX];
        if ((off = getName(pkt, len, off, skip)) < 0)
            return;
        off += 4;
    }
    count = get16(pkt + 6) + get16(pkt + 8) + get16(pkt + 10);
    for (i = 0; i < count; i++) {
        UWORD type, rdlen;
        memset(&r, 0, sizeof(r));
        if ((off = getName(pkt, len, off, r.r_Name)) < 0 || off + 10 > len)
            return;
        type = get16(pkt + off);
        rdlen = get16(pkt + off + 8);
        off += 10;
        if (off + rdlen > len)
            return;
        r.r_Type = type;
        if (type == TYPE_PTR && getName(pkt, len, off, r.r_Target) >= 0)
            addRecord(&r);
        else if (type == TYPE_SRV && rdlen > 6 && getName(pkt, len, off + 6, r.r_Target) >= 0) {
            r.r_Port = get16(pkt + off + 4);
            addRecord(&r);
        } else if (type == TYPE_A && rdlen == 4) {
            memcpy(&r.r_Addr, pkt + off, 4);
            addRecord(&r);
        } else if (type == TYPE_TXT && rdlen <= TXT_MAX) {
            memcpy(r.r_Txt, pkt + off, rdlen);
            r.r_TxtLen = rdlen;
            addRecord(&r);
        }
        off += rdlen;
    }
}

static struct Record *findRecord(UWORD type, const char *name)
{
    LONG i;

    for (i = 0; i < nrecords; i++)
        if (records[i].r_Type == type && strcasecmp(records[i].r_Name, name) == 0)
            return &records[i];
    return NULL;
}

/* The value of key in a TXT record ("" when absent). */
static void txtValue(struct Record *t, const char *key, char *out, LONG max)
{
    LONG off = 0, kl = strlen(key);

    out[0] = 0;
    while (t && off < t->r_TxtLen) {
        UBYTE l = t->r_Txt[off];
        const char *s = (const char *)t->r_Txt + off + 1;
        if (l > kl && strncasecmp(s, key, kl) == 0 && s[kl] == '=') {
            LONG n = l - kl - 1;
            if (n >= max)
                n = max - 1;
            memcpy(out, s + kl + 1, n);
            out[n] = 0;
            return;
        }
        off += 1 + l;
    }
}

/* ------------------------------------------------------------------ the link */

/* One query to the mDNS group from an ordinary port: responders answer this asker directly
   (RFC 6762 section 6.7), so FindPrinters needs neither port 5353 nor a group membership. */
static void sendQuery(LONG fd, const char **names, const UWORD *types, LONG n)
{
    UBYTE pkt[1500];
    LONG len = 12, i;
    struct sockaddr_in to;

    memset(pkt, 0, 12);
    pkt[0] = 0x46;                      /* an ID: legacy unicast answers repeat it */
    pkt[1] = 0x50;
    pkt[5] = n;
    for (i = 0; i < n && len < (LONG)sizeof(pkt) - NAME_MAX - 4; i++) {
        len += putName(pkt + len, names[i]);
        pkt[len++] = 0;
        pkt[len++] = types[i];
        pkt[len++] = 0;
        pkt[len++] = 1;                 /* class IN */
    }
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(MDNS_PORT);
    to.sin_addr.s_addr = htonl(0xe00000fb);  /* 224.0.0.251 */
    sendto(fd, pkt, len, 0, (struct sockaddr *)&to, sizeof(to));
}

static void gather(LONG fd, LONG ms)
{
    UBYTE pkt[9000];
    struct timeval tv;
    fd_set set;
    LONG left = ms;

    while (left > 0) {
        ULONG sigs = SIGBREAKF_CTRL_C;
        LONG n;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        tv.tv_sec = left / 1000;
        tv.tv_usec = (left % 1000) * 1000;
        n = WaitSelect(fd + 1, &set, NULL, NULL, &tv, &sigs);
        if (sigs & SIGBREAKF_CTRL_C)
            return;
        if (n <= 0)
            return;
        n = recv(fd, pkt, sizeof(pkt), 0);
        if (n > 0)
            parsePacket(pkt, n);
        left -= 50;                     /* a chatty link does not keep us forever */
    }
}

/* Asks for what the first answers left out: SRV and TXT of instances, A of their hosts. */
static void askMissing(LONG fd)
{
    const char *names[16];
    UWORD types[16];
    LONG n = 0, i;

    for (i = 0; i < nrecords && n < 14; i++) {
        struct Record *ptr = &records[i];
        struct Record *srv;
        if (ptr->r_Type != TYPE_PTR)
            continue;
        if (!(srv = findRecord(TYPE_SRV, ptr->r_Target))) {
            names[n] = ptr->r_Target;
            types[n++] = TYPE_SRV;
        } else if (!findRecord(TYPE_A, srv->r_Target)) {
            names[n] = srv->r_Target;
            types[n++] = TYPE_A;
        }
        if (!findRecord(TYPE_TXT, ptr->r_Target)) {
            names[n] = ptr->r_Target;
            types[n++] = TYPE_TXT;
        }
    }
    if (n) {
        sendQuery(fd, names, types, n);
        gather(fd, 1500);
    }
}

static void appendNumber(char *s, ULONG v)
{
    char d[12];
    LONG i = 0;

    do {
        d[i++] = '0' + v % 10;
        v /= 10;
    } while (v);
    s += strlen(s);
    while (i)
        *s++ = d[--i];
    *s = 0;
}

static void addressText(ULONG addr, char *out)
{
    const UBYTE *b = (const UBYTE *)&addr;
    LONG i;

    out[0] = 0;
    for (i = 0; i < 4; i++) {
        if (i)
            strcat(out, ".");
        appendNumber(out, b[i]);
    }
}

/* Printers from the records: one per instance name, IPP when it has it. */
static LONG collect(struct MDNSPrinter *printers, LONG max)
{
    LONG pass, i, j, nprinters = 0;

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < nrecords; i++) {
            struct Record *ptr = &records[i], *srv, *a, *txt;
            struct MDNSPrinter *p;
            char inst[NAME_MAX], host[32], rp[NAME_MAX], *dot;
            if (ptr->r_Type != TYPE_PTR || strcasecmp(ptr->r_Name, services[pass]) != 0)
                continue;
            if (!(srv = findRecord(TYPE_SRV, ptr->r_Target)) || !(a = findRecord(TYPE_A, srv->r_Target)))
                continue;
            strcpy(inst, ptr->r_Target);
            if ((dot = strstr(inst, services[pass]) ) && dot > inst)
                dot[-1] = 0;
            for (j = 0; j < nprinters && strcmp(printers[j].mp_Name, inst) != 0; j++)
                ;
            if (j < nprinters || nprinters == max)
                continue;
            p = &printers[nprinters++];
            memset(p, 0, sizeof(*p));
            strcpy(p->mp_Name, inst);
            txt = findRecord(TYPE_TXT, ptr->r_Target);
            addressText(a->r_Addr, host);
            p->mp_IPP = pass == 0;
            if (p->mp_IPP) {
                txtValue(txt, "rp", rp, sizeof(rp));
                strcpy(p->mp_URI, "ipp://");
                strcat(p->mp_URI, host);
                strcat(p->mp_URI, ":");
                appendNumber(p->mp_URI, srv->r_Port);
                strcat(p->mp_URI, "/");
                strcat(p->mp_URI, rp);
            } else {
                strcpy(p->mp_URI, "socket://");
                strcat(p->mp_URI, host);
                strcat(p->mp_URI, ":");
                appendNumber(p->mp_URI, srv->r_Port);
            }
            txtValue(txt, "pdl", p->mp_Formats, sizeof(p->mp_Formats));
            txtValue(txt, "UUID", p->mp_UUID, sizeof(p->mp_UUID));
        }
    }
    return nprinters;
}

LONG MDNS_FindPrinters(LONG waitms, struct MDNSPrinter *printers, LONG max)
{
    UWORD ptrs[2] = { TYPE_PTR, TYPE_PTR };
    LONG fd;

    nrecords = 0;
    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        return -1;
    sendQuery(fd, services, ptrs, 2);
    gather(fd, waitms);
    askMissing(fd);
    CloseSocket(fd);
    return collect(printers, max);
}
