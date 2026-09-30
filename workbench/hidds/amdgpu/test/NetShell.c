/*
 * NetShell — reverse agent bridge over bsdsocket for the DUT fast-iteration
 * loop: shell + bidirectional file transfer + mouse injection, all on ONE
 * outbound connection.
 *
 * The DUT sits behind the NanoKVM (it can dial out; the host cannot dial in),
 * so NetShell DIALS OUT to a listener on the host. Everything rides that single
 * socket. That is deliberate: HttpGet needed a SECOND outbound port (8898) and
 * never worked, while this port did -- consistent with the KVM port-forwarding
 * 2323 rather than doing general NAT. Rather than keep fighting the network to
 * open another port, file transfer moved onto the channel that is already
 * proven. One connection, one thing to keep working.
 *
 * Protocol: line-based text, and lines beginning with '!' are handled HERE
 * instead of being passed to the shell. Everything else is a shell command
 * whose output is streamed back.
 *
 *   !put <path> <nbytes>   host -> DUT: exactly <nbytes> raw bytes follow.
 *                          Replies "!ok <n>" or "!err ...".
 *   !get <path>            DUT -> host: replies "!data <nbytes>" + raw bytes,
 *                          or "!err ...".
 *   !mouse <x> <y>         absolute pointer move (IECLASS_POINTERPOS).
 *   !click <l|r|m>         press+release at the current position.
 *   !ping                  replies "!pong" (liveness without shell noise).
 *   quit / exit            close the session.
 *
 * Raw bytes are NOT line-framed, so binaries transfer intact. The byte count is
 * always sent first so neither side has to guess where the payload ends.
 *
 *   host$  netshell-ctl.py serve
 *   DUT>   NetShell                      # dials out, opens the bridge
 *   host$  netshell-ctl.py push C/FbProbe RAM:FP
 *   host$  netshell-ctl.py run  "RAM:FP"
 *   host$  netshell-ctl.py pull T:result.log ./result.log
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h>

#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <string.h>
#include <stdlib.h>

#define DEBUG 1
#include <aros/debug.h>

#define PLOG(...) do { bug("[NetShell] " __VA_ARGS__); bug("\n"); } while (0)

struct Library *SocketBase = NULL;

/* input.device, opened lazily on the first !mouse/!click */
static struct MsgPort  *inpPort = NULL;
static struct IOStdReq *inpReq  = NULL;

static UWORD ns_htons(UWORD v) { return (UWORD)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF)); }

/* send all bytes (bsdsocket send may short-write). */
static void send_all(int s, const char *p, LONG len)
{
    while (len > 0) {
        LONG n = send(s, (APTR)p, len, 0);
        if (n <= 0) break;
        p += n; len -= n;
    }
}

static void send_str(int s, const char *p) { send_all(s, p, (LONG)strlen(p)); }

/* recv EXACTLY len bytes; returns TRUE only if all arrived. recv() short-reads
 * routinely on a stream socket, and a partial binary is worse than none. */
static BOOL recv_all(int s, char *p, LONG len)
{
    while (len > 0) {
        LONG n = recv(s, p, len, 0);
        if (n <= 0) return FALSE;
        p += n; len -= n;
    }
    return TRUE;
}

static BOOL input_open(void)
{
    if (inpReq) return TRUE;
    inpPort = CreateMsgPort();
    if (!inpPort) return FALSE;
    inpReq = (struct IOStdReq *)CreateIORequest(inpPort, sizeof(struct IOStdReq));
    if (!inpReq) { DeleteMsgPort(inpPort); inpPort = NULL; return FALSE; }
    if (OpenDevice((CONST_STRPTR)"input.device", 0, (struct IORequest *)inpReq, 0)) {
        DeleteIORequest((struct IORequest *)inpReq); inpReq = NULL;
        DeleteMsgPort(inpPort); inpPort = NULL;
        return FALSE;
    }
    return TRUE;
}

static void input_close(void)
{
    if (!inpReq) return;
    CloseDevice((struct IORequest *)inpReq);
    DeleteIORequest((struct IORequest *)inpReq);
    DeleteMsgPort(inpPort);
    inpReq = NULL; inpPort = NULL;
}

static void input_event(UWORD class, UWORD code, WORD x, WORD y)
{
    struct InputEvent ie;

    if (!input_open()) return;
    memset(&ie, 0, sizeof(ie));
    ie.ie_Class     = class;
    ie.ie_SubClass  = 0;
    ie.ie_Code      = code;
    ie.ie_Qualifier = 0;
    ie.ie_X         = x;
    ie.ie_Y         = y;
    ie.ie_NextEvent = NULL;

    inpReq->io_Data    = &ie;
    inpReq->io_Length  = sizeof(ie);
    inpReq->io_Command = IND_WRITEEVENT;
    DoIO((struct IORequest *)inpReq);
}

