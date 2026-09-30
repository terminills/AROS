/*
 * amdgpu.hidd — SMU (MP1) mailbox bring-up. Stage 3 P0: sign of life.
 *
 * RDNA4's VBIOS strips the ATOM display command tables (SetPixelClock /
 * SetCRTC_Timing / EnableCRTC / DIG1EncoderControl all MISSING — proven by the
 * ATOM command-table survey in amdgpu_display.c), so the display PIXEL CLOCK
 * cannot be set the ATOM way. It must go through the SMU firmware. This is the
 * first step of that route: talk to the SMU over the MP1 C2PMSG mailbox — the
 * same mechanism as the PSP/MP0 mailbox in amdgpu_psp.c — and read the SMU
 * version. That is the sign of life proving the mailbox works before we ever
 * send a clock-setting message. GetSmuVersion is a QUERY; it changes nothing.
 *
 * Mailbox registers: MP1 C2PMSG_N is a DWORD offset (0x40 + N) added to the
 * discovered MP1 base (ad->ip.smu.base) — identical base scheme to the PSP
 * (MP0_C2PMSG_35 = 0x40 + 35 = 0x63, amdgpu_psp.c:44). The smu_v13_0/smu_v14_0
 * message protocol uses C2PMSG_66 (message), C2PMSG_82 (parameter / return),
 * C2PMSG_90 (response; 1 = success). Message IDs are the common PPSMC set.
 * These offsets are the highest-uncertainty part (which smu_vXX register bank) —
 * the raw registers are logged first so a boot shows whether the base is live.
 */

#define DEBUG 1
#include <aros/debug.h>

#include <proto/exec.h>
#include <string.h>
#include <errno.h>
#include <hidd/amdgpu.h>
#include <aros/bootloader.h>
#include <proto/bootloader.h>
#include <exec/execbase.h>
#include <exec/memory.h>

#include "amdgpu_intern.h"

/* MP1 C2PMSG mailbox — dword offsets from ad->ip.smu.base (C2PMSG_N = 0x40 + N,
 * same as the PSP MP0 mailbox). smu_v13_0/smu_v14_0 register bank. */
#define MP1_C2PMSG_66       (0x40 + 66)   /* message register            */
#define MP1_C2PMSG_82       (0x40 + 82)   /* parameter / return value    */
#define MP1_C2PMSG_90       (0x40 + 90)   /* response (1 = success)      */

#define SMU_RESP_OK         0x1UL

#define PPSMC_MSG_GetSmuVersion              0x2
#define PPSMC_MSG_GetDriverIfVersion         0x3

/* SMU feature messages (smu_v14_0_2_ppsmc.h in Linux, verified for
 * Navi44/smu_14_0_2). Linux smu_v14_0_set_allowed_mask() sends High then Low with
 * the allowed-feature bitmap during smu_smc_hw_setup(), but only when SCPM is
 * disabled (PSP programs the allowed mask under SCPM).
 *
 * CORRECTION (verified vs Linux v6.12): EnableAllSmuFeatures is sent
 * UNCONDITIONALLY during this init path — smu_smc_hw_setup (amdgpu_smu.c:1667)
 * -> smu_v14_0_system_features_control (smu_v14_0.c:785) -> EnableAllSmuFeatures,
 * SCPM or not. It is the message that actually turns features ON; without it PMFW
 * enables nothing and GetRunningSmuFeatures reads 0, leaving FEATURE_DPM_GFXCLK
 * (bit1) + FEATURE_GFX_IMU (bit36) off and the discrete GFX rail unpowered — the
 * observed wall. The prior "Linux does not send EnableAllSmuFeatures" was wrong. */
#define PPSMC_MSG_SetAllowedFeaturesMaskLow   0x4
#define PPSMC_MSG_SetAllowedFeaturesMaskHigh  0x5
#define PPSMC_MSG_EnableAllSmuFeatures        0x6
#define PPSMC_MSG_DisableAllSmuFeatures       0x7
#define PPSMC_MSG_GetRunningSmuFeaturesLow    0xC
#define PPSMC_MSG_GetRunningSmuFeaturesHigh   0xD

/* GFX-power feature bits (smu14_driver_if_v14_0.h) that gate the discrete GFX
 * rail once EnableAllSmuFeatures runs — decoded in the post-enable log. */
#define SMU14_FEATURE_DPM_GFXCLK_BIT          1
#define SMU14_FEATURE_GFX_IMU_BIT             36

/* Linux smu_smc_hw_setup() registers the driver and tools table locations before
 * SetAllowedFeaturesMask. SMU 14.0.2 is a deliberate exception to the normal
 * PPTable write path: Linux commit 136ce12bd590 removed its write_pptable
 * callback because PMFW/IFWI owns the authoritative table. IDs are from
 * smu_v14_0_2_ppsmc.h. */
#define PPSMC_MSG_SetDriverDramAddrHigh       0xE
#define PPSMC_MSG_SetDriverDramAddrLow        0xF
#define PPSMC_MSG_SetToolsDramAddrHigh        0x10
#define PPSMC_MSG_SetToolsDramAddrLow         0x11
#define PPSMC_MSG_TransferTableSmu2Dram       0x12
#define PPSMC_MSG_UseDefaultPPTable           0x14
#define PPSMC_MSG_RunDcBtc                    0x36

#define SMU_TABLE_COMBO_PPTABLE               1
#define SMU_COMBO_PPTABLE_SIZE                0x4000
#define SMU14_DRIVER_IF_VERSION               0x2E

#define PPSMC_Result_OK                       0x1
#define PPSMC_Result_CmdRejectedPrereq        0xFD  /* precondition not met     */
#define PPSMC_Result_CmdRejectedBusy          0xFC
#define PPSMC_Result_UnknownCmd               0xFE

/* Navi44 fixed VRAM MC base (== amdgpu_gfx_support.c AMDGPU_VRAM_MC_BASE); the SMU
 * accepts VRAM MC addresses (SMC/IMU fw loads at VRAM MC succeed). A zeroed driver
 * table lives at this offset — clear of the GART table (512MiB), autoload (320MiB)
 * and PSP (264MiB) regions. */
#define SMU_VRAM_MC_BASE      0x8000000000ULL
#define SMU_DRV_TABLE_VRAM_OFF ((UQUAD)576 << 20)   /* 0x24000000 */
#define SMU_DRV_TABLE_SIZE    0x10000               /* 64 KiB, zero-filled */
#define SMU_TOOL_TABLE_VRAM_OFF ((UQUAD)640 << 20)  /* 0x28000000 (tools/PMSTATUSLOG) */

/* PSP runtime database at the top of VRAM. Linux reads the SCPM status from
 * entry type 6 before SMU setup; when enabled, PSP owns the signed PPTable and
 * host PPTable uploads plus allowed-feature-mask messages are not permitted. */
#define PSP_RUNTIME_DB_OFFSET                   0x100000ULL
#define PSP_RUNTIME_DB_SIZE                     0x10000UL
#define PSP_RUNTIME_DB_COOKIE                   0x0ED5UL
#define PSP_RUNTIME_DB_VERSION_1                0x0100UL
#define PSP_RUNTIME_DB_MAX_ENTRIES              0x40UL
#define PSP_RUNTIME_DB_DIRECTORY_HEADER_SIZE    4UL
#define PSP_RUNTIME_ENTRY_PPTABLE_ERR_STATUS    0x6UL
#define PSP_SCPM_DISABLED                       0x0UL
#define PSP_SCPM_ENABLED                        0x1UL
#define PSP_SCPM_ENABLED_WITH_ERROR             0x2UL

/* Stage SMU tables in page-aligned system RAM. DMA_ReadFromRAM comes from the
 * Exec ABI (execbase.h); do not duplicate its bit value in a driver. */
#define SMU_SYS_TABLE_ALIGN   0x1000   /* 4KB-align the system-RAM table */

/* Resolve the MP1/SMU register base. The IP-discovery parse keeps only base0 for
 * the combined MP block, so ad->ip.smu.base comes back as MP0's base (0x16000 on
 * Navi 44) — the same value as ad->ip.psp.base. The MP block's base list is
 * 16000(MP0) 16200(MP1) …, i.e. MP1 = MP0 + 0x200 on Navi 4x. Detect that
 * collision and apply the offset; if discovery ever yields a distinct MP1 base,
 * use it as-is. */
ULONG amdgpu_smu_base(struct AmdgpuDevice *ad)
{
    ULONG base = ad->ip.smu.base;

    if (base && ad->ip.psp.valid && base == ad->ip.psp.base)
        base += 0x200;
    return base;
}

/* little-endian reads from a firmware byte buffer (mirrors amdgpu_psp.c le32). */
static inline ULONG amdgpu_smu_le32(const UBYTE *b)
{
    return (ULONG)b[0] | ((ULONG)b[1] << 8) | ((ULONG)b[2] << 16) | ((ULONG)b[3] << 24);
}
static inline ULONG amdgpu_smu_le16(const UBYTE *b)
{
    return (ULONG)b[0] | ((ULONG)b[1] << 8);
}

