/*
 * amdgpu.hidd — PSP (Platform Security Processor / MP0) bring-up.
 *
 * On RDNA2+ the PSP is the gateway to the whole GPU: it boots from its signed
 * "SOS" (Secure OS) firmware, then authenticates and loads every other engine's
 * microcode (GC / SDMA / SMU / DCN). Nothing on the card runs its own firmware
 * until PSP is up — so PSP is the first IP block in the ladder that needs a
 * firmware blob, and the first real chance to make the silicon execute our code.
 *
 * This tier is deliberately narrow and honest: fetch the SOS firmware from
 * DEVS:Firmware after AmdgpuMonitor opens the driver during Startup-Sequence,
 * validate its container header against the hardware we discovered, and stage
 * it. The register-level bootloader handshake
 * at the discovered MP0 base (ad->ip.psp.base) is the next step and mirrors the
 * kernel's psp_v14_0: wait for bootloader-ready, hand it the SOS, wait for
 * sOS-ready, then load TA and set up the PSP ring. We do NOT poke MP0 registers
 * yet — the ladder stays observe-only until that handshake is implemented against
 * the correct psp_v14 C2PMSG offsets.
 */

#define DEBUG 1
#include <aros/debug.h>

#include <proto/exec.h>

#include "amdgpu_intern.h"
#include "amdgpu_vram.h"
#include "amdgpu_query_internal.h"

/* AMD common firmware container header (amdgpu_ucode.h: common_firmware_header).
 * Every AMD ucode blob starts with this; all fields are little-endian and read
 * byte-wise so host endianness is irrelevant. */
static inline UWORD le16(const UBYTE *b) { return b[0] | (b[1] << 8); }
static inline ULONG le32(const UBYTE *b)
{
    return (ULONG)b[0] | ((ULONG)b[1] << 8) |
           ((ULONG)b[2] << 16) | ((ULONG)b[3] << 24);
}

/* MP0 (PSP) C2PMSG mailbox registers — DWORD offsets added to the discovered MP0
 * base (ad->ip.psp.base, 0x16000 on Navi 44). From Linux mp_14_0_2_offset.h; all
 * BASE_IDX 0, so they share the discovered base's segment. psp_v14_0 uses:
 *   C2PMSG_35 = bootloader status (bit31 = ready) AND the load command register
 *   C2PMSG_36 = firmware physical address >> 20
 *   C2PMSG_81 = sign-of-life (non-zero => sys driver + sOS already loaded) */
#define MP0_C2PMSG_35   0x0063
#define MP0_C2PMSG_36   0x0064
#define MP0_C2PMSG_81   0x0091
#define PSP_BL_READY    0x80000000UL   /* C2PMSG_35 bit 31 */

/* KM (kernel-mode / GPCOM) ring-create registers (psp_v14_0_ring_create, bare-metal
 * path). The driver's command channel to the running secure OS. */
#define MP0_C2PMSG_64   0x0080   /* ring init command + bit31 response */
#define MP0_C2PMSG_67   0x0083   /* GPCOM ring write-pointer (set, non-SRIOV) */
#define MP0_C2PMSG_69   0x0085   /* ring buffer MC addr, low 32  */
#define MP0_C2PMSG_70   0x0086   /* ring buffer MC addr, high 32 */
#define MP0_C2PMSG_71   0x0087   /* ring size */
#define MP0_C2PMSG_102  0x00A6   /* GPCOM ring write-pointer (get) */
#define PSP_RING_TYPE_KM 2
#define PSP_RING_SIZE    0x1000
#define PSP_RING_OFF     ((UQUAD)260 << 20)   /* KM ring VRAM offset (matches ring_create) */
#define PSP_MBOX_RESP    0x80000000UL
#define PSP_MBOX_STATUS  0x0000FFFFUL
#define PSP_MBOX_MASK    (PSP_MBOX_RESP | PSP_MBOX_STATUS)

/* GPCOM command submission (post GFX_CMD_ID_LOAD_IP_FW so the PSP powers+loads the
 * otherwise-write-blocked GFX block). VRAM staging offsets, clear of the ring
 * (260 MiB) and the SOS stage (256 MiB). psp_gfx_if.h layout. */
#define PSP_CMD_VRAM_OFF     ((UQUAD)264 << 20)   /* psp_gfx_cmd_resp (1024B, 4KB-aligned) */
#define PSP_FENCE_VRAM_OFF   ((UQUAD)266 << 20)   /* fence dword */
#define PSP_FW_I_VRAM_OFF    ((UQUAD)268 << 20)   /* staged IMU IRAM (4KB-aligned) */
#define PSP_FW_D_VRAM_OFF    ((UQUAD)270 << 20)   /* staged IMU DRAM (4KB-aligned) */
#define PSP_GFX_CMD_LOAD_IP_FW  6
#define PSP_GFX_CMD_BUF_VERSION 1
#define PSP_GFX_FW_TYPE_IMU_I   68
#define PSP_GFX_FW_TYPE_IMU_D   69

/* Linux gfx_v12_0_wait_for_rlc_autoload_complete(): PSP response status is
 * advisory during initialization, so the hardware state is authoritative. */
#define GC0_CP_STAT                         0x0f40
#define GC1_RLC_RLCS_BOOTLOAD_STATUS        0x4e7c
#define RLC_BOOTLOAD_COMPLETE               0x80000000UL

/* MMHUB FB-location registers — DWORD offsets from the discovered MMHUB base
 * (ad->ip.mmhub.base, 0x1a000 on Navi 44), from Linux mmhub_4_1_0_offset.h
 * (BASE_IDX 0). The VRAM MC start = (FB_LOCATION_BASE & 0xFFFFFF) << 24; the PSP
 * DMAs firmware by MC address, so a VRAM-staged SOS sits at vram_start + offset. */
#define MMHUB_FB_LOCATION_BASE  0x0554
#define MMHUB_FB_LOCATION_TOP   0x0555
#define MMHUB_FB_OFFSET         0x04c7

/* psp_fw_type enum values (amdgpu_ucode.h) — the bootloader-stage components. */
#define PSP_FW_TYPE_SOS       1
#define PSP_FW_TYPE_SYS_DRV   2
#define PSP_FW_TYPE_KDB       3
#define PSP_FW_TYPE_TOC       4
#define PSP_FW_TYPE_SPL       5
#define PSP_FW_TYPE_SOC_DRV   7
#define PSP_FW_TYPE_INTF_DRV  8
#define PSP_FW_TYPE_DBG_DRV   9
#define PSP_FW_TYPE_RAS_DRV   10
#define PSP_FW_TYPE_IPKEYMGR  11

/* psp_bootloader_cmd values (amdgpu_psp.h) — written to C2PMSG_35. */
#define PSP_BL_LOAD_SYSDRV   0x00010000UL
#define PSP_BL_LOAD_SOSDRV   0x00020000UL
#define PSP_BL_LOAD_KDB      0x00080000UL
#define PSP_BL_LOAD_SOCDRV   0x000B0000UL
#define PSP_BL_LOAD_DBGDRV   0x000C0000UL
#define PSP_BL_LOAD_INTFDRV  0x000D0000UL
#define PSP_BL_LOAD_RASDRV   0x000E0000UL
#define PSP_BL_LOAD_IPKEYMGR 0x000F0000UL
#define PSP_BL_LOAD_SPL      0x10000000UL

/* Linux uses one 1 MiB-aligned GTT fw_pri_buf for both bootloader components and
 * later GPCOM payloads such as LOAD_TOC. Keep the raw-VRAM slot only as a bounded
 * fallback when the MMHUB GART cannot be established. */
#define PSP_PRIVATE_STAGE_SIZE (1UL << 20)
#define PSP_STAGE_VRAM_OFF   ((UQUAD)256 << 20)

/* Crude busy-delay — no timer.device exists at residentpri 49. Volatile so the
 * loop is not optimised away; not calibrated, only used to pace the handshake. */
static void psp_spin(ULONG loops)
{
    volatile ULONG i = 0;
    while (i < loops)
        i++;
}

/* Poll C2PMSG_35 for the bootloader-ready sentinel (bit 31), bounded so a wedged
 * PSP never hangs the boot (recoverable by power-cycle, never an infinite loop). */
static BOOL psp_wait_ready(struct AmdgpuDevice *ad)
{
    ULONG base = ad->ip.psp.base;
    ULONG t;

    for (t = 0; t < 5000; t++) {
        if (amdgpu_rreg32(ad, base + MP0_C2PMSG_35) & PSP_BL_READY)
            return TRUE;
        psp_spin(2000);
    }
    return FALSE;
}

/* Locate a sub-binary of psp_fw_type `type` in the v2_0 SOS container. Data sits
 * at blob + desc.offset_bytes + header.ucode_array_offset_bytes (verified against
 * parse_sos_bin_descriptor). Descriptor table starts at header_size_bytes; count
 * is the u32 right after the 32-byte common header. All bounds-checked. */
