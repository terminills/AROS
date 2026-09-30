/*
 * amdgpu.hidd — GPU microcode / firmware loader.
 *
 * Pulled down from the radeonsi monolith (radeonsi_request_firmware.c) so the
 * base driver owns firmware loading; the GL (radeonsi) and Vulkan (radv) layers
 * build on top of it. Modern AMD GPUs (GFX9+/RDNA) cannot bring up their engines
 * without vendor microcode (gfx/sdma/rlc/mec/... .bin), so this belongs in the
 * foundation, next to device init / hw_init.
 *
 * Mirrors the nouveau request_firmware() shim: blobs live under the AROS firmware
 * tree (Devs/Firmware/amdgpu/) both in the build sysroot and on the live volume.
 */

#define DEBUG 1
#include <aros/debug.h>

#include <proto/exec.h>
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <devices/timer.h>
#include <utility/tagitem.h>
/* Keep firmware I/O independent of a generated DOSBase. AmdgpuMonitor opens the
 * driver after DOS and SYS: are available, but an explicit lazy open gives a
 * clean failure if the loader is ever called in an earlier context. */
#define __NOLIBBASE__
#include <proto/dos.h>
#undef  __NOLIBBASE__
#include <dos/dos.h>

#include "amdgpu_intern.h"

/* Keep the path builder independent of stdc.library. CopyMem is provided by exec. */
static ULONG fw_strlen(const char *s) { const char *p = s; while (*p) p++; return (ULONG)(p - s); }

/* ---- firmware-read watchdog ----------------------------------------------
 * Two DUT boots (2026-09-23 and 2026-09-24) stopped for good right after
 * "trying 'DEVS:Firmware/amdgpu/gc_12_0_0_me.bin'", each time just as
 * Poseidon's PoPo task started a PUBSCREEN handler. Serial goes silent and the
 * display loses signal, so there is no alert to read. The watchdog below turns
 * the next occurrence into evidence: if a firmware file operation has not
 * finished within FW_WATCHDOG_SECS, it prints every ready and waiting task
 * (name, state, wait/received signals) to serial. It uses only exec and
 * timer.device, so it still runs if DOS is what is blocked. */
#define FW_WATCHDOG_SECS  10
#define FW_WATCHDOG_MAX   96

struct FwWatch {
    volatile BOOL done;
    volatile ULONG step;          /* 1 open, 2 seek, 3 alloc, 4 read, 5 close */
    const char *name;
    struct Task *loader;
};

struct FwTaskShot { char name[28]; UBYTE state; ULONG sigwait, sigrecvd; APTR task; };

static void fw_watch_dump(struct FwWatch *w)
{
    static struct FwTaskShot shot[FW_WATCHDOG_MAX];
    struct Node *n;
    ULONG count = 0, i, j;

    Disable();
    for (i = 0; i < 2; i++) {
        struct List *l = i ? &SysBase->TaskWait : &SysBase->TaskReady;
        ForeachNode(l, n) {
            struct Task *tk = (struct Task *)n;
            struct FwTaskShot *s;
            if (count >= FW_WATCHDOG_MAX) break;
            s = &shot[count++];
            for (j = 0; j < sizeof(s->name) - 1 && tk->tc_Node.ln_Name &&
                        tk->tc_Node.ln_Name[j]; j++)
                s->name[j] = tk->tc_Node.ln_Name[j];
            s->name[j] = 0;
            s->state = tk->tc_State;
            s->sigwait = tk->tc_SigWait;
            s->sigrecvd = tk->tc_SigRecvd;
            s->task = tk;
        }
    }
    Enable();

    bug("[amdgpu-fw] *** WATCHDOG: '%s' step %lu (1 open 2 seek 3 alloc 4 read 5 close) "
        "not done after %lus; loader task %p. Task snapshot (%lu):\n",
        w->name, (unsigned long)w->step, (unsigned long)FW_WATCHDOG_SECS,
        w->loader, (unsigned long)count);
    for (i = 0; i < count; i++)
        bug("[amdgpu-fw]   %p st=%u wait=%08lx recvd=%08lx %s%s\n", shot[i].task,
            (unsigned)shot[i].state, (unsigned long)shot[i].sigwait,
            (unsigned long)shot[i].sigrecvd, shot[i].name,
            shot[i].task == (APTR)w->loader ? "  <- firmware loader" : "");
}

static void fw_watchdog_entry(struct FwWatch *w)
{
    struct MsgPort *port;
    struct timerequest *tr;
    ULONG s;
    BOOL fired = FALSE;

    if (!w) return;
    port = CreateMsgPort();
    tr = port ? (struct timerequest *)CreateIORequest(port, sizeof(*tr)) : NULL;
    if (!tr || OpenDevice("timer.device", UNIT_MICROHZ, (struct IORequest *)tr, 0)) {
        if (tr) DeleteIORequest((struct IORequest *)tr);
        if (port) DeleteMsgPort(port);
        return;
    }
    for (s = 0; s < FW_WATCHDOG_SECS * 4 && !w->done; s++) {
        tr->tr_node.io_Command = TR_ADDREQUEST;
        tr->tr_time.tv_secs = 0;
        tr->tr_time.tv_micro = 250000;
        DoIO((struct IORequest *)tr);
    }
    if (!w->done) {
        fw_watch_dump(w);
        fired = TRUE;
    }
    CloseDevice((struct IORequest *)tr);
    DeleteIORequest((struct IORequest *)tr);
    DeleteMsgPort(port);
    (void)fired;
}