/* Read PSP's runtime DB directly through the established VRAM aperture. Parse
 * the little-endian ABI explicitly instead of overlaying native structs. Linux's
 * psp_runtime_data_directory has a UWORD entry_count followed by two bytes of
 * alignment padding, so entry_list starts at byte 4, not byte 2. Returns TRUE
 * only when a bounded, valid SCPM entry was found. */
static BOOL amdgpu_smu_get_scpm_status(struct AmdgpuDevice *ad, ULONG *status)
{
    ULONG header = 0;
    ULONG directory[(PSP_RUNTIME_DB_DIRECTORY_HEADER_SIZE +
                     PSP_RUNTIME_DB_MAX_ENTRIES * 8 + 3) / 4];
    UBYTE *dir = (UBYTE *)directory;
    UQUAD vram_size;
    UQUAD db_pos;
    ULONG cookie, version, count, i;

    if (status)
        *status = PSP_SCPM_DISABLED;
    if (!ad || !ad->vram_mb)
        return FALSE;

    vram_size = (UQUAD)ad->vram_mb << 20;
    if (vram_size <= PSP_RUNTIME_DB_OFFSET)
        return FALSE;
    db_pos = vram_size - PSP_RUNTIME_DB_OFFSET;

    amdgpu_vram_read(ad, db_pos, &header, 1);
    cookie = header & 0xFFFFUL;
    version = (header >> 16) & 0xFFFFUL;
    bug("[amdgpu-smu] PSP runtime DB @VRAM+0x%08lx%08lx: cookie=0x%04lx version=0x%04lx\n",
        (unsigned long)(db_pos >> 32), (unsigned long)(db_pos & 0xFFFFFFFFUL),
        (unsigned long)cookie, (unsigned long)version);
    if (cookie != PSP_RUNTIME_DB_COOKIE) {
        bug("[amdgpu-smu] PSP runtime DB absent (expected cookie=0x%04lx)\n",
            (unsigned long)PSP_RUNTIME_DB_COOKIE);
        return FALSE;
    }

    amdgpu_vram_read(ad, db_pos + sizeof(header), directory,
                     sizeof(directory) / sizeof(directory[0]));
    count = amdgpu_smu_le16(dir);
    if (count >= PSP_RUNTIME_DB_MAX_ENTRIES) {
        bug("[amdgpu-smu] PSP runtime DB invalid entry count=%lu (max exclusive=%lu)\n",
            (unsigned long)count, (unsigned long)PSP_RUNTIME_DB_MAX_ENTRIES);
        return FALSE;
    }
    bug("[amdgpu-smu] PSP runtime DB directory: entries=%lu%s\n",
        (unsigned long)count,
        (version == PSP_RUNTIME_DB_VERSION_1) ? "" : " !!UNKNOWN-VERSION");

    for (i = 0; i < count; i++) {
        const UBYTE *entry = dir + PSP_RUNTIME_DB_DIRECTORY_HEADER_SIZE + i * 8;
        ULONG type = amdgpu_smu_le32(entry);
        ULONG off = amdgpu_smu_le16(entry + 4);
        ULONG size = amdgpu_smu_le16(entry + 6);

        bug("[amdgpu-smu] PSP runtime DB entry %lu: type=%lu off=0x%lx size=%lu\n",
            (unsigned long)i, (unsigned long)type, (unsigned long)off,
            (unsigned long)size);

        if (type == PSP_RUNTIME_ENTRY_PPTABLE_ERR_STATUS) {
            ULONG words[2] = { 0, 0 };
            ULONG shift = off & 3;
            ULONG scpm;

            if (size < sizeof(ULONG) || off > PSP_RUNTIME_DB_SIZE - sizeof(ULONG)) {
                bug("[amdgpu-smu] PSP SCPM entry invalid: off=0x%lx size=%lu\n",
                    (unsigned long)off, (unsigned long)size);
                return FALSE;
            }
            amdgpu_vram_read(ad, db_pos + (off & ~3UL), words, 2);
            scpm = amdgpu_smu_le32((const UBYTE *)words + shift);
            if (status)
                *status = scpm;
            bug("[amdgpu-smu] *** PSP SCPM status=%lu (%s) entry=%lu off=0x%lx size=%lu ***\n",
                (unsigned long)scpm,
                (scpm == PSP_SCPM_DISABLED) ? "disabled" :
                (scpm == PSP_SCPM_ENABLED) ? "enabled" :
                (scpm == PSP_SCPM_ENABLED_WITH_ERROR) ? "enabled-with-error" :
                "unknown",
                (unsigned long)i, (unsigned long)off, (unsigned long)size);
            return TRUE;
        }
    }

    bug("[amdgpu-smu] PSP runtime DB has no SCPM status entry\n");
    return FALSE;
}

/* Crude busy-delay — no timer.device at driver init (mirrors psp_spin). */
static void smu_spin(ULONG loops)
{
    volatile ULONG i = 0;
    while (i < loops)
        i++;
}

/* Send one SMU message + arg over the MP1 mailbox and poll the response. Returns
 * the RAW response status from C2PMSG_90 (SMU_RESP_OK==1, 0xFD/0xFE/0xFF errors,
 * or 0 if the SMU never answered / mailbox not ready), and fills *result from
 * C2PMSG_82 (the message's return VALUE — e.g. the version for GetSmuVersion).
 * Callers must not conflate the two: status lives in C2PMSG_90, the return value
 * in C2PMSG_82. Bounded poll — a wedged SMU never hangs the boot. */
/* The MP1 mailbox is one set of registers, and a message is a multi-step
 * write/poll sequence, so two tasks must never interleave. The boot path is
 * serial; the lock matters once metrics and clock control are reachable at
 * run time. Initialized once by amdgpu_smu_hw_init (serial device ladder). */
static struct SignalSemaphore smu_mbox_lock;
static BOOL smu_mbox_lock_ready;

static ULONG amdgpu_smu_send_msg_locked(struct AmdgpuDevice *ad, ULONG msg,
                                        ULONG arg, ULONG *result);

ULONG amdgpu_smu_send_msg(struct AmdgpuDevice *ad, ULONG msg, ULONG arg,
                                ULONG *result)
{
    ULONG r;

    if (smu_mbox_lock_ready)
        ObtainSemaphore(&smu_mbox_lock);
    r = amdgpu_smu_send_msg_locked(ad, msg, arg, result);
    if (smu_mbox_lock_ready)
        ReleaseSemaphore(&smu_mbox_lock);
    return r;
}

static ULONG amdgpu_smu_send_msg_locked(struct AmdgpuDevice *ad, ULONG msg,
                                        ULONG arg, ULONG *result)
{
    ULONG base    = amdgpu_smu_base(ad);
    ULONG resp_reg = base + MP1_C2PMSG_90;
    ULONG arg_reg  = base + MP1_C2PMSG_82;
    ULONG msg_reg  = base + MP1_C2PMSG_66;
    ULONG ready = 0, resp = 0, t;

    /* 1. Mailbox is ready for a NEW command once the prior command has FINISHED,
     *    i.e. the response register is non-zero — regardless of whether that prior
     *    result was OK (1) or an error (0xFF/0xFE/0xFD/0xFC). The earlier code
     *    required resp==1 here, so the moment any command returned a non-OK code
     *    (e.g. 0xFD CmdRejectedPrereq) every subsequent message bailed with
     *    "not ready" and never actually sent. Gate on non-zero (Linux
     *    __smu_cmn_poll_stat semantics: wait for resp != 0). */
    /* Each iteration costs ~1us (dominated by the MMIO read), so 2,000,000 ~= 2s.
     * EnableAllSmuFeatures ramps all DPM/clock features and can take well over a
     * second; the prior 200,000 (~0.2s) ready-wait timed out the FOLLOWING message
     * while the SMU was still busy with the enable. Give a multi-second window. */
    for (t = 0; t < 2000000; t++) {
        ready = amdgpu_rreg32(ad, resp_reg);
        if (ready != 0)
            break;
        smu_spin(100);
    }
    if (ready == 0) {
        bug("[amdgpu-smu] mailbox not ready: resp stayed 0 after %lu spins\n",
            (unsigned long)t);
        return 0;
    }
    if (ready != SMU_RESP_OK)
        bug("[amdgpu-smu] prior cmd resp=0x%08lx (non-OK) — mailbox free, sending next\n",
            (unsigned long)ready);

    /* 2. Force a command-register edge: SMU14 firmware can leave the prior opcode
     *    in C2PMSG_66, and re-writing the same value may not re-trigger. A 0 write
     *    + posting read guarantees a clean edge (radv winsys line 1242). */
    amdgpu_wreg32(ad, msg_reg, 0);
    (void)amdgpu_rreg32(ad, msg_reg);

    /* 3. Clear response, load the parameter, fire the message — each followed by a
     *    posting read so the writes commit in order (no write-combine reorder). */
    amdgpu_wreg32(ad, resp_reg, 0);
    amdgpu_wreg32(ad, arg_reg, arg);
    amdgpu_wreg32(ad, msg_reg, msg);
    {
        /* Prove the message + arg writes actually COMMIT to the mailbox (distinguish
         * "write dropped" from "SMU not servicing"). arg uses a sentinel so a 0-arg
         * message is still verifiable. */
        ULONG mb = amdgpu_rreg32(ad, msg_reg);
        ULONG ab = amdgpu_rreg32(ad, arg_reg);
        bug("[amdgpu-smu] wrote msg=0x%02lx -> readback 0x%08lx ; arg=0x%lx -> readback 0x%08lx\n",
            (unsigned long)msg, (unsigned long)mb, (unsigned long)arg, (unsigned long)ab);
    }

    /* 4. Poll for a response — the SMU can be slow, especially the first message
     *    after OS handover, and EnableAllSmuFeatures (ramping every DPM/clock
     *    feature) is the heaviest: it accepts by clearing resp=0 then can take
     *    seconds to complete. ~1us/iter (MMIO-read-dominated), so 8,000,000 ~= 8s.
     *    The prior 2,000,000 (~2s) timed out mid-enable, leaving RunningFeatures=0. */
    for (t = 0; t < 8000000; t++) {
        resp = amdgpu_rreg32(ad, resp_reg);
        if (resp != 0)
            break;
        smu_spin(50);
    }
    if (result)
        *result = amdgpu_rreg32(ad, arg_reg);
    bug("[amdgpu-smu] msg=0x%02lx arg=0x%lx -> resp=0x%08lx after %lu spins ; "
        "post 66=0x%08lx 82=0x%08lx 90=0x%08lx\n",
        (unsigned long)msg, (unsigned long)arg, (unsigned long)resp, (unsigned long)t,
        (unsigned long)amdgpu_rreg32(ad, msg_reg),
        (unsigned long)amdgpu_rreg32(ad, arg_reg),
        (unsigned long)amdgpu_rreg32(ad, resp_reg));
    return resp;   /* raw C2PMSG_90 status (0 = no response) */
}