static BOOL psp_find_subbin(const UBYTE *fw, ULONG fwsize, ULONG type,
                            const UBYTE **out_data, ULONG *out_size)
{
    ULONG hdr_size = le32(fw + 4);        /* offsetof(psp_fw_bin) = 36 for v2_0 */
    ULONG count    = le32(fw + 32);       /* psp_fw_bin_count, after common hdr  */
    ULONG uoff     = le32(fw + 24);       /* ucode_array_offset_bytes = 256      */
    ULONG i;

    for (i = 0; i < count; i++) {
        ULONG d = hdr_size + i * 16;
        if (d + 16 > fwsize)
            break;
        if (le32(fw + d) == type) {
            ULONG off = le32(fw + d + 8);
            ULONG sz  = le32(fw + d + 12);
            if ((UQUAD)uoff + off + sz > fwsize)
                return FALSE;             /* would read past the blob */
            *out_data = fw + uoff + off;
            *out_size = sz;
            return TRUE;
        }
    }
    return FALSE;
}

/* One psp_v14_0 bootloader load_component: zero and reuse the same 1 MiB private
 * GART window Linux uses for fw_pri_buf, hand its MC address (>>20) to the PSP,
 * and wait for ready again. */
static BOOL psp_load_component(struct AmdgpuDevice *ad, UQUAD vram_start,
                              const UBYTE *data, ULONG size, ULONG cmd,
                              const char *label)
{
    ULONG base    = ad->ip.psp.base;
    UQUAD mc_addr;
    ULONG dwords  = (size + 3) >> 2;
    const char *stage_name;
    BOOL use_gart = ad->gart_ready && ad->gart_arena &&
                    size <= PSP_PRIVATE_STAGE_SIZE;

    if (use_gart) {
        mc_addr = ad->gart_fw_va;
        stage_name = "GART-private";
    } else {
        mc_addr = vram_start + PSP_STAGE_VRAM_OFF;
        stage_name = "VRAM-fallback";
    }

    D(bug("[amdgpu-psp] load %s: %lu B -> %s, MC=0x%08lx%08lx "
          "(>>20=0x%08lx) cmd=0x%08lx\n",
          label, (unsigned long)size, stage_name,
          (unsigned long)(mc_addr >> 32), (unsigned long)(mc_addr & 0xFFFFFFFFUL),
          (unsigned long)(mc_addr >> 20), (unsigned long)cmd));

    if (!psp_wait_ready(ad)) {
        D(bug("[amdgpu-psp]   %s: bootloader not ready before load\n", label));
        return FALSE;
    }

    if (use_gart) {
        ULONG *stage = (ULONG *)ad->gart_arena;
        ULONG words = PSP_PRIVATE_STAGE_SIZE / sizeof(*stage);
        ULONG i;

        for (i = 0; i < words; i++)
            stage[i] = 0;
        CopyMem((APTR)data, ad->gart_arena, size);
    } else {
        amdgpu_vram_write(ad, PSP_STAGE_VRAM_OFF,
                          (const ULONG *)data, dwords);
    }

    amdgpu_wreg32(ad, base + MP0_C2PMSG_36, (ULONG)(mc_addr >> 20));
    amdgpu_wreg32(ad, base + MP0_C2PMSG_35, cmd);
    psp_spin(500000);   /* let the bootloader pick up and start the command */

    if (!psp_wait_ready(ad)) {
        D(bug("[amdgpu-psp]   %s: bootloader did not re-ready (rejected/faulted)\n", label));
        return FALSE;
    }
    {
        ULONG st = amdgpu_rreg32(ad, base + MP0_C2PMSG_35);
        D(bug("[amdgpu-psp]   %s done: C2PMSG_35=0x%08lx%s\n", label, (unsigned long)st,
              (st & ~PSP_BL_READY) ? " (ERROR bits set!)" : " (clean)"));
    }
    return TRUE;
}

/* Create the KM (kernel-mode / GPCOM) PSP ring — the driver's command channel to
 * the running secure OS. Stage a zeroed ring buffer in VRAM, hand the PSP its MC
 * address + size, issue the ring-init command, poll C2PMSG_64 for the bit31
 * response. Prereq: sOS signed on. Mirrors psp_v14_0_ring_create (bare-metal). */
static BOOL psp_ring_create(struct AmdgpuDevice *ad, UQUAD vram_start)
{
    ULONG base     = ad->ip.psp.base;
    UQUAD ring_off = (UQUAD)260 << 20;          /* clear of the 256 MiB fw staging */
    UQUAD ring_mc  = vram_start + ring_off;
    ULONG zero[8]  = { 0, 0, 0, 0, 0, 0, 0, 0 };
    ULONG i, t, resp = 0;

    D(bug("[amdgpu-psp] ring_create pre: C2PMSG35=%08lx 64=%08lx 67=%08lx "
          "69=%08lx 70=%08lx 71=%08lx 81=%08lx 102=%08lx\n",
          (unsigned long)amdgpu_rreg32(ad, base + MP0_C2PMSG_35),
          (unsigned long)amdgpu_rreg32(ad, base + MP0_C2PMSG_64),
          (unsigned long)amdgpu_rreg32(ad, base + MP0_C2PMSG_67),
          (unsigned long)amdgpu_rreg32(ad, base + MP0_C2PMSG_69),
          (unsigned long)amdgpu_rreg32(ad, base + MP0_C2PMSG_70),
          (unsigned long)amdgpu_rreg32(ad, base + MP0_C2PMSG_71),
          (unsigned long)amdgpu_rreg32(ad, base + MP0_C2PMSG_81),
          (unsigned long)amdgpu_rreg32(ad, base + MP0_C2PMSG_102)));

    /* Linux refuses to program a bare-metal ring until C2PMSG_64 reports both
     * response bit 31 and a zero status field. A non-zero status is not ready. */
    for (t = 0; t < 20000; t++) {
        resp = amdgpu_rreg32(ad, base + MP0_C2PMSG_64);
        if ((resp & PSP_MBOX_MASK) == PSP_MBOX_RESP)
            break;
        psp_spin(2000);
    }
    if ((resp & PSP_MBOX_MASK) != PSP_MBOX_RESP) {
        D(bug("[amdgpu-psp] ring_create: sOS not ready (C2PMSG_64=0x%08lx)\n",
              (unsigned long)resp));
        return FALSE;
    }

    /* zero the 0x1000-byte ring buffer in VRAM (128 chunks of 8 dwords) */
    for (i = 0; i < (PSP_RING_SIZE / 32); i++)
        amdgpu_vram_write(ad, ring_off + (UQUAD)i * 32, zero, 8);

    D(bug("[amdgpu-psp] ring_create: KM ring @VRAM 260MiB MC=0x%08lx%08lx size=0x%lx\n",
          (unsigned long)(ring_mc >> 32), (unsigned long)(ring_mc & 0xFFFFFFFFUL),
          (unsigned long)PSP_RING_SIZE));

    amdgpu_wreg32(ad, base + MP0_C2PMSG_69, (ULONG)(ring_mc & 0xFFFFFFFFUL));
    amdgpu_wreg32(ad, base + MP0_C2PMSG_70, (ULONG)(ring_mc >> 32));
    amdgpu_wreg32(ad, base + MP0_C2PMSG_71, PSP_RING_SIZE);
    amdgpu_wreg32(ad, base + MP0_C2PMSG_64, (ULONG)PSP_RING_TYPE_KM << 16);
    psp_spin(2000000);   /* ~mdelay(20) */

    for (t = 0; t < 20000; t++) {
        resp = amdgpu_rreg32(ad, base + MP0_C2PMSG_64);
        if ((resp & PSP_MBOX_MASK) == PSP_MBOX_RESP)
            break;
        psp_spin(2000);
    }
    if ((resp & PSP_MBOX_MASK) == PSP_MBOX_RESP) {
        ULONG wptr = amdgpu_rreg32(ad, base + MP0_C2PMSG_67);
        D(bug("[amdgpu-psp] *** PSP KM RING CREATED *** C2PMSG_64=0x%08lx — driver "
              "command channel to the secure OS is open; wptr=%lu\n",
              (unsigned long)resp, (unsigned long)wptr));
        return TRUE;
    }
    D(bug("[amdgpu-psp] ring_create: response/status failure (C2PMSG_64=0x%08lx)\n",
          (unsigned long)resp));
    return FALSE;
}

/* ---- GPCOM ring command submission ------------------------------------- *
 * Post a prepared psp_gfx_cmd_resp (in a host buffer) to the KM ring, wait for
 * the fence, and return whether transport completed. The PSP response status is
 * reported separately: Linux warns about a non-zero value during initialization
 * but still consumes response fields because some PSP versions leave it stale.
 * Mirrors psp_cmd_submit_buf + psp_ring_cmd_submit; bare-metal wptr get/set both
 * use C2PMSG_67 (C2PMSG_102 is the SR-IOV path only). */

