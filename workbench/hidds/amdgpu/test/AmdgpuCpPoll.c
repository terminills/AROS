/* Read, and optionally set, the GFX ring write-pointer poll control
 * (CP_RB_WPTR_POLL_CNTL) through amdgpu.hidd's public register ABI.
 *
 * On the RX 9060 XT DUT, an idle desktop keeps GRBM_STATUS.GUI_ACTIVE set
 * because the CP fetcher is busy polling the GFX queue's write pointer
 * (CP_CPF_BUSY_STAT.HQD_WPTR_POLL_BUSY), so the PMFW reports GFX 100% busy.
 * Linux programs POLL_FREQUENCY=0x100 and IDLE_POLL_COUNT=0x90 here
 * (gfx_v12_0.c, coarse-grain clock gating path); the AROS driver never does.
 *
 *   AmdgpuCpPoll          read the register
 *   AmdgpuCpPoll SET      write Linux's value 0x00900100, read back
 *   AmdgpuCpPoll VALUE=n  write n, read back (to restore a prior value)
 *   AmdgpuCpPoll REG=r [VALUE=n]  read (and write) one of the allowlisted GC0
 *                         registers below instead: 0x0f62 CP_RB_WPTR_POLL_CNTL,
 *                         0x1dfa CP_RB_DOORBELL_RANGE_LOWER,
 *                         0x1dfb CP_RB_DOORBELL_RANGE_UPPER. Linux programs the
 *                         range to [ring doorbell, 0xFFC]; the AROS driver
 *                         never writes it (reset 0..0x108 excludes the ring's
 *                         doorbell 0x458).
 *
 * Only for the known Navi 44 DUT: GC segment 0 base 0x1260 comes from its IP
 * discovery (as in GfxStateRead); other devices are refused.
 */
#define DEBUG 1
#define __INLINE_AMDGPU_STACKCALL__ 1
#include <aros/debug.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/amdgpu.h>
#include <stdio.h>

struct Library *AmdgpuBase;
#define LOG(...) do { printf("[AmdgpuCpPoll] " __VA_ARGS__); printf("\n"); \
                       bug("[AmdgpuCpPoll] " __VA_ARGS__); bug("\n"); } while (0)

#define DUT_GC0_BASE              0x1260UL
#define regCP_RB_WPTR_POLL_CNTL   0x0f62UL      /* BASE_IDX 0 */
#define LINUX_POLL_CNTL           0x00900100UL  /* IDLE_POLL_COUNT 0x90, POLL_FREQUENCY 0x100 */

int main(void)
{
    struct Library *resident;
    struct AmdgpuDevice *ad;
    struct AmdgpuGpuInfo info = { 0 };
    struct RDArgs *rd;
    IPTR args[3] = { 0, 0, 0 };
    ULONG rel = regCP_RB_WPTR_POLL_CNTL, reg, before, after, want = 0;
    BOOL write = FALSE;
    LONG result = RETURN_FAIL;

    rd = ReadArgs((CONST_STRPTR)"SET/S,VALUE/N,REG/N", args, NULL);
    if (rd) {
        if (args[0]) { write = TRUE; want = LINUX_POLL_CNTL; }
        if (args[1]) { write = TRUE; want = (ULONG)*(LONG *)args[1]; }
        if (args[2]) rel = (ULONG)*(LONG *)args[2];
        FreeArgs(rd);
    }
    if (rel != 0x0f62UL && rel != 0x1dfaUL && rel != 0x1dfbUL) {
        LOG("REFUSED: register 0x%04lx is not on the allowlist", (unsigned long)rel);
        return RETURN_FAIL;
    }
    if (args[0] && rel != regCP_RB_WPTR_POLL_CNTL) {
        LOG("REFUSED: SET applies only to CP_RB_WPTR_POLL_CNTL; use VALUE=");
        return RETURN_FAIL;
    }
    reg = DUT_GC0_BASE + rel;

    Forbid();
    resident = (struct Library *)FindName(&SysBase->LibList, "amdgpu.hidd");
    Permit();
    if (!resident) {
        LOG("REFUSED: driver is not already resident; no OpenLibrary attempted");
        return result;
    }
    AmdgpuBase = OpenLibrary("amdgpu.hidd", 0);
    if (!AmdgpuBase) return result;
    if (amdgpu_get_device_count() != 1 || !(ad = amdgpu_get_device(0))) {
        LOG("REFUSED: expected exactly one device");
        goto out;
    }
    amdgpu_query_gpu_info(ad, &info);
    if (info.asic_id != 0x7590 || info.gc_major != 12) {
        LOG("REFUSED: not the Navi 44 DUT (id=%04x GC %u)", info.asic_id, info.gc_major);
        goto out;
    }
    before = amdgpu_reg_read(ad, reg);
    LOG("GC0+0x%04lx @DW%05lx = %08lx", (unsigned long)rel, (unsigned long)reg,
        (unsigned long)before);
    if (write) {
        amdgpu_reg_write(ad, reg, want);
        after = amdgpu_reg_read(ad, reg);
        LOG("wrote %08lx, read back %08lx (%s)", (unsigned long)want,
            (unsigned long)after, after == want ? "held" : "DIFFERENT");
    }
    result = RETURN_OK;
out:
    CloseLibrary(AmdgpuBase);
    return result;
}