/* Mirror the relevant Linux smu_smc_hw_setup() ordering: register driver/tools
 * tables, run DC BTC, and set the allowed masks only when SCPM is disabled.
 * SMU 14.0.2 leaves PPTable ownership with PMFW/IFWI. */
/* Decode a raw response code for the log. */
static const char *smu_resp_str(ULONG r)
{
    switch (r) {
    case 0:                              return "NO-RESPONSE/TIMEOUT";
    case PPSMC_Result_OK:                return "OK";
    case PPSMC_Result_CmdRejectedPrereq: return "REJECT:PREREQ(0xFD)";
    case PPSMC_Result_CmdRejectedBusy:   return "REJECT:BUSY(0xFC)";
    case PPSMC_Result_UnknownCmd:        return "REJECT:UNKNOWN-CMD(0xFE)";
    case 0xFFUL:                         return "REJECT:FAILED(0xFF)";
    default:                             return "REJECT:?";
    }
}

/* SMU14 message IDs and clock/feature indices used by the DPM policy
 * (smu_v14_0_2_ppsmc.h, smu14_driver_if_v14_0.h). */
#define PPSMC_MSG_SetSoftMinByFreq            0x19
#define PPSMC_MSG_SetSoftMaxByFreq            0x1A
#define PPSMC_MSG_SetHardMinByFreq            0x1B
#define PPSMC_MSG_GetMinDpmFreq               0x1D
#define PPSMC_MSG_GetMaxDpmFreq               0x1E
#define SMU14_PPCLK_GFXCLK                    0
#define SMU14_PPCLK_SOCCLK                    1
#define SMU14_PPCLK_UCLK                      2
#define SMU14_PPCLK_FCLK                      3
#define SMU14_FEATURE_DPM_UCLK_BIT            3
#define SMU14_FEATURE_DPM_FCLK_BIT            4
#define SMU14_FEATURE_DPM_SOCCLK_BIT          5

/* AMDGPU=gfxclkmax on the kernel command line: the pre-2026-09-24 pin of
 * GFXCLK to its DPM maximum, kept for A/B runs on hardware. */
static BOOL amdgpu_smu_gfxclk_pin_requested(void)
{
    struct BootLoaderBase *BootLoaderBase;
    struct List *list;
    struct Node *node;

    if (!(BootLoaderBase = OpenResource("bootloader.resource")))
        return FALSE;
    if (!(list = (struct List *)GetBootInfo(BL_Args)))
        return FALSE;
    ForeachNode(list, node)
    {
        if (node->ln_Name && strncmp(node->ln_Name, "AMDGPU=", 7) == 0 &&
            strstr(node->ln_Name + 7, "gfxclkmax"))
            return TRUE;
    }
    return FALSE;
}

static void amdgpu_smu_auto_range(struct AmdgpuDevice *ad, ULONG clk,
                                  const char *name, ULONG running_low,
                                  ULONG feature_bit)
{
    ULONG fmin = 0, fmax = 0, v, rmax, rmin;

    if (!(running_low & (1UL << feature_bit))) {
        bug("[amdgpu-smu] DPM %s: feature not running, range left to PMFW\n",
            name);
        return;
    }
    (void)amdgpu_smu_send_msg(ad, PPSMC_MSG_GetMinDpmFreq, clk << 16, &fmin);
    (void)amdgpu_smu_send_msg(ad, PPSMC_MSG_GetMaxDpmFreq, clk << 16, &fmax);
    rmax = amdgpu_smu_send_msg(ad, PPSMC_MSG_SetSoftMaxByFreq,
                               (clk << 16) | 0xFFFF, &v);
    rmin = amdgpu_smu_send_msg(ad, PPSMC_MSG_SetSoftMinByFreq,
                               (clk << 16) | 0, &v);
    bug("[amdgpu-smu] DPM %s AUTO: table %lu..%lu MHz, SoftMax %s SoftMin %s\n",
        name, (unsigned long)fmin, (unsigned long)fmax,
        smu_resp_str(rmax), smu_resp_str(rmin));
}

static void amdgpu_smu_apply_dpm_policy(struct AmdgpuDevice *ad,
                                        ULONG running_low)
{
    amdgpu_smu_auto_range(ad, SMU14_PPCLK_GFXCLK, "GFXCLK", running_low,
                          SMU14_FEATURE_DPM_GFXCLK_BIT);
    amdgpu_smu_auto_range(ad, SMU14_PPCLK_SOCCLK, "SOCCLK", running_low,
                          SMU14_FEATURE_DPM_SOCCLK_BIT);
    amdgpu_smu_auto_range(ad, SMU14_PPCLK_UCLK, "UCLK", running_low,
                          SMU14_FEATURE_DPM_UCLK_BIT);
    amdgpu_smu_auto_range(ad, SMU14_PPCLK_FCLK, "FCLK", running_low,
                          SMU14_FEATURE_DPM_FCLK_BIT);

    if (amdgpu_smu_gfxclk_pin_requested()) {
        ULONG gmax = 0, v, rh, rs;

        (void)amdgpu_smu_send_msg(ad, PPSMC_MSG_GetMaxDpmFreq,
                                  SMU14_PPCLK_GFXCLK << 16, &gmax);
        if (gmax) {
            ULONG a = (SMU14_PPCLK_GFXCLK << 16) | (gmax & 0xFFFF);
            rh = amdgpu_smu_send_msg(ad, PPSMC_MSG_SetHardMinByFreq, a, &v);
            rs = amdgpu_smu_send_msg(ad, PPSMC_MSG_SetSoftMinByFreq, a, &v);
            bug("[amdgpu-smu] AMDGPU=gfxclkmax: GFXCLK pinned to %lu MHz "
                "(HardMin %s SoftMin %s)\n", (unsigned long)gmax,
                smu_resp_str(rh), smu_resp_str(rs));
        }
    }
}

/* ---- SMU metrics (TABLE_SMU_METRICS) --------------------------------------
 * Offsets are SmuMetrics_t from smu14_driver_if_v14_0.h, measured by compiling
 * that header (PPCLK_COUNT 11, SVI_PLANE_COUNT 4, TEMP_COUNT 12,
 * THROTTLER_COUNT 21; sizeof SmuMetricsExternal_t 412). The PMFW copies the
 * table into the driver table with TransferTableSmu2Dram(5), as
 * smu_cmn_update_table() does for SMU_TABLE_SMU_METRICS. */