/* GFX state snapshot taken around EVERY PSP command.
 *
 * The GC memory-path resets never release: GFX_IMU_GFX_RESET_CTRL sits at 0x30
 * (GRBM_RESETB|DFLL_SRESETB) where a healthy card reaches 0x7F (also HARD, EA,
 * UTCL2, SDMA, SE_EA). On the PSP load path Linux NEVER writes that register —
 * gfx_v12_0_hw_init() calls no IMU function when load_type == AMDGPU_FW_LOAD_PSP,
 * and only the DIRECT path's imu_v12_0_wait_for_reset_status() even reads it. So
 * the register is owned by the IMU FIRMWARE, which is alive here
 * (GFX_IMU_SCRATCH_1 == 0xBABEFACE) yet stops halfway. The open question is not
 * "who releases it" but "what is the running IMU waiting for".
 *
 * Sampling only at stage boundaries cannot answer that. This snapshots the whole
 * GFX state either side of every single PSP command, so the exact command that
 * SHOULD flip a bit and does not becomes visible in one capture. Read-only.
 * SCRATCH_10 is included because it is plausibly the IMU's phase code — Linux's
 * debug path writes SCRATCH_10 |= 0x20010007, so the low nibble carries meaning.
 */
#define GC1_GFX_IMU_GFX_RESET_CTRL   0x40bc
#define GC1_GFX_IMU_CORE_CTRL        0x40b6
#define GC1_GFX_IMU_SCRATCH_10       0x4072
#define GC1_GFX_IMU_SCRATCH_1        0x4069
#define GC1_RLC_STAT                 0x4c04
#define GC1_RLC_RLCS_GPM_STAT        0x4e6c

static void psp_gfx_snapshot(struct AmdgpuDevice *ad, const char *tag)
{
    if (!ad->gc_seg[0] || !ad->gc_seg[1])
        return;

    bug("[amdgpu-psp]   GFX[%s] RESET_CTRL=%08lx CORE=%08lx SCR10=%08lx SCR1=%08lx "
        "BOOTLOAD=%08lx CP_STAT=%08lx RLC_STAT=%08lx GPM_STAT=%08lx\n", tag,
        (unsigned long)amdgpu_rreg32(ad, ad->gc_seg[1] + GC1_GFX_IMU_GFX_RESET_CTRL),
        (unsigned long)amdgpu_rreg32(ad, ad->gc_seg[1] + GC1_GFX_IMU_CORE_CTRL),
        (unsigned long)amdgpu_rreg32(ad, ad->gc_seg[1] + GC1_GFX_IMU_SCRATCH_10),
        (unsigned long)amdgpu_rreg32(ad, ad->gc_seg[1] + GC1_GFX_IMU_SCRATCH_1),
        (unsigned long)amdgpu_rreg32(ad, ad->gc_seg[1] + GC1_RLC_RLCS_BOOTLOAD_STATUS),
        (unsigned long)amdgpu_rreg32(ad, ad->gc_seg[0] + GC0_CP_STAT),
        (unsigned long)amdgpu_rreg32(ad, ad->gc_seg[1] + GC1_RLC_STAT),
        (unsigned long)amdgpu_rreg32(ad, ad->gc_seg[1] + GC1_RLC_RLCS_GPM_STAT));
}

static ULONG psp_gfx_cmd_counter = 0;

static BOOL psp_gpcom_submit(struct AmdgpuDevice *ad, UQUAD vram_start,
                             const ULONG *cmd, ULONG cmd_dwords,
                             ULONG *response_status)
{
    ULONG base     = ad->ip.psp.base;
    UQUAD cmd_mc   = vram_start + PSP_CMD_VRAM_OFF;
    UQUAD fence_mc = vram_start + PSP_FENCE_VRAM_OFF;
    ULONG frame[16];
    ULONG wptr, newwptr, i, fence_rb = 0, status = 0xFFFFFFFFUL, zero = 0;

    psp_gfx_snapshot(ad, "pre ");
    ULONG index = ++psp_gfx_cmd_counter;
    const ULONG ring_size_dw = PSP_RING_SIZE / 4;   /* 1024 */

    /* stage the command buffer + clear the fence in VRAM */
    amdgpu_vram_write(ad, PSP_CMD_VRAM_OFF, cmd, cmd_dwords);
    amdgpu_vram_write(ad, PSP_FENCE_VRAM_OFF, &zero, 1);

    /* Bare-metal producer pointer is authoritative in C2PMSG_67. */
    wptr = amdgpu_rreg32(ad, base + MP0_C2PMSG_67);
    if (wptr >= ring_size_dw || (wptr & 15)) {
        bug("[amdgpu-psp] GPCOM invalid C2PMSG_67 wptr=%lu ring_dwords=%lu\n",
            (unsigned long)wptr, (unsigned long)ring_size_dw);
        return FALSE;
    }
    /* build the 64-byte rb_frame and drop it at ring[wptr] */
    { ULONG z; for (z = 0; z < 16; z++) frame[z] = 0; }
    frame[0] = (ULONG)(cmd_mc & 0xFFFFFFFFUL);   /* cmd_buf_addr_lo */
    frame[1] = (ULONG)(cmd_mc >> 32);            /* cmd_buf_addr_hi */
    /* cmd_buf_size remains zero: Linux zeroes the frame and does not set it. */
    frame[3] = (ULONG)(fence_mc & 0xFFFFFFFFUL); /* fence_addr_lo   */
    frame[4] = (ULONG)(fence_mc >> 32);          /* fence_addr_hi   */
    frame[5] = index;                            /* fence_value     */
    amdgpu_vram_write(ad, PSP_RING_OFF + (UQUAD)wptr * 4, frame, 16);

    /* Flush write-combine buffering before signaling the PSP: read the just-
     * written ring frame + command header back so the SOS is guaranteed to fetch
     * the bytes we wrote, not stale/zero VRAM (Linux calls amdgpu_device_flush_hdp
     * right before psp_ring_set_wptr). Without this, a command whose SOS handler
     * PARSES its buffer — LOAD_TOC — can reject with native status 0x11. */
    { ULONG fl; amdgpu_vram_read(ad, PSP_RING_OFF + (UQUAD)wptr * 4, &fl, 1);
                amdgpu_vram_read(ad, PSP_CMD_VRAM_OFF, &fl, 1); (void)fl; }

    /* advance wptr by one frame (16 dwords) and signal the PSP */
    newwptr = (wptr + 16) % ring_size_dw;
    amdgpu_wreg32(ad, base + MP0_C2PMSG_67, newwptr);

    /* wait for the PSP to write our fence value */
    for (i = 0; i < 200000; i++) {
        amdgpu_vram_read(ad, PSP_FENCE_VRAM_OFF, &fence_rb, 1);
        if (fence_rb == index)
            break;
        psp_spin(2000);
    }
    /* resp.status lives at offset 864 in the command buffer */
    amdgpu_vram_read(ad, PSP_CMD_VRAM_OFF + 864, &status, 1);

    if (fence_rb != index) {
        bug("[amdgpu-psp] GPCOM submit TIMEOUT (fence=0x%lx want 0x%lx, wptr %lu->%lu)\n",
            (unsigned long)fence_rb, (unsigned long)index,
            (unsigned long)wptr, (unsigned long)newwptr);
        if (response_status)
            *response_status = status;
        return FALSE;
    }
    if (response_status)
        *response_status = status;
    psp_gfx_snapshot(ad, "post");
    return TRUE;
}

/* Build + submit a LOAD_IP_FW command for firmware at absolute address fw_mc.
 * fw_mc is either a raw VRAM MC address (address-tolerant SMC/IMU) or a GART VA
 * (the autoload set — RLC/CP/MES — which the PSP resolves through the GART).
 * vram_start is still needed: psp_gpcom_submit's cmd/fence buffers live in VRAM. */
static BOOL psp_load_ip_fw_report(struct AmdgpuDevice *ad, UQUAD vram_start, UQUAD fw_mc,
                           ULONG fw_size, ULONG fw_type, const char *name, ULONG *response)
{
    ULONG cmd[256];
    ULONG status = 0xFFFFFFFFUL, fw_addr_lo = 0, fw_addr_hi = 0, z;
    BOOL completed;

    for (z = 0; z < 256; z++) cmd[z] = 0;
    cmd[0] = 1024;                        /* buf_size    @0  */
    cmd[1] = PSP_GFX_CMD_BUF_VERSION;     /* buf_version @4  */
    cmd[2] = PSP_GFX_CMD_LOAD_IP_FW;      /* cmd_id      @8  */
    cmd[7] = (ULONG)(fw_mc & 0xFFFFFFFFUL); /* cmd_load_ip_fw.fw_phy_addr_lo @28 */
    cmd[8] = (ULONG)(fw_mc >> 32);          /* fw_phy_addr_hi @32 */
    cmd[9] = fw_size;                        /* fw_size        @36 */
    cmd[10] = fw_type;                       /* fw_type        @40 */

    completed = psp_gpcom_submit(ad, vram_start, cmd, 256, &status);
    if (response) *response = status;
    if (completed) {
        amdgpu_vram_read(ad, PSP_CMD_VRAM_OFF + 872, &fw_addr_lo, 1);
        amdgpu_vram_read(ad, PSP_CMD_VRAM_OFF + 876, &fw_addr_hi, 1);
    }
    bug("[amdgpu-psp] LOAD_IP_FW %-10s type=%lu addr=0x%08lx%08lx size=%lu -> transport=%s "
        "status=0x%08lx tmr_addr=0x%08lx%08lx%s\n",
        name, (unsigned long)fw_type,
        (unsigned long)(fw_mc >> 32),
        (unsigned long)(fw_mc & 0xFFFFFFFFUL),
        (unsigned long)fw_size,
        completed ? "complete" : "TIMEOUT", (unsigned long)status,
        (unsigned long)fw_addr_hi, (unsigned long)fw_addr_lo,
        completed && status != 0 ? " (status advisory; response retained)" : "");
    if (ad->gart_ready && fw_mc < ((UQUAD)512 << 20))
        amdgpu_gart_dump_fault(ad, name);
    return completed;
}