/* "!put <path> <nbytes>" — receive a file. */
static void do_put(int s, char *args)
{
    char *path = args, *sizetok;
    LONG want, got = 0;
    BPTR fh;
    char buf[4096];

    while (*path == ' ') path++;
    sizetok = strchr(path, ' ');
    if (!sizetok) { send_str(s, "!err put needs <path> <nbytes>\n"); return; }
    *sizetok++ = '\0';
    while (*sizetok == ' ') sizetok++;
    want = atol(sizetok);
    if (want < 0) { send_str(s, "!err bad size\n"); return; }

    fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (!fh) {
        /* Still drain the payload, or the stream desyncs and every later
         * command is parsed out of file data. */
        while (got < want) {
            LONG chunk = (want - got) > (LONG)sizeof(buf) ? (LONG)sizeof(buf) : (want - got);
            if (!recv_all(s, buf, chunk)) break;
            got += chunk;
        }
        send_str(s, "!err cannot open dest\n");
        return;
    }

    while (got < want) {
        LONG chunk = (want - got) > (LONG)sizeof(buf) ? (LONG)sizeof(buf) : (want - got);
        if (!recv_all(s, buf, chunk)) break;
        if (Write(fh, buf, chunk) != chunk) break;
        got += chunk;
    }
    Close(fh);

    if (got == want) {
        /* Pushed binaries are meant to be run, so make them executable. */
        SetProtection((CONST_STRPTR)path, 0);
        { char rep[96]; __sprintf(rep, "!ok %ld\n", (long)got); send_str(s, rep); }
        PLOG("put %s %ld bytes", path, (long)got);
    } else {
        char rep[96]; __sprintf(rep, "!err short %ld/%ld\n", (long)got, (long)want);
        send_str(s, rep);
        PLOG("put %s SHORT %ld/%ld", path, (long)got, (long)want);
    }
}

/* "!get <path>" — send a file back. */
static void do_get(int s, char *args)
{
    char *path = args;
    BPTR fh;
    LONG size = 0, n;
    char buf[4096], hdr[96];

    while (*path == ' ') path++;
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) { send_str(s, "!err cannot open src\n"); return; }

    /* Size first so the host knows exactly how much to read. */
    if (Seek(fh, 0, OFFSET_END) >= 0) { size = Seek(fh, 0, OFFSET_BEGINNING); }
    if (size < 0) size = 0;
    Seek(fh, 0, OFFSET_BEGINNING);

    __sprintf(hdr, "!data %ld\n", (long)size);
    send_str(s, hdr);
    while ((n = Read(fh, buf, sizeof(buf))) > 0)
        send_all(s, buf, n);
    Close(fh);
    PLOG("get %s %ld bytes", path, (long)size);
}

static void do_mouse(int s, char *args)
{
    char *p = args, *q;
    LONG x, y;

    while (*p == ' ') p++;
    q = strchr(p, ' ');
    if (!q) { send_str(s, "!err mouse needs <x> <y>\n"); return; }
    *q++ = '\0';
    x = atol(p); y = atol(q);
    input_event(IECLASS_POINTERPOS, 0, (WORD)x, (WORD)y);
    send_str(s, "!ok\n");
}

static void do_click(int s, char *args)
{
    UWORD down = IECODE_LBUTTON;
    char *p = args;

    while (*p == ' ') p++;
    if (*p == 'r') down = IECODE_RBUTTON;
    else if (*p == 'm') down = IECODE_MBUTTON;

    input_event(IECLASS_RAWMOUSE, down, 0, 0);
    input_event(IECLASS_RAWMOUSE, (UWORD)(down | IECODE_UP_PREFIX), 0, 0);
    send_str(s, "!ok\n");
}