#define SMU14_TABLE_SMU_METRICS        5
#define SMU14_METRICS_EXT_SIZE         412
#define SMUM_CURRCLOCK                 0     /* ULONG[11], MHz */
#define SMUM_AVG_GFXCLK_POSTDS         48
#define SMUM_AVG_FCLK_POSTDS           52
#define SMUM_AVG_MEMCLK_POSTDS         56
#define SMUM_METRICS_COUNTER           104
#define SMUM_AVG_GFX_ACTIVITY          124
#define SMUM_AVG_UCLK_ACTIVITY         126
#define SMUM_AVG_SOCKET_POWER          136
#define SMUM_AVG_TOTAL_BOARD_POWER     138
#define SMUM_AVG_TEMPERATURE           140   /* UWORD[12]: EDGE 0, HOTSPOT 1, MEM 4 */
#define SMUM_AVG_FAN_PWM               168
#define SMUM_AVG_FAN_RPM               170

static BOOL smu_metrics_registered;
#define SMUM_SENTINEL 0xA5A5A5A5UL
/* [0] samples delivered, [1] acknowledged transfers that never landed */
static ULONG smu_metrics_stats[2];

static UWORD smum_le16(const UBYTE *b) { return (UWORD)(b[0] | (b[1] << 8)); }

BOOL amdgpu_smu_read_metrics(struct AmdgpuDevice *ad, struct AmdgpuSmuMetrics *m)
{
    /* The PMFW acknowledges a transfer into a host system-RAM address but
     * writes nothing there (boot log: "changed=0/16384"); into VRAM it
     * delivers. So metrics use the reserved VRAM driver table, as Linux
     * allocates its driver table in VRAM, and are read back through the
     * indirect VRAM aperture. */
    const UQUAD tbl_mc = SMU_VRAM_MC_BASE + SMU_DRV_TABLE_VRAM_OFF;
    ULONG raw[(SMU14_METRICS_EXT_SIZE + 3) / 4];
    ULONG val, st;
    const UBYTE *b = (const UBYTE *)raw;
    BOOL ok = FALSE;

    if (!ad || !m || !ad->rmmio || !ad->ip.smu.valid)
        return FALSE;
    if (smu_mbox_lock_ready)
        ObtainSemaphore(&smu_mbox_lock);

    if (!smu_metrics_registered) {
        if (amdgpu_smu_send_msg_locked(ad, PPSMC_MSG_SetDriverDramAddrHigh,
                                       (ULONG)(tbl_mc >> 32), &val) != SMU_RESP_OK ||
            amdgpu_smu_send_msg_locked(ad, PPSMC_MSG_SetDriverDramAddrLow,
                                       (ULONG)(tbl_mc & 0xFFFFFFFFUL), &val) != SMU_RESP_OK)
            goto out;
        smu_metrics_registered = TRUE;
    }
    /* The PMFW sometimes acknowledges TransferTableSmu2Dram without the table
     * having landed yet (seen on the DUT: 3 of 10 samples read the slot's old
     * contents). Fill the slot with a sentinel first, then accept the sample
     * only once the counter word no longer holds it; retry the transfer if it
     * never changes. */
    {
        ULONG attempt, poll, n = sizeof(raw) / sizeof(raw[0]), i;

        for (attempt = 0; attempt < 3 && !ok; attempt++) {
            for (i = 0; i < n; i++)
                raw[i] = SMUM_SENTINEL;
            amdgpu_vram_write(ad, SMU_DRV_TABLE_VRAM_OFF, raw, n);
            st = amdgpu_smu_send_msg_locked(ad, PPSMC_MSG_TransferTableSmu2Dram,
                                            SMU14_TABLE_SMU_METRICS, &val);
            if (st != SMU_RESP_OK)
                continue;
            for (poll = 0; poll < 200 && !ok; poll++) {
                amdgpu_vram_read(ad, SMU_DRV_TABLE_VRAM_OFF, raw, n);
                if (raw[SMUM_METRICS_COUNTER / 4] != SMUM_SENTINEL &&
                    raw[SMUM_AVG_TEMPERATURE / 4] != SMUM_SENTINEL)
                    ok = TRUE;
                else
                    smu_spin(1000);
            }
            smu_metrics_stats[ok ? 0 : 1]++;
            if (!ok)
                bug("[amdgpu-smu] metrics: transfer %lu acknowledged but table "
                    "not delivered after %lu polls\n",
                    (unsigned long)attempt + 1, (unsigned long)poll);
        }
        if (!ok)
            goto out;
    }

    m->gfxclk_cur    = amdgpu_smu_le32(b + SMUM_CURRCLOCK + 4 * SMU14_PPCLK_GFXCLK);
    m->socclk_cur    = amdgpu_smu_le32(b + SMUM_CURRCLOCK + 4 * SMU14_PPCLK_SOCCLK);
    m->uclk_cur      = amdgpu_smu_le32(b + SMUM_CURRCLOCK + 4 * SMU14_PPCLK_UCLK);
    m->fclk_cur      = amdgpu_smu_le32(b + SMUM_CURRCLOCK + 4 * SMU14_PPCLK_FCLK);
    m->gfxclk_avg    = smum_le16(b + SMUM_AVG_GFXCLK_POSTDS);
    m->fclk_avg      = smum_le16(b + SMUM_AVG_FCLK_POSTDS);
    m->uclk_avg      = smum_le16(b + SMUM_AVG_MEMCLK_POSTDS);
    m->counter       = amdgpu_smu_le32(b + SMUM_METRICS_COUNTER);
    m->gfx_activity  = smum_le16(b + SMUM_AVG_GFX_ACTIVITY);
    m->uclk_activity = smum_le16(b + SMUM_AVG_UCLK_ACTIVITY);
    m->socket_power  = smum_le16(b + SMUM_AVG_SOCKET_POWER);
    m->board_power   = smum_le16(b + SMUM_AVG_TOTAL_BOARD_POWER);
    m->temp_edge     = smum_le16(b + SMUM_AVG_TEMPERATURE + 2 * 0);
    m->temp_hotspot  = smum_le16(b + SMUM_AVG_TEMPERATURE + 2 * 1);
    m->temp_mem      = smum_le16(b + SMUM_AVG_TEMPERATURE + 2 * 4);
    m->fan_pwm       = b[SMUM_AVG_FAN_PWM];
    m->fan_rpm       = smum_le16(b + SMUM_AVG_FAN_RPM);
out:
    if (smu_mbox_lock_ready)
        ReleaseSemaphore(&smu_mbox_lock);
    return ok;
}

/* Read the PMFW state before changing it, then ask PMFW to DMA its combo power
 * table into a sentinel buffer. Linux uses this reverse transfer to obtain the
 * board-specific table on Navi 44. A successful response plus changed bytes
 * proves the SMU's system-memory DMA path is alive; a failed host upload then
 * points at command policy/table validity rather than a dead shared DMA engine.
 * This probe restores the DMA mapping and frees the temporary allocation before
 * the persistent driver/tools table addresses are registered. */