static BOOL psp_load_ip_fw(struct AmdgpuDevice *ad, UQUAD vram_start, UQUAD fw_mc,
                           ULONG fw_size, ULONG fw_type, const char *name)
{
    return psp_load_ip_fw_report(ad, vram_start, fw_mc, fw_size, fw_type, name, NULL);
}

/* Have the PSP load the IMU firmware (IMU_I + IMU_D). The PSP writes+powers the
 * GFX/IMU block that amdgpu cannot (write-blocked in the live-GOP state); once the
 * IMU is loaded+powered it runs and releases the GFX reset. */
/* GFX12 PSP firmware type IDs (psp_gfx_if.h) for the full RLC-autoload set. */
#define FWT_SMU                18
#define FWT_VCN                13
#define FWT_RLC_G              8
#define FWT_RLC_GPM_MEM        20
#define FWT_RLC_SRM_MEM        21
#define FWT_RLC_IRAM           26
#define FWT_RLC_DRAM_BOOT      48
#define FWT_CP_MES             33
#define FWT_MES_STACK          34
#define FWT_SDMA_RS64          71
#define FWT_CP_MES1            81
#define FWT_MES1_STACK         82
#define FWT_RS64_PFP           87
#define FWT_RS64_ME            88
#define FWT_RS64_MEC           89
#define FWT_RS64_PFP_P0_STACK  90
#define FWT_RS64_ME_P0_STACK   92
#define FWT_RS64_MEC_P0_STACK  94
#define FWT_RS64_MEC_P1_STACK  95

/* Running GFX firmware staging area, above the IMU slots (272 MiB up). */
#define PSP_FW_STAGE_OFF   ((UQUAD)272 << 20)

/* Stage a ucode region (ucode[0..size]) at *off (4KB-aligned) and LOAD_IP_FW it
 * under fw_type. Advances *off. When the GART is enabled, *off is an offset into
 * the system-RAM arena and the PSP is handed a GART VA (the Linux-correct path for
 * the autoload set); otherwise it falls back to raw-VRAM staging (legacy). */
static BOOL psp_stage_load_report(struct AmdgpuDevice *ad, UQUAD vram_start, UQUAD *off,
                           const UBYTE *ucode, ULONG size, ULONG fw_type, const char *name,
                           ULONG *response)
{
    UQUAD fw_mc;
    BOOL completed;

    if (response) *response = 0xffffffffUL;
    if (!ucode || !size)
        return FALSE;
    *off = (*off + 0xFFFUL) & ~(UQUAD)0xFFFUL;        /* 4KB align */

    if (ad->gart_ready) {
        if (*off + size > ad->gart_arena_size) {
            bug("[amdgpu-psp] %s: GART arena overflow (off=0x%08lx size=%lu cap=0x%lx)\n",
                name, (unsigned long)*off, (unsigned long)size,
                (unsigned long)ad->gart_arena_size);
            return FALSE;
        }
        CopyMem((APTR)ucode, ad->gart_arena + *off, size);
        fw_mc = ad->gart_fw_va + *off;
    } else {
        amdgpu_vram_write(ad, *off, (const ULONG *)ucode, (size + 3) / 4);
        fw_mc = vram_start + *off;
    }
    completed = psp_load_ip_fw_report(ad, vram_start, fw_mc, size, fw_type, name, response);
    *off += (size + 0xFFFUL) & ~(UQUAD)0xFFFUL;
    return completed;
}

static void psp_stage_load(struct AmdgpuDevice *ad, UQUAD vram_start, UQUAD *off,
                           const UBYTE *ucode, ULONG size, ULONG fw_type, const char *name)
{
    (void)psp_stage_load_report(ad, vram_start, off, ucode, size, fw_type, name, NULL);
}

/* Decode exactly the gfx_firmware_header_v2_0 consumed by GC12. Validate all
 * extents BEFORE forming a payload pointer. The container CRC is provenance,
 * not an assertion of authentication; PSP responses remain separately visible.
 */
static BOOL psp_cp_header(const UBYTE *fw, ULONG len, ULONG type,
                           struct AmdgpuFirmwareInfo *info)
{
    ULONG header, payload, bytes, instruction, ib, data, db;
    if (!fw || len < 60 || le32(fw) != len || le16(fw + 8) != 2 ||
        le16(fw + 10) != 0 || le16(fw + 12) != 12 || le16(fw + 14) != 0)
        return FALSE;
    header = le32(fw + 4); payload = le32(fw + 24); bytes = le32(fw + 20);
    instruction = le32(fw + 40); ib = le32(fw + 36);
    data = le32(fw + 48); db = le32(fw + 44);
    if (header < 60 || header > len || payload < header || payload > len ||
        !bytes || bytes > len - payload || !ib || !db ||
        instruction < payload || instruction > payload + bytes ||
        ib > payload + bytes - instruction ||
        data < payload || data > payload + bytes || db > payload + bytes - data ||
        (instruction < data + db && data < instruction + ib) ||
        ((instruction | ib | data | db) & 3)) return FALSE;
    info->abi_version = AMDGPU_FIRMWARE_INFO_ABI_VERSION;
    info->type = type; info->flags = AMDGPU_FIRMWARE_HEADER_VALID;
    info->version = le32(fw + 16); info->feature = le32(fw + 32);
    info->source_bytes = len; info->header_bytes = header;
    info->header_major = 2; info->header_minor = 0;
    info->ip_major = 12; info->ip_minor = 0; info->source_crc32 = le32(fw + 28);
    info->instruction_bytes = ib; info->data_bytes = db;
    info->required_stages = type == AMDGPU_FIRMWARE_MEC ? 7 : 3;
    info->completed_stages = 0;
    info->psp_status[0] = info->psp_status[1] = info->psp_status[2] = 0xffffffffUL;
    return TRUE;
}

static void psp_cp_stage(struct AmdgpuDevice *ad, UQUAD vram_start, UQUAD *off,
                          const UBYTE *fw, struct AmdgpuFirmwareInfo *info,
                          ULONG stage, ULONG fw_type, const char *name)
{
    ULONG offset = le32(fw + (stage ? 48 : 40));
    ULONG bytes = stage ? info->data_bytes : info->instruction_bytes;
    if (psp_stage_load_report(ad, vram_start, off, fw + offset, bytes, fw_type,
                             name, &info->psp_status[stage]))
        info->completed_stages |= 1UL << stage;
}

static void psp_cp_record(struct AmdgpuDevice *ad, struct AmdgpuFirmwareInfo *info)
{
    if (!(info->flags & AMDGPU_FIRMWARE_HEADER_VALID)) return;
    if (info->completed_stages == info->required_stages)
        info->flags |= AMDGPU_FIRMWARE_STAGED;
    amdgpu_firmware_info_record(ad, info);
    bug("[amdgpu-psp] CP firmware type=%lu version=%lu feature=%lu flags=%lx "
        "stages=%lx/%lx response=%08lx/%08lx/%08lx source=%lu crc=%08lx\n",
        (unsigned long)info->type, (unsigned long)info->version,
        (unsigned long)info->feature, (unsigned long)info->flags,
        (unsigned long)info->completed_stages, (unsigned long)info->required_stages,
        (unsigned long)info->psp_status[0], (unsigned long)info->psp_status[1],
        (unsigned long)info->psp_status[2], (unsigned long)info->source_bytes,
        (unsigned long)info->source_crc32);
}

/* Linux's fully armed Navi44 trace submits LOAD_TOC after the two firmware-
 * reservation queries and before SMC. Stage it through the same proven GART
 * mapping as the firmware list. SETUP_TMR remains skipped: boot_time_tmr and
 * autoload_supported make psp_skip_tmr() true, but they do not skip LOAD_TOC. */
#define PSP_GFX_CMD_LOAD_TOC  0x20
#define PSP_TOC_STAGE_SIZE    ((UQUAD)PSP_PRIVATE_STAGE_SIZE)

