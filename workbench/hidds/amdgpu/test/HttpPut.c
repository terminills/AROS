/*
 * HttpPut — send a file FROM the DUT to the host over the RNDIS link.
 *
 * WHY THIS EXISTS
 * ---------------
 * The bench had push but no pull: C:HttpGet fetches host -> DUT, and the only
 * way back was `Type <file>` over the NetShell socket. That is fine for a few
 * lines of probe output, which is all it ever had to carry. It is NOT a
 * transport: pulling a 23KB PCITool report wedged the shell partway through
 * (19250 of 23201 bytes, stopping before the entry we actually wanted), because
 * NetShell is a single-threaded shell on one socket and any other command sent
 * while Type is streaming stalls it.
 *
 * Now that the rescue media carries artifact-producing tools — PCITool reports,
 * and AROSVNCServer — retrieving files needs to be a first-class operation.
 *
 *   HttpPut RAM:PCIToolInfo.txt 192.168.0.158 8898 /upload/pcitool.txt
 *
 * Host side is anything that accepts a PUT body, e.g.
 *   python3 -c 'import http.server,socketserver;
 *              ...' or the existing collector in qemu-test/.
 *
 * Deliberately dumb: one socket, one PUT, no chunking, no retry. The DUT side
 * of this bench should stay trivially debuggable — if a transfer fails we want
 * to see it fail plainly, not silently half-succeed the way Type did.
 */
#define DEBUG 1
#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <string.h>

#define PLOG(...)  do { bug("[HttpPut] " __VA_ARGS__); bug("\n"); } while (0)
#define SHELL(...) do { Printf((CONST_STRPTR)"[HttpPut] " __VA_ARGS__); \
                        Printf((CONST_STRPTR)"\n"); } while (0)

#define CHUNK 4096

int main(void)
{
    struct RDArgs *rd;
    IPTR args[4] = { 0, 0, 0, 0 };
    STRPTR path, host, uri;
    LONG port = 8898;
    struct Library *SocketBase = NULL;
    BPTR fh = BNULL;
    LONG sock = -1;
    struct sockaddr_in sa;
    UBYTE *buf = NULL;
    LONG total = 0, n;
    LONG rc = 20;

    rd = ReadArgs((CONST_STRPTR)"FILE/A,HOST/A,PORT/N,URI", args, NULL);
    if (!rd) {
        Printf((CONST_STRPTR)"usage: HttpPut <file> <host> [port] [uri]\n");
        return 20;
    }
    path = (STRPTR)args[0];
    host = (STRPTR)args[1];
    if (args[2]) port = *(LONG *)args[2];
    uri  = args[3] ? (STRPTR)args[3] : (STRPTR)"/upload";

    buf = AllocVec(CHUNK, MEMF_ANY);
    if (!buf) { PLOG("out of memory"); goto out; }

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) { PLOG("cannot open %s", path); SHELL("FAIL: open %s", path); goto out; }

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (!SocketBase) { PLOG("no bsdsocket.library"); goto out; }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { PLOG("socket() failed"); goto out; }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((UWORD)port);
    sa.sin_addr.s_addr = inet_addr((char *)host);
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        PLOG("connect %s:%ld failed", host, port);
        SHELL("FAIL: connect %s:%ld", host, (IPTR)port);
        goto out;
    }

    /* Chunked transfer-encoding keeps this streaming without needing the file
     * size up front, so it works on anything Open() can read. */
    {
        char hdr[320];
        LONG hl = 0;
        hl += snprintf(hdr + hl, sizeof(hdr) - hl,
                       "PUT %s HTTP/1.1\r\nHost: %s\r\n"
                       "Transfer-Encoding: chunked\r\n"
                       "Content-Type: application/octet-stream\r\n\r\n",
                       (char *)uri, (char *)host);
        if (send(sock, hdr, hl, 0) != hl) { PLOG("header send failed"); goto out; }
    }

    while ((n = Read(fh, buf, CHUNK)) > 0) {
        char cl[16];
        LONG cll = snprintf(cl, sizeof(cl), "%lx\r\n", (unsigned long)n);
        if (send(sock, cl, cll, 0) != cll)   { PLOG("chunk hdr failed"); goto out; }
        if (send(sock, buf, n, 0) != n)      { PLOG("chunk body failed"); goto out; }
        if (send(sock, "\r\n", 2, 0) != 2)   { PLOG("chunk crlf failed"); goto out; }
        total += n;
    }
    if (send(sock, "0\r\n\r\n", 5, 0) != 5) { PLOG("terminator failed"); goto out; }

    PLOG("*** sent %ld bytes of %s to %s:%ld%s ***",
         total, path, host, port, uri);
    SHELL("sent %ld bytes -> %s:%ld%s", total, host, (IPTR)port, uri);
    rc = 0;

out:
    if (sock >= 0)   CloseSocket(sock);
    if (SocketBase)  CloseLibrary(SocketBase);
    if (fh)          Close(fh);
    if (buf)         FreeVec(buf);
    if (rd)          FreeArgs(rd);
    return rc;
}