static BOOL amdgpu_smu_probe_protocol(struct AmdgpuDevice *ad)
{
    ULONG st, val = 0, running_low = 0, running_high = 0;
    IPTR raw;
    BOOL vram_pptable_staged = FALSE;

    st = amdgpu_smu_send_msg(ad, PPSMC_MSG_GetDriverIfVersion, 0, &val);
    bug("[amdgpu-smu] GetDriverIfVersion -> %s (resp=0x%08lx value=0x%08lx; "
        "Linux driver IF=0x%02lx)%s\n",
        smu_resp_str(st), (unsigned long)st, (unsigned long)val,
        (unsigned long)SMU14_DRIVER_IF_VERSION,
        (st == PPSMC_Result_OK && val != SMU14_DRIVER_IF_VERSION)
            ? " (tolerated firmware version skew)" : "");

    st = amdgpu_smu_send_msg(ad, PPSMC_MSG_GetRunningSmuFeaturesLow, 0,
                             &running_low);
    bug("[amdgpu-smu] GetRunningSmuFeaturesLow -> %s (resp=0x%08lx mask=0x%08lx)\n",
        smu_resp_str(st), (unsigned long)st, (unsigned long)running_low);
    st = amdgpu_smu_send_msg(ad, PPSMC_MSG_GetRunningSmuFeaturesHigh, 0,
                             &running_high);
    bug("[amdgpu-smu] GetRunningSmuFeaturesHigh -> %s (resp=0x%08lx mask=0x%08lx)\n",
        smu_resp_str(st), (unsigned long)st, (unsigned long)running_high);
    if (running_low || running_high)
        bug("[amdgpu-smu] PMFW already reports running features (high:low=%08lx:%08lx); "
            "a rejected host PPT upload does not imply disabled SMU power management\n",
            (unsigned long)running_high, (unsigned long)running_low);

    raw = (IPTR)AllocMem((IPTR)SMU_COMBO_PPTABLE_SIZE + SMU_SYS_TABLE_ALIGN,
                         MEMF_ANY);
    if (raw) {
        UBYTE *tbl = (UBYTE *)((raw + (SMU_SYS_TABLE_ALIGN - 1)) &
                               ~(IPTR)(SMU_SYS_TABLE_ALIGN - 1));
        ULONG len = SMU_COMBO_PPTABLE_SIZE;
        ULONG post_len;
        ULONG changed = 0;
        ULONG hash = 2166136261UL;
        ULONG i;
        APTR dma;
        UQUAD tbl_mc;

        for (i = 0; i < SMU_COMBO_PPTABLE_SIZE; i++)
            tbl[i] = 0xA5;

        dma = CachePreDMA((APTR)tbl, &len, 0); /* device -> RAM */
        tbl_mc = (UQUAD)(IPTR)dma;
        bug("[amdgpu-smu] reverse combo-PPT probe: tbl=%p dma=0x%08lx%08lx len=%lu%s\n",
            (APTR)tbl, (unsigned long)(tbl_mc >> 32),
            (unsigned long)(tbl_mc & 0xFFFFFFFFUL), (unsigned long)len,
            (len < SMU_COMBO_PPTABLE_SIZE) ? " !!SPLIT-DMA" : "");

        if (dma && len >= SMU_COMBO_PPTABLE_SIZE) {
            st = amdgpu_smu_send_msg(ad, PPSMC_MSG_SetDriverDramAddrHigh,
                                     (ULONG)(tbl_mc >> 32), &val);
            bug("[amdgpu-smu] reverse probe SetDriverDramAddrHigh -> %s (resp=0x%08lx)\n",
                smu_resp_str(st), (unsigned long)st);
            st = amdgpu_smu_send_msg(ad, PPSMC_MSG_SetDriverDramAddrLow,
                                     (ULONG)(tbl_mc & 0xFFFFFFFFUL), &val);
            bug("[amdgpu-smu] reverse probe SetDriverDramAddrLow -> %s (resp=0x%08lx)\n",
                smu_resp_str(st), (unsigned long)st);
            st = amdgpu_smu_send_msg(ad, PPSMC_MSG_TransferTableSmu2Dram,
                                     SMU_TABLE_COMBO_PPTABLE, &val);
        } else {
            st = 0;
        }

        post_len = len;
        CachePostDMA((APTR)tbl, &post_len, 0);
        for (i = 0; i < SMU_COMBO_PPTABLE_SIZE; i++) {
            if (tbl[i] != 0xA5)
                changed++;
            hash ^= tbl[i];
            hash *= 16777619UL;
        }
        bug("[amdgpu-smu] %s TransferTableSmu2Dram(COMBO_PPTABLE=1) -> %s "
            "(resp=0x%08lx changed=%lu/%lu hash=0x%08lx first=%08lx %08lx %08lx %08lx)\n",
            (st == PPSMC_Result_OK && changed) ? "***" : "   ", smu_resp_str(st),
            (unsigned long)st, (unsigned long)changed,
            (unsigned long)SMU_COMBO_PPTABLE_SIZE, (unsigned long)hash,
            (unsigned long)amdgpu_smu_le32(tbl + 0),
            (unsigned long)amdgpu_smu_le32(tbl + 4),
            (unsigned long)amdgpu_smu_le32(tbl + 8),
            (unsigned long)amdgpu_smu_le32(tbl + 12));

        FreeMem((APTR)raw, (IPTR)SMU_COMBO_PPTABLE_SIZE + SMU_SYS_TABLE_ALIGN);
    } else {
        bug("[amdgpu-smu] reverse combo-PPT probe AllocMem failed\n");
    }

    /* Linux allocates the SMU driver table in VRAM. Repeat the same reverse
     * transfer at the reserved driver-table VRAM slot and verify it through the
     * proven MM_INDEX aperture. This distinguishes an unsupported host-physical
     * address from a command that acknowledges without moving any data. */
    {
        const UQUAD tbl_mc = SMU_VRAM_MC_BASE + SMU_DRV_TABLE_VRAM_OFF;
        ULONG pattern[64];
        ULONG readback[64];
        ULONG off, i, j, attempt;

        for (i = 0; i < 64; i++)
            pattern[i] = 0xA5A5A5A5UL;

        /* A prior boot returned the expected combo wrapper while the next boot
         * returned a similarly sized non-wrapper payload. Retry only this
         * read-only transfer, with a fresh sentinel each time, so the log can
         * distinguish transient PMFW state from stable table-ID semantics. */
        for (attempt = 1; attempt <= 3 && !vram_pptable_staged; attempt++) {
            ULONG first[4] = { 0, 0, 0, 0 };
            ULONG changed = 0;
            ULONG hash = 2166136261UL;

            for (off = 0; off < SMU_COMBO_PPTABLE_SIZE; off += sizeof(pattern))
                amdgpu_vram_write(ad, SMU_DRV_TABLE_VRAM_OFF + off, pattern,
                                  sizeof(pattern) / sizeof(pattern[0]));

            st = amdgpu_smu_send_msg(ad, PPSMC_MSG_SetDriverDramAddrHigh,
                                     (ULONG)(tbl_mc >> 32), &val);
            bug("[amdgpu-smu] VRAM reverse attempt %lu SetDriverDramAddrHigh -> %s "
                "(resp=0x%08lx)\n", (unsigned long)attempt,
                smu_resp_str(st), (unsigned long)st);
            st = amdgpu_smu_send_msg(ad, PPSMC_MSG_SetDriverDramAddrLow,
                                     (ULONG)(tbl_mc & 0xFFFFFFFFUL), &val);
            bug("[amdgpu-smu] VRAM reverse attempt %lu SetDriverDramAddrLow -> %s "
                "(resp=0x%08lx)\n", (unsigned long)attempt,
                smu_resp_str(st), (unsigned long)st);
            st = amdgpu_smu_send_msg(ad, PPSMC_MSG_TransferTableSmu2Dram,
                                     SMU_TABLE_COMBO_PPTABLE, &val);

            for (off = 0; off < SMU_COMBO_PPTABLE_SIZE; off += sizeof(readback)) {
                amdgpu_vram_read(ad, SMU_DRV_TABLE_VRAM_OFF + off, readback,
                                 sizeof(readback) / sizeof(readback[0]));
                for (i = 0; i < sizeof(readback) / sizeof(readback[0]); i++) {
                    ULONG v = readback[i];
                    ULONG index = (off / sizeof(ULONG)) + i;
                    if (index < 4)
                        first[index] = v;
                    for (j = 0; j < sizeof(ULONG); j++) {
                        UBYTE b = (UBYTE)(v >> (j * 8));
                        if (b != 0xA5)
                            changed++;
                        hash ^= b;
                        hash *= 16777619UL;
                    }
                }
            }
            bug("[amdgpu-smu] %s VRAM attempt %lu "
                "TransferTableSmu2Dram(COMBO_PPTABLE=1) -> %s "
                "(resp=0x%08lx mc=%08lx:%08lx changed=%lu/%lu hash=0x%08lx "
                "first=%08lx %08lx %08lx %08lx)\n",
                (st == PPSMC_Result_OK && changed) ? "***" : "   ",
                (unsigned long)attempt, smu_resp_str(st), (unsigned long)st,
                (unsigned long)(tbl_mc >> 32),
                (unsigned long)(tbl_mc & 0xFFFFFFFFUL), (unsigned long)changed,
                (unsigned long)SMU_COMBO_PPTABLE_SIZE, (unsigned long)hash,
                (unsigned long)first[0], (unsigned long)first[1],
                (unsigned long)first[2], (unsigned long)first[3]);

            if (st == PPSMC_Result_OK && changed) {
                ULONG wrapper_size = first[0] & 0xFFFFUL;
                ULONG pmfw_off = (first[1] >> 16) & 0xFFFFUL;
                ULONG pmfw_size = first[2] & 0xFFFFUL;

                if (wrapper_size <= SMU_COMBO_PPTABLE_SIZE && pmfw_off && pmfw_size &&
                    pmfw_off + pmfw_size <= wrapper_size &&
                    pmfw_size <= SMU_DRV_TABLE_SIZE) {
                    ULONG src_off = pmfw_off;
                    ULONG dst_off = 0;
                    ULONG remain = (pmfw_size + sizeof(ULONG) - 1) / sizeof(ULONG);

                    /* Destination precedes source, so a forward chunked copy is
                     * overlap-safe. Keep the data in VRAM for PMFW's table DMA. */
                    while (remain) {
                        ULONG count = (remain > 64) ? 64 : remain;
                        amdgpu_vram_read(ad, SMU_DRV_TABLE_VRAM_OFF + src_off,
                                         readback, count);
                        amdgpu_vram_write(ad, SMU_DRV_TABLE_VRAM_OFF + dst_off,
                                          readback, count);
                        src_off += count * sizeof(ULONG);
                        dst_off += count * sizeof(ULONG);
                        remain -= count;
                    }
                    amdgpu_vram_read(ad, SMU_DRV_TABLE_VRAM_OFF, first, 4);
                    vram_pptable_staged = TRUE;
                    bug("[amdgpu-smu] *** authoritative PMFW PPTable staged in VRAM: "
                        "attempt=%lu wrapper=%lu off=0x%lx size=%lu mc=%08lx:%08lx "
                        "first=%08lx %08lx %08lx %08lx ***\n",
                        (unsigned long)attempt, (unsigned long)wrapper_size,
                        (unsigned long)pmfw_off, (unsigned long)pmfw_size,
                        (unsigned long)(tbl_mc >> 32),
                        (unsigned long)(tbl_mc & 0xFFFFFFFFUL),
                        (unsigned long)first[0], (unsigned long)first[1],
                        (unsigned long)first[2], (unsigned long)first[3]);
                } else {
                    bug("[amdgpu-smu] PMFW combo wrapper invalid on attempt %lu: "
                        "size=%lu off=0x%lx ppt=%lu\n",
                        (unsigned long)attempt, (unsigned long)wrapper_size,
                        (unsigned long)pmfw_off, (unsigned long)pmfw_size);
                }
            }

            if (!vram_pptable_staged && attempt < 3)
                smu_spin(200000);
        }
        if (!vram_pptable_staged)
            bug("[amdgpu-smu] PMFW combo wrapper unavailable after 3 bounded attempts; "
                "falling back to host firmware table\n");
    }
    return vram_pptable_staged;
}