static BOOL psp_load_toc(struct AmdgpuDevice *ad, UQUAD vram_start, UQUAD *off,
                         const UBYTE *sos, ULONG sos_len)
{
    const UBYTE *toc;
    ULONG toc_sz, cmd[256], status = 0xFFFFFFFFUL;
    ULONG tmr_size = 0, z;
    UQUAD toc_mc;
    BOOL completed;

    if (!ad->gart_ready) {
        bug("[amdgpu-psp] LOAD_TOC: GART unavailable; refusing a raw-VRAM substitute\n");
        return FALSE;
    }
    if (!psp_find_subbin(sos, sos_len, PSP_FW_TYPE_TOC, &toc, &toc_sz)) {
        bug("[amdgpu-psp] LOAD_TOC: packed SOS has no valid TOC descriptor\n");
        return FALSE;
    }

    *off = (*off + 0xFFFUL) & ~(UQUAD)0xFFFUL;
    if (!toc_sz || *off + PSP_TOC_STAGE_SIZE > ad->gart_arena_size ||
        toc_sz > PSP_TOC_STAGE_SIZE) {
        bug("[amdgpu-psp] LOAD_TOC: invalid packed-SOS stage size=%lu "
            "arena_off=0x%08lx arena_cap=0x%lx\n",
            (unsigned long)toc_sz, (unsigned long)*off,
            (unsigned long)ad->gart_arena_size);
        return FALSE;
    }

    /* Linux's fw_pri_buf is a zeroed 1 MiB staging buffer. Preserve that contract
     * while sending only the validated payload length in the command. */
    {
        ULONG *stage = (ULONG *)(ad->gart_arena + *off);
        ULONG words = (ULONG)(PSP_TOC_STAGE_SIZE / sizeof(*stage));
        ULONG i;

        for (i = 0; i < words; i++)
            stage[i] = 0;
    }
    CopyMem((APTR)toc, ad->gart_arena + *off, toc_sz);
    toc_mc = ad->gart_fw_va + *off;
    bug("[amdgpu-psp] LOAD_TOC source=packed psp_14_0_2_sos descriptor type=%lu "
        "size=%lu first=0x%08lx\n",
        (unsigned long)PSP_FW_TYPE_TOC, (unsigned long)toc_sz,
        (unsigned long)(toc_sz >= 4 ? le32(toc) : 0));

    for (z = 0; z < 256; z++) cmd[z] = 0;
    cmd[0] = 1024;
    cmd[1] = PSP_GFX_CMD_BUF_VERSION;
    cmd[2] = PSP_GFX_CMD_LOAD_TOC;
    cmd[7] = (ULONG)(toc_mc & 0xFFFFFFFFUL);
    cmd[8] = (ULONG)(toc_mc >> 32);
    cmd[9] = toc_sz;

    completed = psp_gpcom_submit(ad, vram_start, cmd, 256, &status);
    if (completed)
        amdgpu_vram_read(ad, PSP_CMD_VRAM_OFF + 880, &tmr_size, 1);
    bug("[amdgpu-psp] LOAD_TOC addr=0x%08lx%08lx size=%lu -> transport=%s "
        "status=0x%08lx tmr_size=0x%08lx\n",
        (unsigned long)(toc_mc >> 32),
        (unsigned long)(toc_mc & 0xFFFFFFFFUL),
        (unsigned long)toc_sz, completed ? "complete" : "TIMEOUT",
        (unsigned long)status, (unsigned long)tmr_size);
    amdgpu_gart_dump_fault(ad, "LOAD_TOC");

    *off += PSP_TOC_STAGE_SIZE;
    return completed && status == 0;
}

/* FB firmware-reservation handshake. Query the PSP's boot-time reserved region
 * before direct firmware loads, as Linux psp_hw_start() does. The returned region
 * feeds the driver's immutable VRAM exclusion map before any managed BO. The
 * response's fw_reserve_info union sits at resp+64 (resp@864), i.e.
 * base_hi@928 base_lo@932 size@936 in the command buffer. */
static void psp_query_fw_reservation(struct AmdgpuDevice *ad, UQUAD vram_start)
{
    ULONG cmd[256], status, z, base_hi, base_lo, size;
    BOOL completed;
    int i;

    for (i = 0; i < 2; i++) {
        ULONG cid = i ? 0x51 : 0x50;
        for (z = 0; z < 256; z++) cmd[z] = 0;
        cmd[0] = 1024; cmd[1] = PSP_GFX_CMD_BUF_VERSION; cmd[2] = cid;
        completed = psp_gpcom_submit(ad, vram_start, cmd, 256, &status);
        base_hi = base_lo = size = 0;
        if (completed) {
            amdgpu_vram_read(ad, PSP_CMD_VRAM_OFF + 928, &base_hi, 1);
            amdgpu_vram_read(ad, PSP_CMD_VRAM_OFF + 932, &base_lo, 1);
            amdgpu_vram_read(ad, PSP_CMD_VRAM_OFF + 936, &size,    1);
        }
        bug("[amdgpu-psp] FB_FW_RESERV%s(0x%02lx) -> transport=%s status=0x%08lx "
            "base=0x%08lx%08lx size=0x%08lx\n",
            i ? "_EXT" : "    ", (unsigned long)cid,
            completed ? "complete" : "TIMEOUT", (unsigned long)status,
            (unsigned long)base_hi, (unsigned long)base_lo, (unsigned long)size);
        amdgpu_vram_record_psp(ad, i, completed, status,
            ((UQUAD)base_hi << 32) | base_lo, size);
    }
}

/* Observe the PSP-owned autoload path before amdgpu_gfx_hw_init can touch IMU
 * state. This is intentionally read-only and uses Linux's exact completion
 * condition: CP idle and RLC bootload complete. */
static void psp_wait_rlc_autoload_state(struct AmdgpuDevice *ad)
{
    ULONG cp_initial, boot_initial, cp_status, boot_status, t;

    if (!ad->gc_seg[0] || !ad->gc_seg[1]) {
        bug("[amdgpu-psp] RLC autoload state unavailable: GC segments missing\n");
        return;
    }

    cp_initial = amdgpu_rreg32(ad, ad->gc_seg[0] + GC0_CP_STAT);
    boot_initial = amdgpu_rreg32(ad,
        ad->gc_seg[1] + GC1_RLC_RLCS_BOOTLOAD_STATUS);
    cp_status = cp_initial;
    boot_status = boot_initial;

    /* Existing psp_spin calibration makes this a bounded ~100 ms observation,
     * matching Linux's normal usec_timeout scale without timer.device. */
    for (t = 0; t < 5000; t++) {
        cp_status = amdgpu_rreg32(ad, ad->gc_seg[0] + GC0_CP_STAT);
        boot_status = amdgpu_rreg32(ad,
            ad->gc_seg[1] + GC1_RLC_RLCS_BOOTLOAD_STATUS);
        if (cp_status == 0 && (boot_status & RLC_BOOTLOAD_COMPLETE))
            break;
        psp_spin(2000);
    }

    bug("[amdgpu-psp] RLC autoload hardware: initial CP_STAT=0x%08lx "
        "BOOTLOAD_STATUS=0x%08lx; final CP_STAT=0x%08lx "
        "BOOTLOAD_STATUS=0x%08lx after %lu polls -> %s\n",
        (unsigned long)cp_initial, (unsigned long)boot_initial,
        (unsigned long)cp_status, (unsigned long)boot_status,
        (unsigned long)t,
        (cp_status == 0 && (boot_status & RLC_BOOTLOAD_COMPLETE))
            ? "COMPLETE" : "TIMEOUT");
}

/* PSP-load the full GFX12 firmware set, then trigger AUTOLOAD_RLC so the PSP
 * powers the (otherwise write-blocked) GFX block. Header offsets: common ucode
 * @20/@24; gfx_firmware_header_v2_0 (RS64 CP) ucode @36/@40 data @44/@48; mes_v1_0
 * ucode @36/@40 data @48/@52; rlc v2.1/2.2 sub-blobs at the fixed field offsets. */
