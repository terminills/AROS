/*
 * HttpGet — minimal bsdsocket HTTP/1.0 downloader for the DUT fast-iteration
 * loop. Fetches a file from the host HTTP server over the RNDIS link into a
 * local path (RAM:), so a freshly-built GfxResetProbe (or any binary) can be
 * pushed and run WITHOUT re-imaging/rebooting — reboot only when a reset lever
 * actually needs a clean slate.
 *
 *   HttpGet GfxResetProbe RAM:GRP
 *   HttpGet GfxResetProbe RAM:GRP HOST=192.168.0.158 PORT=8898
 *   RAM:GRP grbm-f0        # then run the fetched binary with a lever
 *
 * Headers are assumed to arrive within the first recv() (true for a simple
 * python http.server); the body after the first CRLFCRLF is streamed to DEST.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h>

#include <exec/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <string.h>

#define DEBUG 1
#include <aros/debug.h>

/* Log to BOTH the shell (operator on the DUT screen) and serial/COM1 (captured
 * by the host) so a failed fetch is diagnosable from the serial log. */
#define PLOG(...) do { Printf((CONST_STRPTR)"[HttpGet] " __VA_ARGS__); \
                       Printf((CONST_STRPTR)"\n"); \
                       bug("[HttpGet] " __VA_ARGS__); bug("\n"); } while (0)

struct Library *SocketBase = NULL;

/* little-endian x86_64: host<->network short byte swap. */
static UWORD hg_htons(UWORD v) { return (UWORD)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF)); }

int main(void)
{
    struct RDArgs *rd;
    IPTR args[4] = { 0, 0, 0, 0 };
    const char *path, *dest, *host = "192.168.0.158";
    LONG port = 8898;
    int s;
    struct sockaddr_in sa;
    char req[600];
    char buf[8192];
    BPTR fh;
    LONG n, total = 0;
    int header_done = 0;

    rd = ReadArgs((CONST_STRPTR)"PATH/A,DEST/A,HOST/K,PORT/K/N", args, NULL);
    if (!rd) { PLOG("usage: HttpGet PATH DEST [HOST=ip] [PORT=n]"); return 20; }
    path = (const char *)args[0];
    dest = (const char *)args[1];
    if (args[2]) host = (const char *)args[2];
    if (args[3]) port = *(LONG *)args[3];

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 0);
    if (!SocketBase) { PLOG("no bsdsocket.library — is AROSTCP up?"); FreeArgs(rd); return 20; }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { PLOG("socket() failed"); goto out; }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = hg_htons((UWORD)port);
    sa.sin_addr.s_addr = inet_addr((char *)host);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        PLOG("connect(%s:%ld) failed", host, (LONG)port);
        CloseSocket(s); goto out;
    }

    strcpy(req, "GET /");
    strncat(req, path, sizeof(req) - 128);
    strcat(req, " HTTP/1.0\r\nHost: ");
    strncat(req, host, 64);
    strcat(req, "\r\nConnection: close\r\n\r\n");
    send(s, req, strlen(req), 0);
    PLOG("GET /%s from %s:%ld -> %s", path, host, (LONG)port, dest);

    fh = Open((CONST_STRPTR)dest, MODE_NEWFILE);
    if (!fh) { PLOG("cannot open dest '%s'", dest); CloseSocket(s); goto out; }

    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
        if (!header_done) {
            LONG i;
            for (i = 0; i + 3 < n; i++) {
                if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
                    LONG body = n - (i + 4);
                    if (body > 0) { Write(fh, buf + i + 4, body); total += body; }
                    header_done = 1;
                    break;
                }
            }
            if (!header_done)
                PLOG("WARN: CRLFCRLF not in first chunk (%ld bytes) — headers too big?", (LONG)n);
        } else {
            Write(fh, buf, n); total += n;
        }
    }

    Close(fh);
    CloseSocket(s);
    PLOG("*** wrote %ld bytes to %s ***", total, dest);

out:
    if (SocketBase) CloseLibrary(SocketBase);
    FreeArgs(rd);
    return (total > 0) ? 0 : 20;
}