/* EnableAllSmuFeatures takes a FEATURE_PWR_DOMAIN_e parameter — the name is a
 * misnomer, the arg selects WHICH power domain to bring up. The enum is stable
 * across smu11/smu13 driver-if headers (FEATURE_PWR_DOMAIN_e, e.g.
 * smu13_driver_if_v13_0_0.h:132) and unchanged for 14.0.2. We have always sent
 * 0 = FEATURE_PWR_ALL, which asks the PMFW to ramp EVERY domain and therefore
 * needs the full DPM context (parsed SkuTable, PCIe params, display count) that
 * we never built. smu_v14_0_2_enable_gfx_features() (smu_v14_0_2_ppt.c) sends
 * this same message with FEATURE_PWR_GFX instead, and is wired as
 * asic_reset_enable_gfx_features — AMD's sanctioned way to power ONLY the GFX
 * block after a reset, which is exactly our situation. */
#define FEATURE_PWR_ALL   0
#define FEATURE_PWR_S5    1
#define FEATURE_PWR_BACO  2
#define FEATURE_PWR_SOC   3
#define FEATURE_PWR_GFX   4

static void amdgpu_smu_enable_features(struct AmdgpuDevice *ad,
                                       BOOL vram_pptable_staged,
                                       BOOL scpm_enabled,
                                       ULONG pwr_domain)
{
    UQUAD tbl_mc = SMU_VRAM_MC_BASE + SMU_DRV_TABLE_VRAM_OFF;
    ULONG st, val = 0;

    bug("[amdgpu-smu] SMU 14.0.2 uses the PMFW/IFWI PPTable; host upload omitted "
        "(Linux 136ce12bd590)%s\n",
        vram_pptable_staged ? "; reverse table read verified" : "");

    st = amdgpu_smu_send_msg(ad, PPSMC_MSG_SetDriverDramAddrHigh,
                             (ULONG)(tbl_mc >> 32), &val);
    bug("[amdgpu-smu] SetDriverDramAddrHigh(0x%08lx) -> %s (resp=0x%08lx)\n",
        (unsigned long)(ULONG)(tbl_mc >> 32), smu_resp_str(st), (unsigned long)st);
    st = amdgpu_smu_send_msg(ad, PPSMC_MSG_SetDriverDramAddrLow,
                             (ULONG)(tbl_mc & 0xFFFFFFFFUL), &val);
    bug("[amdgpu-smu] SetDriverDramAddrLow(0x%08lx) -> %s (resp=0x%08lx)\n",
        (unsigned long)(ULONG)(tbl_mc & 0xFFFFFFFFUL), smu_resp_str(st),
        (unsigned long)st);

    /* Register the tools (PMSTATUSLOG) table address too. Linux does this before
     * setting the allowed feature mask. */
    {
        UQUAD tool_mc = SMU_VRAM_MC_BASE + SMU_TOOL_TABLE_VRAM_OFF;
        st = amdgpu_smu_send_msg(ad, PPSMC_MSG_SetToolsDramAddrHigh,
                                 (ULONG)(tool_mc >> 32), &val);
        bug("[amdgpu-smu] SetToolsDramAddrHigh(0x%08lx) -> %s (resp=0x%08lx)\n",
            (unsigned long)(ULONG)(tool_mc >> 32), smu_resp_str(st), (unsigned long)st);
        st = amdgpu_smu_send_msg(ad, PPSMC_MSG_SetToolsDramAddrLow,
                                 (ULONG)(tool_mc & 0xFFFFFFFFUL), &val);
        bug("[amdgpu-smu] SetToolsDramAddrLow(0x%08lx) -> %s (resp=0x%08lx)\n",
            (unsigned long)(ULONG)(tool_mc & 0xFFFFFFFFUL), smu_resp_str(st),
            (unsigned long)st);
    }

    /* Linux issues the DC board-training command after table setup and before
     * setting the allowed feature mask. Keep the probe non-fatal so one bounded
     * DUT run captures both this response and the downstream mask response. */
    st = amdgpu_smu_send_msg(ad, PPSMC_MSG_RunDcBtc, 0, &val);
    bug("[amdgpu-smu] RunDcBtc -> %s (resp=0x%08lx)\n",
        smu_resp_str(st), (unsigned long)st);

    /* UseDefaultPPTable (0x14) is deliberately NOT sent here.
     *
     * I originally landed it as "the message that breaks the GFX power wall".
     * That was wrong, and the control disproved it (evidence
     * control-0x14-not-required-20260818T021918Z): on a fresh boot with GFX
     * verified dark, SetAllowedFeaturesMask + EnableAllSmuFeatures ALONE — no
     * 0x14 anywhere — powers the rail to exactly the same end state
     * (running 0 -> 0498f59e:38fffcfb, RESET_CTRL 0x30 -> 0x7F,
     * BOOTLOAD_STATUS -> 0x8000003f, GRBM_STATUS -> 0x382c).
     *
     * How the wrong conclusion happened, since it is worth not repeating: 0x14
     * alone left features at 0, then 0x14 followed by the mask/enable pair
     * worked, and I credited 0x14. The variable between those runs was the
     * enable, not 0x14. Its ~13300-poll duration made it LOOK like the
     * heavyweight actor, and that impression stood in for a control it took
     * eight minutes to actually run.
     *
     * So it is an unproven state change to PMFW with no demonstrated benefit,
     * which does not belong in a boot path. It remains available as a probe
     * lever (C:GfxResetProbe smumsg MSG 20) if a card ever turns up whose PMFW
     * genuinely lacks a usable table — note this one demonstrably HAS one:
     * TransferTableSmu2Dram(COMBO_PPTABLE) returns real content, and its first
     * dwords are the feature mask, before 0x14 is ever sent. */

    /* SetAllowedFeaturesMask: Linux SKIPS this under SCPM (PSP owns the signed
     * mask, smu_v14_0_set_allowed_mask) and SCPM *is* enabled on this card — but
     * we send it ANYWAY, deliberately, because the sequence measured working on
     * the DUT sent it. GfxResetProbe's `smumask` lever fires the mask pair
     * unconditionally and that is the run that powered the rail; skipping it here
     * would mean shipping a sequence nobody has ever seen work. It is rejected
     * harmlessly (0xFD) when PSP owns the mask, so the cost of sending is a log
     * line and the cost of not sending is an untested boot path.
     *
     * CAUTION on reading the responses below: on the run that actually powered
     * the rail, SetAllowedFeaturesMask returned 0xFD (CmdRejectedPrereq) and
     * EnableAllSmuFeatures returned 0xFE (UnknownCmd) — and the features came on
     * anyway. The PPSMC response codes on this PMFW are not reliable attribution
     * for these two; trust the RunningFeatures readback, not the return code. */
    if (scpm_enabled)
        bug("[amdgpu-smu] SCPM enabled (PSP owns the signed mask); sending "
            "SetAllowedFeaturesMask regardless — this is what was measured working\n");

    st = amdgpu_smu_send_msg(ad, PPSMC_MSG_SetAllowedFeaturesMaskHigh,
                             0xFFFFFFFFUL, &val);
    bug("[amdgpu-smu] SetAllowedFeaturesMaskHigh(0xFFFFFFFF) -> %s (resp=0x%08lx)\n",
        smu_resp_str(st), (unsigned long)st);

    st = amdgpu_smu_send_msg(ad, PPSMC_MSG_SetAllowedFeaturesMaskLow,
                             0xFFFFFFFFUL, &val);
    bug("[amdgpu-smu] SetAllowedFeaturesMaskLow(0xFFFFFFFF) -> %s (resp=0x%08lx)\n",
        smu_resp_str(st), (unsigned long)st);

    /* EnableAllSmuFeatures — UNCONDITIONAL (Linux smu_smc_hw_setup:1667). This is
     * the message that actually turns features ON. Previously omitted, which is why
     * RunningSmuFeatures read 0 and the discrete GFX rail stayed unpowered.
     * pwr_domain selects the domain: 0 = FEATURE_PWR_ALL (what smu_smc_hw_setup
     * sends, needs the full DPM context), 4 = FEATURE_PWR_GFX (what
     * smu_v14_0_2_enable_gfx_features sends to power just the GFX block). */
    st = amdgpu_smu_send_msg(ad, PPSMC_MSG_EnableAllSmuFeatures, pwr_domain, &val);
    bug("[amdgpu-smu] *** EnableAllSmuFeatures(pwr_domain=%lu %s) -> %s (resp=0x%08lx) ***\n",
        (unsigned long)pwr_domain,
        (pwr_domain == FEATURE_PWR_ALL) ? "ALL"  :
        (pwr_domain == FEATURE_PWR_S5)  ? "S5"   :
        (pwr_domain == FEATURE_PWR_BACO)? "BACO" :
        (pwr_domain == FEATURE_PWR_SOC) ? "SOC"  :
        (pwr_domain == FEATURE_PWR_GFX) ? "GFX"  : "?",
        smu_resp_str(st), (unsigned long)st);

    {
        ULONG running_low = 0, running_high = 0;
        ULONG low_st, high_st;
        BOOL gfxclk, gfximu;

        low_st = amdgpu_smu_send_msg(ad, PPSMC_MSG_GetRunningSmuFeaturesLow, 0,
                                     &running_low);
        high_st = amdgpu_smu_send_msg(ad, PPSMC_MSG_GetRunningSmuFeaturesHigh, 0,
                                      &running_high);
        /* DPM_GFXCLK bit1 lives in the low word; GFX_IMU bit36 in the high word
         * (bit 36-32=4). Both ON == GFX rail powered -> autoload can complete. */
        gfxclk = (running_low  & (1UL << SMU14_FEATURE_DPM_GFXCLK_BIT)) != 0;
        gfximu = (running_high & (1UL << (SMU14_FEATURE_GFX_IMU_BIT - 32))) != 0;
        bug("[amdgpu-smu] post-enable running features -> low:%s=0x%08lx "
            "high:%s=0x%08lx combined=%08lx:%08lx\n",
            smu_resp_str(low_st), (unsigned long)running_low,
            smu_resp_str(high_st), (unsigned long)running_high,
            (unsigned long)running_high, (unsigned long)running_low);
        bug("[amdgpu-smu] *** GFX-power features: DPM_GFXCLK(b1)=%s GFX_IMU(b36)=%s -> "
            "GFX rail %s ***\n",
            gfxclk ? "ON" : "off", gfximu ? "ON" : "off",
            (gfxclk && gfximu) ? "POWERED (autoload should complete)" :
            (running_low || running_high) ? "PARTIAL (some features on)" :
            "STILL UNPOWERED (features=0 -> PSP SCPM pptable handoff / 0xa5 poison next)");

        /* DPM policy: AUTO, as Linux leaves a booted card
         * (smu_v14_0_set_performance_level, AMD_DPM_FORCED_LEVEL_AUTO ->
         * smu_v14_0_set_soft_freq_limited_range with automatic=true). For every
         * clock whose DPM feature is running, the soft range is opened to the
         * whole DPM table: SoftMax=(PPCLK<<16)|0xFFFF, SoftMin=(PPCLK<<16)|0.
         * PMFW then picks the level from load and idles the GPU when nothing
         * runs. No hard minimum is set.
         *
         * Until 2026-09-24 this block pinned GFXCLK's soft and hard minimum to
         * the DPM maximum on every boot. That was a 2026-08-23 discriminator
         * for "is GFX clock-parked?" while the ME did not execute; ME execution
         * was later established through the MES mapping work, and the pin was
         * never released, so the GPU ran at its top clock at idle.
         * AMDGPU=gfxclkmax on the kernel command line restores the pin for an
         * A/B on hardware. */
        amdgpu_smu_apply_dpm_policy(ad, running_low);
        {
            struct AmdgpuSmuMetrics mt;
            if (amdgpu_smu_read_metrics(ad, &mt))
                bug("[amdgpu-smu] metrics #%lu: GFXCLK cur %lu avg %u MHz, UCLK %lu, "
                    "SOCCLK %lu, FCLK %lu | GFX busy %u%% | %u W board | "
                    "edge %u C hotspot %u C mem %u C | fan %u%% %u rpm\n",
                    (unsigned long)mt.counter, (unsigned long)mt.gfxclk_cur,
                    mt.gfxclk_avg, (unsigned long)mt.uclk_cur,
                    (unsigned long)mt.socclk_cur, (unsigned long)mt.fclk_cur,
                    mt.gfx_activity, mt.board_power, mt.temp_edge,
                    mt.temp_hotspot, mt.temp_mem, mt.fan_pwm, mt.fan_rpm);
            else
                bug("[amdgpu-smu] metrics: TransferTableSmu2Dram(5) did not deliver\n");
        }
    }
}