BOOL amdgpu_request_firmware(const char *name, UBYTE **data, ULONG *size)
{
    static const char * const prefixes[] = {
        "",
        "DEVS:Firmware/amdgpu/",
        "DEVS:Firmware/",
        "SYS:Devs/Firmware/amdgpu/",
        "SYS:Devs/Firmware/",
        "BOOT:Devs/Firmware/amdgpu/",
        "BOOT:Devs/Firmware/",
    };
    struct DosLibrary *DOSBase;
    char path[512];
    BPTR fh = BNULL;
    LONG file_size;
    UBYTE *blob = NULL;
    ULONG i, plen, nlen, tlen;
    BOOL ok = FALSE;
    struct FwWatch *watch = NULL;

    if (!data || !size || !name || !name[0])
        return FALSE;

    *data = NULL;
    *size = 0;

    /* Firmware is deliberately disk-backed. If DOS is unavailable, defer instead
     * of silently falling back to a second, embedded copy that can become stale. */
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 0);
    if (DOSBase == NULL) {
        D(bug("[amdgpu-fw] dos.library not up yet; deferring firmware '%s'\n", name));
        return FALSE;
    }

    D(bug("[amdgpu-fw] request_firmware: name='%s'\n", name));

    watch = AllocMem(sizeof(*watch), MEMF_PUBLIC | MEMF_CLEAR);
    if (watch) {
        watch->name = name;
        watch->loader = FindTask(NULL);
        watch->step = 1;
        if (!NewCreateTask(TASKTAG_NAME, (IPTR)"amdgpu firmware watchdog",
                           TASKTAG_PRI, 5,
                           TASKTAG_PC, (IPTR)fw_watchdog_entry,
                           TASKTAG_ARG1, (IPTR)watch,
                           TASKTAG_STACKSIZE, 16384,
                           TAG_DONE)) {
            FreeMem(watch, sizeof(*watch));
            watch = NULL;
        }
    }

    for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        plen = fw_strlen(prefixes[i]);
        nlen = fw_strlen(name);
        tlen = plen + nlen;
        if (tlen >= sizeof(path))
            continue;
        if (plen)
            CopyMem((APTR)prefixes[i], path, plen);
        CopyMem((APTR)name, path + plen, nlen);
        path[tlen] = '\0';

        D(bug("[amdgpu-fw] trying '%s'\n", path));
        fh = Open(path, MODE_OLDFILE);
        if (fh != BNULL)
            break;
    }

    if (fh == BNULL) {
        D(bug("[amdgpu-fw] '%s' not found in any prefix\n", name));
        goto out;
    }

    if (watch) watch->step = 2;
    if (Seek(fh, 0, OFFSET_END) < 0) {
        D(bug("[amdgpu-fw] seek-end failed path='%s' ioerr=%ld\n", path, (long)IoErr()));
        goto out_close;
    }
    file_size = Seek(fh, 0, OFFSET_CURRENT);
    if (file_size <= 0) {
        D(bug("[amdgpu-fw] size query failed path='%s' size=%ld ioerr=%ld\n",
              path, (long)file_size, (long)IoErr()));
        goto out_close;
    }
    if (Seek(fh, 0, OFFSET_BEGINNING) < 0) {
        D(bug("[amdgpu-fw] seek-begin failed path='%s' ioerr=%ld\n", path, (long)IoErr()));
        goto out_close;
    }

    if (watch) watch->step = 3;
    blob = AllocVec((ULONG)file_size, MEMF_PUBLIC);
    if (!blob) {
        D(bug("[amdgpu-fw] alloc failed path='%s' size=%ld\n", path, (long)file_size));
        goto out_close;
    }

    if (watch) watch->step = 4;
    if (Read(fh, blob, (ULONG)file_size) != file_size) {
        D(bug("[amdgpu-fw] read short path='%s' want=%ld ioerr=%ld\n",
              path, (long)file_size, (long)IoErr()));
        FreeVec(blob);
        blob = NULL;
        goto out_close;
    }

    *data = blob;
    *size = (ULONG)file_size;
    ok = TRUE;
    D(bug("[amdgpu-fw] loaded '%s' (%lu bytes)\n", path, (unsigned long)file_size));

out_close:
    if (watch) watch->step = 5;
    Close(fh);
out:
    CloseLibrary((struct Library *)DOSBase);
    /* Stop the watchdog: it sees done within 250 ms and exits. The record is
     * deliberately never freed, since the watchdog may read it once more
     * before it ends; it is a few dozen bytes per firmware file. */
    if (watch)
        watch->done = TRUE;
    return ok;
}

VOID amdgpu_release_firmware(UBYTE *data)
{
    if (data)
        FreeVec(data);
}