int main(void)
{
    struct RDArgs *rd;
    IPTR args[2] = { 0, 0 };
    const char *host = "192.168.0.158";
    LONG port = 2323;
    int s;
    struct sockaddr_in sa;
    char line[512];
    LONG lpos = 0;

    rd = ReadArgs((CONST_STRPTR)"HOST/K,PORT/K/N", args, NULL);
    if (rd) {
        if (args[0]) host = (const char *)args[0];
        if (args[1]) port = *(LONG *)args[1];
    }

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 0);
    if (!SocketBase) { PLOG("no bsdsocket.library"); if (rd) FreeArgs(rd); return 20; }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = ns_htons((UWORD)port);
    sa.sin_addr.s_addr = inet_addr((char *)host);
    PLOG("target %s:%ld — dialing (retry until AROSTCP is up)", host, (LONG)port);

    /* Retry the connect with per-attempt serial debug: when auto-launched from
     * the rescue Startup-Network, NetShell can start before AROSTCP finishes
     * binding the RNDIS interface, so the first connects fail. 40 x ~3s = ~2min.
     * The errno is worth reading: 65/EHOSTUNREACH means no route at all, 60/
     * ETIMEDOUT means the packets are routed but nothing answers (a missing
     * forward at the NAT hop, not a missing listener). */
    s = -1;
    {
        int attempt, ok = 0;
        for (attempt = 1; attempt <= 40; attempt++) {
            if (s >= 0) CloseSocket(s);
            s = socket(AF_INET, SOCK_STREAM, 0);
            if (s < 0) { PLOG("attempt %ld: socket() failed errno=%ld", (LONG)attempt, (LONG)Errno()); Delay(150); continue; }
            if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
                PLOG("*** connected on attempt %ld — reverse bridge open ***", (LONG)attempt);
                ok = 1; break;
            }
            PLOG("attempt %ld/40: connect failed errno=%ld — retry in 3s (networking not up yet?)",
                 (LONG)attempt, (LONG)Errno());
            Delay(150);   /* ~3s (50 ticks/s) */
        }
        if (!ok) { PLOG("gave up after 40 attempts — no host listener / no route"); if (s >= 0) CloseSocket(s); goto out; }
    }
    send_all(s, "AROS NetShell ready\n$ ", 22);

    for (;;) {
        char c;
        LONG n = recv(s, &c, 1, 0);
        if (n <= 0) break;                 /* peer closed */
        if (c == '\r') continue;
        if (c != '\n') {
            if (lpos < (LONG)sizeof(line) - 1) line[lpos++] = c;
            continue;
        }
        line[lpos] = '\0';
        lpos = 0;
        if (line[0] == '\0') { send_all(s, "$ ", 2); continue; }
        if (!strcmp(line, "quit") || !strcmp(line, "exit")) break;

        /* Bridge verbs are handled here and never reach the shell. They send
         * their own reply and deliberately do NOT emit a "$ " prompt, so the
         * host can parse a clean response (and, for !get, raw bytes) without a
         * prompt landing in the middle of the payload. */
        if (line[0] == '!') {
            if (!strncmp(line + 1, "put ", 4))        { do_put(s, line + 5);   continue; }
            if (!strncmp(line + 1, "get ", 4))        { do_get(s, line + 5);   continue; }
            if (!strncmp(line + 1, "mouse ", 6))      { do_mouse(s, line + 7); continue; }
            if (!strncmp(line + 1, "click", 5))       { do_click(s, line + 6); continue; }
            if (!strcmp(line + 1, "ping"))            { send_str(s, "!pong\n"); continue; }
            send_str(s, "!err unknown bridge verb\n");
            continue;
        }

        /* Echo the command to the DUT CONSOLE too, so the operator can watch the
         * live session on the KVM screen (not just over the socket). */
        Printf((CONST_STRPTR)"$ %s\n", (IPTR)line);

        /* Run the command through the shell, capture output to a temp file, then
         * mirror it to BOTH the socket (host) and the console (KVM screen).
         * Probes also log to serial via bug() — three channels. */
        {
            BPTR in  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
            BPTR out = Open((CONST_STRPTR)"T:nsout", MODE_NEWFILE);
            if (out) {
                SystemTags((CONST_STRPTR)line,
                           SYS_Input,  (IPTR)in,
                           SYS_Output, (IPTR)out,
                           SYS_UserShell, TRUE,
                           TAG_END);
                Close(out);
                out = Open((CONST_STRPTR)"T:nsout", MODE_OLDFILE);
                if (out) {
                    char buf[2048]; LONG rn;
                    BPTR con = Output();
                    while ((rn = Read(out, buf, sizeof(buf))) > 0) {
                        send_all(s, buf, rn);       /* -> host (my automation) */
                        if (con) Write(con, buf, rn); /* -> KVM screen (operator watches) */
                    }
                    Close(out);
                }
            }
            if (in) Close(in);
        }
        Flush(Output());
        send_all(s, "\n$ ", 3);
    }

    send_all(s, "bye\n", 4);
    CloseSocket(s);
    PLOG("session closed");

out:
    input_close();
    if (SocketBase) CloseLibrary(SocketBase);
    if (rd) FreeArgs(rd);
    return 0;
}