/*
 * Stage 3 P0 — SMU sign of life. Log the MP1 base + raw mailbox registers (so a
 * boot shows whether the base/offset is live, not an all-zero/all-ones hole),
 * then GetSmuVersion. Query only: no clock or power state is changed. Wired into
 * the IP-block ladder as the 'smu' hw_init (was NULL / "not implemented").
 */
BOOL amdgpu_smu_hw_init(struct AmdgpuDevice *ad)
{
    ULONG base, r66, r82, r90, ver = 0;
    BOOL ok;

    if (!ad || !ad->rmmio || !ad->ip.smu.valid || !ad->ip.smu.base) {
        bug("[amdgpu-smu] no SMU/MP1 block (valid=%d base=0x%x) — clock route blocked\n",
            ad ? (int)ad->ip.smu.valid : -1,
            ad ? (unsigned)(ad->ip.smu.base) : 0);
        return TRUE;   /* not fatal to boot */
    }
    if (!smu_mbox_lock_ready) {
        InitSemaphore(&smu_mbox_lock);
        smu_mbox_lock_ready = TRUE;
    }
    base = amdgpu_smu_base(ad);
    if (base != ad->ip.smu.base)
        bug("[amdgpu-smu] MP1 base adjusted 0x%05x -> 0x%05x (discovery base0=MP0; MP1=+0x200)\n",
            (unsigned)ad->ip.smu.base, (unsigned)base);

    r66 = amdgpu_rreg32(ad, base + MP1_C2PMSG_66);
    r82 = amdgpu_rreg32(ad, base + MP1_C2PMSG_82);
    r90 = amdgpu_rreg32(ad, base + MP1_C2PMSG_90);
    bug("[amdgpu-smu] MP1 v%u.%u.%u base=0x%05x  C2PMSG_66=0x%08lx 82=0x%08lx 90=0x%08lx\n",
        (unsigned)ad->ip.smu.major, (unsigned)ad->ip.smu.minor,
        (unsigned)ad->ip.smu.revision, (unsigned)base,
        (unsigned long)r66, (unsigned long)r82, (unsigned long)r90);

    if ((r66 == 0xFFFFFFFFUL) && (r90 == 0xFFFFFFFFUL)) {
        bug("[amdgpu-smu] mailbox reads all-ones — base/offset wrong or block absent; not sending\n");
        return TRUE;
    }

    /* Diagnostic: dump the live C2PMSG bank so we can locate the message/param
     * registers empirically (the response reg is the one holding a POST value).
     * C2PMSG_N dword = 0x40 + N; scan N=0..127, log only the non-zero ones. */
    {
        ULONG n;
        bug("[amdgpu-smu] C2PMSG scan @0x%05x (non-zero):\n", (unsigned)base);
        for (n = 0; n < 128; n++) {
            ULONG v = amdgpu_rreg32(ad, base + 0x40 + n);
            if (v != 0 && v != 0xFFFFFFFFUL)
                bug("[amdgpu-smu]   C2PMSG_%lu = 0x%08lx\n",
                    (unsigned long)n, (unsigned long)v);
        }
    }

    ok = (amdgpu_smu_send_msg(ad, PPSMC_MSG_GetSmuVersion, 0, &ver) == SMU_RESP_OK);
    if (ok) {
        ULONG scpm_status = PSP_SCPM_DISABLED;
        BOOL scpm_found = amdgpu_smu_get_scpm_status(ad, &scpm_status);
        BOOL scpm_enabled = scpm_found && scpm_status != PSP_SCPM_DISABLED;

        bug("[amdgpu-smu] *** SMU SIGN OF LIFE: GetSmuVersion OK -> 0x%08lx "
            "(%lu.%lu.%lu) — mailbox live, clock messages unlocked ***\n",
            (unsigned long)ver, (unsigned long)((ver >> 16) & 0xFF),
            (unsigned long)((ver >> 8) & 0xFF), (unsigned long)(ver & 0xFF));
        /* Capture read-only PMFW state and reverse-DMA evidence before changing
         * any table or feature state. */
        BOOL vram_pptable_staged = amdgpu_smu_probe_protocol(ad);
        (void)vram_pptable_staged;
        /* ARMED 2026-08-18. This was PARKED with three reasons, and the DUT has
         * now falsified all three (evidence gfx-power-wall-broken-20260818T005800Z):
         *
         *  "EnableAllSmuFeatures(0x6) wedges the mailbox for the rest of boot" —
         *      it does not. With UseDefaultPPTable ahead of it, 0x6 returns (0xFE)
         *      and the mailbox keeps answering every subsequent message.
         *  "the feature rejects are DOWNSTREAM of GFX power anyway" — inverted.
         *      The features ARE the GFX power: enabling them took the rail from
         *      0 mV to 0xb5, GPM_STAT 0 -> 0x00b40016, RESET_CTRL 0x30 -> 0x7F and
         *      RLC_RLCS_BOOTLOAD_STATUS to BOOTLOAD_COMPLETE=1.
         *  "the real wall is a reset that ACTUALLY resets GC" — it is not. The
         *      whole reset-lever search (mode1/mode2/FLR/GRBM/D3-D0/strap) came
         *      back conclusively negative because a reset cannot substitute for
         *      power, and PMFW owns power on this part.
         *
         * The PMFW-newer-than-ROCm note (GetDriverIfVersion 0x33 vs 0x2E) stands
         * as a fact but was never a blocker — it is informational.
         *
         * FEATURE_PWR_ALL, not GFX: the measured-working run went through
         * GfxResetProbe's `smumask` lever, which sends EnableAllSmuFeatures with
         * arg 0. Passing FEATURE_PWR_GFX here would be a different experiment from
         * the one that produced the evidence. */
        (void)scpm_enabled;
        amdgpu_smu_full_setup(ad, FEATURE_PWR_ALL);
    } else
        bug("[amdgpu-smu] GetSmuVersion: no OK response (got resp path, ver=0x%08lx) — "
            "verify smu_v14 C2PMSG_66/82/90 offsets + MP1 base\n",
            (unsigned long)ver);
    return TRUE;
}