static void psp_load_gfx_firmware(struct AmdgpuDevice *ad, UQUAD vram_start,
                                  const UBYTE *sos, ULONG sos_len)
{
    UBYTE *smc=NULL,*sdma=NULL,*imu=NULL,*rlc=NULL,*pfp=NULL,*me=NULL,*mec=NULL,*mes=NULL,*vcn=NULL;
    ULONG  smc_len=0,sdma_len=0,imu_len=0,rlc_len=0,pfp_len=0,me_len=0,mec_len=0,mes_len=0,vcn_len=0;
    UQUAD  voff;
    struct AmdgpuFirmwareInfo pfp_info = {0}, me_info = {0}, mec_info = {0};

    amdgpu_firmware_info_reset(ad);

    /* Enable the PCIE GART first, then stage the autoload set at GART VAs. Probe
     * the same IMU-I payload through GART and raw VRAM so address translation
     * remains isolated from firmware-type semantics in the DUT evidence. */
    amdgpu_gart_enable(ad, vram_start);
    voff = ad->gart_ready ? 0 : PSP_FW_STAGE_OFF;

    /* Query the boot-time firmware reservation before direct firmware loads,
     * matching psp_hw_start(). */
    psp_query_fw_reservation(ad, vram_start);

    /* LOAD_TOC is REQUIRED here, immediately after the two reservation queries
     * and before SMC. The FULLY ARMED kprobe capture of the exact Ubuntu kernel
     * that drives this exact card (qemu-test/linux-psp-trace/reboot4.out) reads:
     *     id=80 type=0                       FB_FW_RESERV
     *     id=81 type=0                       FB_FW_RESERV_EXT
     *     id=32 type=0 lo=7340032 sz=2304    LOAD_TOC   <-- here
     *     id=6  type=18 ...                  SMC, then the LOAD_IP_FW array
     * The TOC is the autoload manifest: without it the PSP has nothing to
     * autoload into and rejects the whole GFX ucode set. */
    (void)psp_load_toc(ad, vram_start, &voff, sos, sos_len);

    /* SMC/PMFW next — the RLC autoload relies on the SMU firmware being loaded
     * first (gfx_v12_0.c: "rlc firmware loading relies on smu firmware loaded
     * firstly"); it governs the GFX power rails the PSP ungates at AUTOLOAD_RLC. */
    if (amdgpu_request_firmware("smu_14_0_2.bin", &smc, &smc_len) && smc) {
        ULONG ao = le32(smc+24), sz = le32(smc+20);
        if (ao && ao + sz <= smc_len)
            psp_stage_load(ad, vram_start, &voff, smc+ao, sz, FWT_SMU, "SMC");
        else
            bug("[amdgpu-psp] smu_14_0_2.bin header bad (ao=%lu sz=%lu len=%lu)\n",
                (unsigned long)ao, (unsigned long)sz, (unsigned long)smc_len);
        amdgpu_release_firmware(smc);
    } else {
        bug("[amdgpu-psp] smu_14_0_2.bin MISSING — GFX power rails cannot be ungated\n");
    }

    /* Linux submits the populated firmware array in AMDGPU_UCODE_ID order.
     * GFX12 starts with the single RS64 SDMA payload, then the three CP
     * instruction blobs, and only then the stack entries initialized by
     * gfx_v12_0_init_microcode(). */
    if (amdgpu_request_firmware("sdma_7_0_0.bin", &sdma, &sdma_len) && sdma) {
        ULONG ao = le32(sdma+24), sz = le32(sdma+40);
        if (ao && sz && ao + sz <= sdma_len)
            psp_stage_load(ad, vram_start, &voff, sdma+ao, sz,
                           FWT_SDMA_RS64, "SDMA_RS64");
        else
            bug("[amdgpu-psp] sdma_7_0_0.bin header bad (ao=%lu sz=%lu len=%lu)\n",
                (unsigned long)ao, (unsigned long)sz, (unsigned long)sdma_len);
    }

    /* RS64 CP instruction blobs. */
    if (amdgpu_request_firmware("gc_12_0_0_pfp.bin", &pfp, &pfp_len) && pfp) {
        if (psp_cp_header(pfp, pfp_len, AMDGPU_FIRMWARE_PFP, &pfp_info))
            psp_cp_stage(ad, vram_start, &voff, pfp, &pfp_info, 0, FWT_RS64_PFP, "PFP");
        else
            bug("[amdgpu-psp] PFP CP header invalid; instruction/stack not submitted\n");
    }
    if (amdgpu_request_firmware("gc_12_0_0_me.bin", &me, &me_len) && me) {
        if (psp_cp_header(me, me_len, AMDGPU_FIRMWARE_ME, &me_info))
            psp_cp_stage(ad, vram_start, &voff, me, &me_info, 0, FWT_RS64_ME, "ME");
        else
            bug("[amdgpu-psp] ME CP header invalid; instruction/stack not submitted\n");
    }
    if (amdgpu_request_firmware("gc_12_0_0_mec.bin", &mec, &mec_len) && mec) {
        if (psp_cp_header(mec, mec_len, AMDGPU_FIRMWARE_MEC, &mec_info))
            psp_cp_stage(ad, vram_start, &voff, mec, &mec_info, 0, FWT_RS64_MEC, "MEC");
        else
            bug("[amdgpu-psp] MEC CP header invalid; instruction/stack not submitted\n");
    }

    /* GFX12 initializes exactly these four stack IDs. PFP P1, ME P1, and MEC
     * P2/P3 remain absent from Linux's firmware array and must not be sent. */
    if (pfp_info.flags & AMDGPU_FIRMWARE_HEADER_VALID)
        psp_cp_stage(ad, vram_start, &voff, pfp, &pfp_info, 1, FWT_RS64_PFP_P0_STACK, "PFP_P0");
    if (me_info.flags & AMDGPU_FIRMWARE_HEADER_VALID)
        psp_cp_stage(ad, vram_start, &voff, me, &me_info, 1, FWT_RS64_ME_P0_STACK, "ME_P0");
    if (mec_info.flags & AMDGPU_FIRMWARE_HEADER_VALID) {
        psp_cp_stage(ad, vram_start, &voff, mec, &mec_info, 1, FWT_RS64_MEC_P0_STACK, "MEC_P0");
        psp_cp_stage(ad, vram_start, &voff, mec, &mec_info, 2, FWT_RS64_MEC_P1_STACK, "MEC_P1");
    }
    psp_cp_record(ad, &pfp_info);
    psp_cp_record(ad, &me_info);
    psp_cp_record(ad, &mec_info);

    /* NON-UNI MES (Navi44 default, enable_uni_mes=false): each MES pipe runs its
     * OWN firmware — mes.bin drives the SCHEDULER pipe (CP_MES/MES_STACK), mes1.bin
     * drives the KIQ pipe (CP_MES1/MES1_STACK). uni_mes.bin is the UNIFIED-model
     * blob; staging it into both pipes leaves the wrong ucode flavor and the pipes
     * boot but never service their queues. Mirror Linux amdgpu_mes_init_microcode. */
    if (amdgpu_request_firmware("gc_12_0_0_mes.bin", &mes, &mes_len) && mes) {
        psp_stage_load(ad, vram_start, &voff, mes+le32(mes+40), le32(mes+36), FWT_CP_MES,    "MES");
        psp_stage_load(ad, vram_start, &voff, mes+le32(mes+52), le32(mes+48), FWT_MES_STACK, "MES_DATA");
        amdgpu_release_firmware(mes); mes = NULL;
    }
    if (amdgpu_request_firmware("gc_12_0_0_mes1.bin", &mes, &mes_len) && mes) {
        psp_stage_load(ad, vram_start, &voff, mes+le32(mes+40), le32(mes+36), FWT_CP_MES1,    "MES1");
        psp_stage_load(ad, vram_start, &voff, mes+le32(mes+52), le32(mes+48), FWT_MES1_STACK, "MES1_DATA");
    }

    /* IMU I/D follow the MES pairs in Linux's firmware array. Submit each once
     * through the proven GART path; retain raw VRAM only as the no-GART fallback. */
    if (amdgpu_request_firmware("gc_12_0_0_imu.bin", &imu, &imu_len) && imu) {
        ULONG ao = le32(imu+24), i_sz = le32(imu+32), d_sz = le32(imu+40);
        if (ao && ao + i_sz + d_sz <= imu_len) {
            if (ad->gart_ready) {
                psp_stage_load(ad, vram_start, &voff, imu+ao, i_sz,
                               PSP_GFX_FW_TYPE_IMU_I, "IMU_I");
                psp_stage_load(ad, vram_start, &voff, imu+ao+i_sz, d_sz,
                               PSP_GFX_FW_TYPE_IMU_D, "IMU_D");
            } else {
                amdgpu_vram_write(ad, PSP_FW_I_VRAM_OFF, (const ULONG *)(imu+ao),
                                  (i_sz+3)/4);
                amdgpu_vram_write(ad, PSP_FW_D_VRAM_OFF, (const ULONG *)(imu+ao+i_sz),
                                  (d_sz+3)/4);
                psp_load_ip_fw(ad, vram_start, vram_start + PSP_FW_I_VRAM_OFF, i_sz,
                               PSP_GFX_FW_TYPE_IMU_I, "IMU_I_RAW");
                psp_load_ip_fw(ad, vram_start, vram_start + PSP_FW_D_VRAM_OFF, d_sz,
                               PSP_GFX_FW_TYPE_IMU_D, "IMU_D_RAW");
            }
        }
    }

    /* RLC sub-components, then RLC_G LAST — Linux loads RLC_G as the final
     * autoload component and fires AUTOLOAD_RLC immediately after it. */
    if (amdgpu_request_firmware("gc_12_0_0_rlc.bin", &rlc, &rlc_len) && rlc) {
        psp_stage_load(ad, vram_start, &voff, rlc+le32(rlc+136), le32(rlc+132), FWT_RLC_GPM_MEM,   "RLC_GPM");
        psp_stage_load(ad, vram_start, &voff, rlc+le32(rlc+152), le32(rlc+148), FWT_RLC_SRM_MEM,   "RLC_SRM");
        psp_stage_load(ad, vram_start, &voff, rlc+le32(rlc+160), le32(rlc+156), FWT_RLC_IRAM,      "RLC_IRAM");
        psp_stage_load(ad, vram_start, &voff, rlc+le32(rlc+168), le32(rlc+164), FWT_RLC_DRAM_BOOT, "RLC_DRAM");
        psp_stage_load(ad, vram_start, &voff, rlc+le32(rlc+24),  le32(rlc+20),  FWT_RLC_G,         "RLC_G");
    }

    /* AUTOLOAD_RLC immediately after RLC_G -> the PSP runs its IMU + RLC autoload
     * and powers up the (otherwise write-blocked) GFX block. */
    {
        ULONG cmd[256], status, z;
        BOOL completed;
        for (z = 0; z < 256; z++) cmd[z] = 0;
        cmd[0] = 1024;
        cmd[1] = PSP_GFX_CMD_BUF_VERSION;
        cmd[2] = 0x21;                    /* GFX_CMD_ID_AUTOLOAD_RLC */
        completed = psp_gpcom_submit(ad, vram_start, cmd, 256, &status);
        bug("[amdgpu-psp] AUTOLOAD_RLC -> transport=%s status=0x%08lx%s\n",
            completed ? "complete" : "TIMEOUT", (unsigned long)status,
            completed && status != 0 ? " (status advisory; verify hardware state)" : "");
    }

    /* The matched Linux dc=0 trace sends VCN type 13 immediately after
     * AUTOLOAD_RLC, before SMU and the later GFX autoload wait. AROS previously
     * stopped at AUTOLOAD. Preserve that exact next command as a bounded A/B:
     * if BOOTLOAD_STATUS changes only after this submission, PSP ring progression
     * is causal; if it remains zero, VCN is eliminated without enabling VCN IP. */
    if (ad->gc_seg[0] && ad->gc_seg[1]) {
        bug("[amdgpu-psp] pre-VCN RLC state: CP_STAT=0x%08lx "
            "BOOTLOAD_STATUS=0x%08lx\n",
            (unsigned long)amdgpu_rreg32(ad, ad->gc_seg[0] + GC0_CP_STAT),
            (unsigned long)amdgpu_rreg32(ad,
                ad->gc_seg[1] + GC1_RLC_RLCS_BOOTLOAD_STATUS));
    }

    if (amdgpu_request_firmware("vcn_5_0_0.bin", &vcn, &vcn_len) && vcn) {
        if (vcn_len >= 28) {
            ULONG ao = le32(vcn + 24);
            ULONG sz = le32(vcn + 20);

            if (ao && sz && (UQUAD)ao + sz <= vcn_len) {
                bug("[amdgpu-psp] Linux-order discriminator: VCN type=%lu "
                    "payload off=%lu size=%lu\n",
                    (unsigned long)FWT_VCN, (unsigned long)ao,
                    (unsigned long)sz);
                psp_stage_load(ad, vram_start, &voff, vcn + ao, sz,
                               FWT_VCN, "VCN");
            } else {
                bug("[amdgpu-psp] vcn_5_0_0.bin header bad "
                    "(ao=%lu sz=%lu len=%lu)\n",
                    (unsigned long)ao, (unsigned long)sz,
                    (unsigned long)vcn_len);
            }
        } else {
            bug("[amdgpu-psp] vcn_5_0_0.bin too small (%lu bytes)\n",
                (unsigned long)vcn_len);
        }
    } else {
        bug("[amdgpu-psp] vcn_5_0_0.bin MISSING — VCN command progression "
            "discriminator unavailable\n");
    }

    psp_wait_rlc_autoload_state(ad);

    if (imu) amdgpu_release_firmware(imu);
    if (rlc) amdgpu_release_firmware(rlc);
    if (sdma) amdgpu_release_firmware(sdma);
    if (pfp) amdgpu_release_firmware(pfp);
    if (me)  amdgpu_release_firmware(me);
    if (mec) amdgpu_release_firmware(mec);
    if (mes) amdgpu_release_firmware(mes);
    if (vcn) amdgpu_release_firmware(vcn);
}

BOOL amdgpu_psp_hw_init(struct AmdgpuDevice *ad)
{
    static const char * const sos_name = "amdgpu/psp_14_0_2_sos.bin";
    UBYTE *fw = NULL;
    ULONG  fwsize = 0;

    D(bug("[amdgpu-psp] %s: MP0 (PSP) base 0x%05lx%s\n", ad->name,
          (unsigned long)ad->ip.psp.base,
          ad->ip.psp.valid ? "" : " (NOT discovered!)"));

    /* Fetch the SOS from DEVS:Firmware/amdgpu. A miss is not fatal in the
     * observe-only ladder — we just cannot bring PSP up yet. */
    if (!amdgpu_request_firmware(sos_name, &fw, &fwsize)) {
        D(bug("[amdgpu-psp] SOS '%s' unavailable on disk — deferring PSP bring-up\n",
              sos_name));
        return TRUE;
    }

    if (fwsize < 32) {
        D(bug("[amdgpu-psp] SOS blob too small (%lu bytes) — ignoring\n",
              (unsigned long)fwsize));
        amdgpu_release_firmware(fw);
        return TRUE;
    }

    {
        ULONG size_bytes = le32(fw + 0);
        ULONG hdr_size   = le32(fw + 4);
        UWORD hv_major   = le16(fw + 8);
        UWORD hv_minor   = le16(fw + 10);
        UWORD ipv_major  = le16(fw + 12);
        UWORD ipv_minor  = le16(fw + 14);
        ULONG ucode_ver  = le32(fw + 16);
        ULONG ucode_size = le32(fw + 20);
        ULONG ucode_off  = le32(fw + 24);
        BOOL  ok;

        D(bug("[amdgpu-psp] SOS fetched %lu bytes | container: size=%lu hdr=%lu "
              "hdr_ver %u.%u ip_ver %u.%u ucode_ver 0x%08lx ucode_size=%lu off=%lu\n",
              (unsigned long)fwsize, (unsigned long)size_bytes,
              (unsigned long)hdr_size, hv_major, hv_minor, ipv_major, ipv_minor,
              (unsigned long)ucode_ver, (unsigned long)ucode_size,
              (unsigned long)ucode_off));

        /* Two independent self-consistency checks prove we have REAL PSP 14
         * firmware in hand (not a truncated/garbled blob): the container's
         * declared size must equal what we loaded, and the IP version must be
         * the 14.x we expect for Navi 44. */
        ok = TRUE;
        if (size_bytes != fwsize) {
            D(bug("[amdgpu-psp] WARN container size_bytes(%lu) != loaded(%lu)\n",
                  (unsigned long)size_bytes, (unsigned long)fwsize));
            ok = FALSE;
        }
        if (ipv_major != 14) {
            D(bug("[amdgpu-psp] WARN ip_version_major=%u (expected 14 for Navi 44)\n",
                  ipv_major));
            ok = FALSE;
        }
        if (ok)
            D(bug("[amdgpu-psp] SOS container VALIDATED — PSP %u.%u firmware staged, "
                  "ready for the MP0 bootloader handshake (next tier)\n",
                  ipv_major, ipv_minor));
    }

    /* OBSERVE-FIRST MP0 contact — READ ONLY, no register writes yet. POST already
     * ran the VBIOS which brought PSP up and lit the panel, so if our register
     * addressing is right the sign-of-life reads non-zero and the bootloader
     * reports ready. Reading POST's live PSP state both PROVES we can reach the
     * MP0 mailbox at the discovered base and tells us the state to drive from.
     * A read anywhere inside the mapped BAR is side-effect-free and cannot fault. */
    if (ad->rmmio && ad->ip.psp.valid) {
        ULONG base = ad->ip.psp.base;
        ULONG sol  = amdgpu_rreg32(ad, base + MP0_C2PMSG_81);
        ULONG bl   = amdgpu_rreg32(ad, base + MP0_C2PMSG_35);

        D(bug("[amdgpu-psp] MP0 probe @base 0x%05lx: C2PMSG_81(sol)=0x%08lx "
              "C2PMSG_35(bl)=0x%08lx\n", (unsigned long)base,
              (unsigned long)sol, (unsigned long)bl));
        D(bug("[amdgpu-psp]   sign-of-life: %s | bootloader: %s\n",
              sol ? "ALIVE (POST loaded sOS)" : "cold/zero",
              (bl & PSP_BL_READY) ? "READY (bit31)" : "not-ready"));
        if (bl & PSP_BL_READY)
            D(bug("[amdgpu-psp] *** MP0 ADDRESSING CONFIRMED — bootloader ready sentinel "
                  "(0x80000000) read on bare metal; sos alive=%s ***\n",
                  sol ? "yes" : "no (we get to load it)"));
    } else {
        D(bug("[amdgpu-psp] MP0 not probed (rmmio=%p psp.valid=%d)\n",
              ad->rmmio, (int)ad->ip.psp.valid));
    }

    /* Last two unknowns for the SOS-load handshake, proven observe-first (a read
     * plus a scratch VRAM roundtrip well clear of anything in use) before we ever
     * stage 421 KB and issue a real load command to the silicon:
     *   (a) the VRAM MC base (vram_start) the PSP addresses firmware by, and
     *   (b) that our VRAM write path works (needed to stage the SOS into VRAM). */
    if (ad->rmmio && ad->ip.mmhub.valid) {
        ULONG mb      = ad->ip.mmhub.base;
        ULONG fb_base = amdgpu_rreg32(ad, mb + MMHUB_FB_LOCATION_BASE);
        ULONG fb_top  = amdgpu_rreg32(ad, mb + MMHUB_FB_LOCATION_TOP);
        ULONG fb_off  = amdgpu_rreg32(ad, mb + MMHUB_FB_OFFSET);
        UQUAD vram_start = ((UQUAD)(fb_base & 0x00FFFFFFUL)) << 24;

        D(bug("[amdgpu-psp] mmhub@0x%05lx FB_LOCATION_BASE=0x%08lx TOP=0x%08lx OFFSET=0x%08lx\n",
              (unsigned long)mb, (unsigned long)fb_base,
              (unsigned long)fb_top, (unsigned long)fb_off));
        D(bug("[amdgpu-psp] derived VRAM MC start = 0x%08lx%08lx\n",
              (unsigned long)(vram_start >> 32),
              (unsigned long)(vram_start & 0xFFFFFFFFUL)));

        /* Scratch roundtrip at 256 MiB into VRAM — above the framebuffer (low
         * VRAM) and far below the discovery table (top), so nothing in use is
         * touched. MM_INDEX/MM_DATA VRAM access is already proven (discovery read
         * the table through it); this confirms the write direction too. */
        {
            UQUAD scratch = (UQUAD)256 << 20;
            ULONG wpat[4] = { 0xA5A5F00DUL, 0x12345678UL, 0xDEADBEEFUL, 0x0BADC0DEUL };
            ULONG rbk[4]  = { 0, 0, 0, 0 };

            amdgpu_vram_write(ad, scratch, wpat, 4);
            amdgpu_vram_read(ad, scratch, rbk, 4);

            D(bug("[amdgpu-psp] VRAM r/w @256MiB: wrote %08lx.%08lx.%08lx.%08lx | "
                  "read %08lx.%08lx.%08lx.%08lx\n",
                  (unsigned long)wpat[0], (unsigned long)wpat[1],
                  (unsigned long)wpat[2], (unsigned long)wpat[3],
                  (unsigned long)rbk[0], (unsigned long)rbk[1],
                  (unsigned long)rbk[2], (unsigned long)rbk[3]));
            if (rbk[0] == wpat[0] && rbk[1] == wpat[1] &&
                rbk[2] == wpat[2] && rbk[3] == wpat[3])
                D(bug("[amdgpu-psp] *** VRAM write path VERIFIED — all primitives for the "
                      "SOS load are proven (mailbox + MC base + VRAM stage) ***\n"));
            else
                D(bug("[amdgpu-psp] VRAM roundtrip MISMATCH — staging not trustworthy yet\n"));
        }
    }

    /* ================================ THE WAKE ================================
     * Drive the psp_v14_0 bootloader to load KDB -> (SPL) -> SYSDRV -> SOS from
     * the container, then poll sign-of-life. This is the first time the GPU's own
     * security processor executes our firmware. Only attempted when the bootloader
     * is ready and the sOS is not already up; every poll is bounded so a wedged PSP
     * is recoverable by power-cycle, never an infinite hang. */
    if (fw && ad->rmmio && ad->ip.psp.valid && ad->ip.mmhub.valid) {
        ULONG base = ad->ip.psp.base;
        ULONG mb   = ad->ip.mmhub.base;
        ULONG sol0 = amdgpu_rreg32(ad, base + MP0_C2PMSG_81);
        UQUAD vram_start =
            ((UQUAD)(amdgpu_rreg32(ad, mb + MMHUB_FB_LOCATION_BASE) & 0x00FFFFFFUL)) << 24;

        if (sol0 != 0) {
            D(bug("[amdgpu-psp] sign-of-life already set (0x%08lx) — sOS up; skipping load\n",
                  (unsigned long)sol0));
        } else if (!(amdgpu_rreg32(ad, base + MP0_C2PMSG_35) & PSP_BL_READY)) {
            D(bug("[amdgpu-psp] bootloader not ready — not attempting wake\n"));
        } else {
            /* Linux programs the MMHUB GART before psp_hw_start() and uses the
             * same 1 MiB GTT fw_pri_buf for KDB through SOS and LOAD_TOC.  The
             * old AROS sequence used raw VRAM for bootloader loads, then a new
             * GART address for TOC.  Establish the mapping before the first
             * bootloader command so the address lifetime and reuse match. */
            amdgpu_gart_enable(ad, vram_start);
            D(bug("[amdgpu-psp] boot private buffer: %s MC=0x%08lx%08lx "
                  "size=0x%08lx\n",
                  ad->gart_ready ? "GART" : "VRAM fallback",
                  (unsigned long)((ad->gart_ready ? ad->gart_fw_va :
                                   vram_start + PSP_STAGE_VRAM_OFF) >> 32),
                  (unsigned long)((ad->gart_ready ? ad->gart_fw_va :
                                   vram_start + PSP_STAGE_VRAM_OFF) &
                                  0xFFFFFFFFUL),
                  (unsigned long)PSP_PRIVATE_STAGE_SIZE));

            /* Exact psp_hw_start bootloader order (amdgpu_psp.c): each loads only
             * if present in the container. SOS is LAST — the sOS signs on (sets
             * C2PMSG_81) only once its whole driver stack is underneath it. */
            static const struct { ULONG type; ULONG cmd; const char *name; } seq[] = {
                { PSP_FW_TYPE_KDB,      PSP_BL_LOAD_KDB,      "KDB"      },
                { PSP_FW_TYPE_SPL,      PSP_BL_LOAD_SPL,      "SPL"      },
                { PSP_FW_TYPE_SYS_DRV,  PSP_BL_LOAD_SYSDRV,   "SYSDRV"   },
                { PSP_FW_TYPE_SOC_DRV,  PSP_BL_LOAD_SOCDRV,   "SOC_DRV"  },
                { PSP_FW_TYPE_INTF_DRV, PSP_BL_LOAD_INTFDRV,  "INTF_DRV" },
                { PSP_FW_TYPE_DBG_DRV,  PSP_BL_LOAD_DBGDRV,   "DBG_DRV"  },
                { PSP_FW_TYPE_RAS_DRV,  PSP_BL_LOAD_RASDRV,   "RAS_DRV"  },
                { PSP_FW_TYPE_IPKEYMGR, PSP_BL_LOAD_IPKEYMGR, "IPKEYMGR" },
                { PSP_FW_TYPE_SOS,      PSP_BL_LOAD_SOSDRV,   "SOS"      },
            };
            ULONG hs = le32(fw + 4), cnt = le32(fw + 32), i, s;
            BOOL ok = TRUE;

            D(bug("[amdgpu-psp] === WAKE: KDB->SYSDRV->SOS via vram_start 0x%08lx%08lx ===\n",
                  (unsigned long)(vram_start >> 32),
                  (unsigned long)(vram_start & 0xFFFFFFFFUL)));
            D(bug("[amdgpu-psp] container: %lu sub-binaries (hdr_size %lu)\n",
                  (unsigned long)cnt, (unsigned long)hs));
            for (i = 0; i < cnt && (hs + i * 16 + 16) <= fwsize; i++) {
                ULONG d = hs + i * 16;
                D(bug("[amdgpu-psp]   [%lu] type=%lu off=0x%lx size=%lu\n",
                      (unsigned long)i, (unsigned long)le32(fw + d),
                      (unsigned long)le32(fw + d + 8), (unsigned long)le32(fw + d + 12)));
            }

            for (s = 0; s < sizeof(seq) / sizeof(seq[0]) && ok; s++) {
                const UBYTE *sd;
                ULONG ss;
                if (!psp_find_subbin(fw, fwsize, seq[s].type, &sd, &ss)) {
                    D(bug("[amdgpu-psp] %s not in container — skipping\n", seq[s].name));
                    continue;
                }
                ok = psp_load_component(ad, vram_start, sd, ss, seq[s].cmd, seq[s].name);
            }

            if (ok) {
                ULONG sol = 0, t;
                psp_spin(2000000);   /* ~mdelay: give the sOS time to sign on */
                for (t = 0; t < 20000; t++) {
                    sol = amdgpu_rreg32(ad, base + MP0_C2PMSG_81);
                    if (sol != 0)
                        break;
                    psp_spin(2000);
                }
                if (sol != 0) {
                    D(bug("[amdgpu-psp] *** SILICON AWAKE *** PSP sign-of-life = 0x%08lx: "
                          "the GPU is executing our SOS firmware on bare metal\n",
                          (unsigned long)sol));
                    /* sOS is up — open the KM command ring to it, then PSP-load
                     * the IMU so the PSP powers the (write-blocked) GFX block. */
                    if (psp_ring_create(ad, vram_start))
                        psp_load_gfx_firmware(ad, vram_start, fw, fwsize);
                } else
                    D(bug("[amdgpu-psp] sequence accepted but sign-of-life still 0 — sOS not "
                          "signing on (investigate: order/component/TMR)\n"));
            } else {
                D(bug("[amdgpu-psp] wake aborted — a component was rejected by the bootloader\n"));
            }
        }
    }

    amdgpu_release_firmware(fw);
    return TRUE;
}