/*
 * Full Linux-order SMU bring-up, driven from a probe instead of from init.
 *
 * (History: amdgpu_smu_enable_features() was PARKED in the boot path until it
 * was armed on 2026-08-18; while parked, a booted card had NO driver/tools
 * DRAM address registered with the PMFW at all.)
 * Linux treats smu_set_driver_table_location() as a hard prerequisite —
 * smu_smc_hw_setup() aborts on its failure long before it reaches
 * EnableAllSmuFeatures — so a bare enable sent from a probe is not the same
 * experiment Linux runs. This wrapper replays the whole prefix in Linux order
 * (driver table -> tools table -> RunDcBtc -> allowed mask unless SCPM ->
 * EnableAllSmuFeatures -> read back running features), resolving SCPM from the
 * PSP runtime DB exactly as the boot path would. Exported for C:GfxResetProbe so
 * the sequence can be tested on metal without arming it in init.
 */
void amdgpu_smu_full_setup(struct AmdgpuDevice *ad, ULONG pwr_domain)
{
    ULONG scpm_status = PSP_SCPM_DISABLED;
    BOOL scpm_found   = amdgpu_smu_get_scpm_status(ad, &scpm_status);
    BOOL scpm_enabled = scpm_found && (scpm_status != PSP_SCPM_DISABLED);
    BOOL staged;

    bug("[amdgpu-smu] full setup: SCPM %s (status=%lu, found=%d)\n",
        scpm_enabled ? "ENABLED" : "disabled",
        (unsigned long)scpm_status, (int)scpm_found);

    staged = amdgpu_smu_probe_protocol(ad);
    amdgpu_smu_enable_features(ad, staged, scpm_enabled, pwr_domain);
}

/*
 * Read-only diagnostics for the standalone AmdgpuWake harness (fast network
 * iteration, no reboot). Does NOT enable features, so it won't wedge a live SMU:
 * SMU version, running-feature mask, and a RAW hex dump of the PSP runtime DB
 * around the SCPM entry so the parse can be fixed against the actual bytes. The
 * VRAM read goes through the MM_INDEX aperture, independent of the SMU mailbox,
 * so the DB dump is valid even if the mailbox is wedged from a prior attempt.
 */
void amdgpu_smu_dump(struct AmdgpuDevice *ad)
{
    ULONG ver = 0, flo = 0, fhi = 0, st;
    UQUAD vram_size, db_pos;
    ULONG raw[16];
    ULONG i;

    if (!ad || !ad->rmmio || !ad->ip.smu.valid || !ad->ip.smu.base) {
        bug("[amdgpu-wake] no SMU/MP1 block (valid=%d base=0x%x)\n",
            ad ? (int)ad->ip.smu.valid : -1, ad ? (unsigned)ad->ip.smu.base : 0);
        return;
    }

    st = amdgpu_smu_send_msg(ad, PPSMC_MSG_GetSmuVersion, 0, &ver);
    bug("[amdgpu-wake] GetSmuVersion -> %s ver=0x%08lx (%lu.%lu.%lu)%s\n",
        smu_resp_str(st), (unsigned long)ver,
        (unsigned long)((ver >> 16) & 0xFF), (unsigned long)((ver >> 8) & 0xFF),
        (unsigned long)(ver & 0xFF),
        (st != SMU_RESP_OK) ? "  <-- mailbox WEDGED/quiet" : "");
    amdgpu_smu_send_msg(ad, PPSMC_MSG_GetRunningSmuFeaturesLow, 0, &flo);
    amdgpu_smu_send_msg(ad, PPSMC_MSG_GetRunningSmuFeaturesHigh, 0, &fhi);
    bug("[amdgpu-wake] RunningFeatures = %08lx:%08lx (DPM_GFXCLK b1=%s GFX_IMU b36=%s)\n",
        (unsigned long)fhi, (unsigned long)flo,
        (flo & (1UL << SMU14_FEATURE_DPM_GFXCLK_BIT)) ? "ON" : "off",
        (fhi & (1UL << (SMU14_FEATURE_GFX_IMU_BIT - 32))) ? "ON" : "off");

    if (!ad->vram_mb) {
        bug("[amdgpu-wake] vram_mb=0 — cannot locate PSP runtime DB\n");
        return;
    }
    vram_size = (UQUAD)ad->vram_mb << 20;
    db_pos = vram_size - PSP_RUNTIME_DB_OFFSET;
    bug("[amdgpu-wake] PSP runtime DB @VRAM 0x%08lx%08lx — raw dwords:\n",
        (unsigned long)(db_pos >> 32), (unsigned long)(db_pos & 0xFFFFFFFFUL));
    amdgpu_vram_read(ad, db_pos, raw, 16);
    for (i = 0; i < 16; i += 4)
        bug("[amdgpu-wake]  +0x%03lx: %08lx %08lx %08lx %08lx\n",
            (unsigned long)(i * 4), (unsigned long)raw[i], (unsigned long)raw[i+1],
            (unsigned long)raw[i+2], (unsigned long)raw[i+3]);
    bug("[amdgpu-wake] PSP runtime DB +0x200 (SCPM entry region):\n");
    amdgpu_vram_read(ad, db_pos + 0x200, raw, 16);
    for (i = 0; i < 16; i += 4)
        bug("[amdgpu-wake]  +0x%03lx: %08lx %08lx %08lx %08lx\n",
            (unsigned long)(0x200 + i * 4), (unsigned long)raw[i], (unsigned long)raw[i+1],
            (unsigned long)raw[i+2], (unsigned long)raw[i+3]);
}

LONG amdgpu_query_sensors(struct AmdgpuDevice *ad, struct AmdgpuSensorInfo *out,
                          ULONG out_size)
{
    struct AmdgpuSmuMetrics m;

    if (!out || out_size < sizeof(*out))
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!ad || !ad->rmmio || !ad->ip.smu.valid)
        return -ENODEV;
    if (!amdgpu_smu_read_metrics(ad, &m))
        return -EIO;
    out->abi_version    = AMDGPU_SENSOR_INFO_ABI_VERSION;
    out->sample_counter = m.counter;
    out->gfxclk_cur = m.gfxclk_cur;   out->socclk_cur = m.socclk_cur;
    out->uclk_cur   = m.uclk_cur;     out->fclk_cur   = m.fclk_cur;
    out->gfxclk_avg = m.gfxclk_avg;   out->uclk_avg   = m.uclk_avg;
    out->fclk_avg   = m.fclk_avg;
    out->gfx_activity = m.gfx_activity; out->mem_activity = m.uclk_activity;
    out->socket_power = m.socket_power; out->board_power  = m.board_power;
    out->temp_edge = m.temp_edge; out->temp_hotspot = m.temp_hotspot;
    out->temp_mem  = m.temp_mem;
    out->fan_pwm = m.fan_pwm; out->fan_rpm = m.fan_rpm;
    return 0;
}
