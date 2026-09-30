/*
 * amdgpu_reset.c — SOC24 / Navi 44 (GFX12) mode1 ASIC reset via the SMU debug
 * mailbox.
 *
 * WHY THIS EXISTS
 * ---------------
 * amdgpu.hidd adopts the live GOP scanout (no ASIC reset) so the boot display
 * survives the driver taking over. That convenience is exactly what blocks 3D:
 * the GFX/GC block comes up write-blocked, and the PSP refuses the entire GFX
 * firmware-load path from that GOP-initialised state — proven on the DUT:
 *   LOAD_TOC     -> 0x00000011  (TMR never sized)
 *   GFX fw loads -> 0xffff0006  (x16, no TMR to load into)
 *   AUTOLOAD_RLC -> 0xffff0007  (fw set incomplete)
 * The PSP's GFX path assumes a freshly-reset ASIC, exactly as Linux resets the
 * ASIC before gfx bring-up. So we must reset first, then re-init the whole IP
 * chain (gmc/psp/gfx/dcn) on the clean silicon.
 *
 * THE LEVER (not the PSP)
 * -----------------------
 * For GC12 the reset method is MODE1 (soc24_asic_reset_method), and mode1 for
 * this generation is driven from the SMU (MP1) *debug* mailbox — psp_v14_0 has
 * NO mode1_reset. The debug message is fire-and-forget: it needs no SMU ack,
 * which is why our normal SMU never acking (it is "live but autonomous") never
 * mattered for the reset. Traced from Linux:
 *   smu_v14_0_2_mode1_reset
 *     -> smu_cmn_send_debug_smc_msg(DEBUGSMC_MSG_Mode1Reset = 2)
 *       -> smu_msg_v1_send_debug_msg: WREG debug_param, debug_msg, debug_resp=0
 * with mp_14_0_2 offsets regMP1_SMN_C2PMSG_53(param)=0x0075, _75(msg)=0x008b,
 * _54(resp)=0x0076 — identical to our C2PMSG_N = 0x40 + N scheme off the
 * resolved MP1 base (amdgpu_smu_base()).
 *
 * AFTER THE RESET
 * ---------------
 * mode1 is a warm reset that PRESERVES PCI config (BARs survive), so our OPAL
 * BAR mapping and the parsed IP-discovery bases stay valid — no re-map, no
 * re-discovery. We poll RCC_CONFIG_MEMSIZE until it stops reading 0xffffffff
 * (Linux amdgpu_device_mode1_reset), then the normal IP loop re-inits gmc, psp
 * (bootloader + SOS), gfx and dcn against the clean ASIC. Linux also suppresses
 * all MMIO for a fixed 1000 ms after sending the SMU 14.0.2 reset command;
 * seeing RCC_CONFIG_MEMSIZE return earlier does not shorten that settle window.
 *
 * The live GOP scanout dies with the reset; the display stays dark until the
 * DCN block performs a real modeset (a follow-up milestone). This reset is
 * therefore gated by AMDGPU_MODE1_RESET_ON_INIT so a normal boot is unaffected.
 */

/* MP0 bootloader status, same values amdgpu_psp.c uses (Linux mp_14_0_2_offset.h):
 * C2PMSG_35 bit 31 = bootloader ready. Duplicated here rather than exported so
 * the reset path has no link-order dependency on the PSP module. */
#define MODE1_MP0_C2PMSG_35   0x0063
#define MODE1_PSP_BL_READY    0x80000000UL

#include <aros/debug.h>
#include <proto/exec.h>
#include <proto/oop.h>
#include <hidd/pci.h>
#include <hardware/pci.h>   /* PCICS_COMMAND, PCICMF_BUSMASTER/MEMDECODE */

#include "amdgpu_intern.h"
#include "asic_reg/discovery_excerpt.h"   /* AMDGPU_mmRCC_CONFIG_MEMSIZE */

#define DEBUGSMC_MSG_Mode1Reset   2

/* MP1 C2PMSG_N dword offset (== 0x40 + N) off the resolved MP1 base. These three
 * map to mp_14_0_2 regMP1_SMN_C2PMSG_53/75/54 (0x0075/0x008b/0x0076). */
#define MP1_C2PMSG_53   (0x40 + 53)   /* debug param  */
#define MP1_C2PMSG_54   (0x40 + 54)   /* debug resp   */
#define MP1_C2PMSG_75   (0x40 + 75)   /* debug msg    */

/* Crude busy-delay — no timer.device exists at driver init (mirrors psp_spin).
 * Volatile so the loop is not optimised away. psp_spin(2000000) ~= mdelay(20),
 * i.e. ~100000 loops/ms; used only to pace the reset window. */
static void reset_spin(ULONG loops)
{
    volatile ULONG i = 0;
    while (i < loops)
        i++;
}

/* Fire a MODE1 reset and wait for the ASIC to return. Returns TRUE once
 * RCC_CONFIG_MEMSIZE reads a sane value again, FALSE on timeout / no MP1. */
/* PCI config-space word access, straight through the Hidd_PCIDevice methods so
 * the reset path needs no attribute base of its own. */
static UWORD reset_cfg_r16(OOP_Object *dev, UWORD reg)
{
    struct pHidd_PCIDevice_ReadConfigWord m = {
        .mID = OOP_GetMethodID(IID_Hidd_PCIDevice,
                               moHidd_PCIDevice_ReadConfigWord),
        .reg = reg };
    return (UWORD)OOP_DoMethod(dev, (OOP_Msg)&m);
}

static UBYTE reset_cfg_r8(OOP_Object *dev, UWORD reg)
{
    struct pHidd_PCIDevice_ReadConfigByte m = {
        .mID = OOP_GetMethodID(IID_Hidd_PCIDevice,
                               moHidd_PCIDevice_ReadConfigByte),
        .reg = reg };
    return (UBYTE)OOP_DoMethod(dev, (OOP_Msg)&m);
}

static void reset_cfg_w16(OOP_Object *dev, UWORD reg, UWORD val)
{
    struct pHidd_PCIDevice_WriteConfigWord m = {
        .mID = OOP_GetMethodID(IID_Hidd_PCIDevice,
                               moHidd_PCIDevice_WriteConfigWord),
        .reg = reg, .val = val };
    OOP_DoMethod(dev, (OOP_Msg)&m);
}

BOOL amdgpu_mode1_reset(struct AmdgpuDevice *ad)
{
    ULONG base, memsize = 0xffffffff, t;
    UWORD cmd_saved = 0;
    BOOL cfg_saved = FALSE;

    if (!ad || !ad->rmmio || !ad->ip.smu.valid || !ad->ip.smu.base) {
        bug("[amdgpu-reset] no MP1/SMU block (valid=%d base=0x%x) — cannot mode1-reset\n",
            ad ? (int)ad->ip.smu.valid : -1,
            ad ? (unsigned)ad->ip.smu.base : 0);
        return FALSE;
    }

    base = amdgpu_smu_base(ad);
    bug("[amdgpu-reset] MODE1 via SMU debug mailbox: C2PMSG_75<-%u @MP1 base=0x%05x\n",
        (unsigned)DEBUGSMC_MSG_Mode1Reset, (unsigned)base);

    /* CACHE PCI STATE + DROP BUS MASTER, matching amdgpu_device_mode1_reset():
     *   amdgpu_device_cache_pci_state(pdev);
     *   pci_clear_master(pdev);
     * and restored after the reset with amdgpu_device_load_pci_state(pdev).
     * We save/restore the COMMAND word rather than the whole config space: the
     * BARs demonstrably survive this warm reset (MMIO works immediately after,
     * RCC_CONFIG_MEMSIZE reads back), so COMMAND (memory decode + bus master) is
     * the part that actually needs restoring. Leaving bus master ASSERTED across
     * the reset lets in-flight DMA race the ASIC going down; clearing it without
     * restoring afterwards leaves the device unable to DMA at all, which is why
     * the earlier pci_clear_master-only experiment (d559db8296) was not a fair
     * test of this step. */
    if (ad->pciDevice) {
        cmd_saved = reset_cfg_r16(ad->pciDevice, PCICS_COMMAND);
        cfg_saved = TRUE;
        reset_cfg_w16(ad->pciDevice, PCICS_COMMAND,
                      (UWORD)(cmd_saved & ~(UWORD)PCICMF_BUSMASTER));
        bug("[amdgpu-reset] PCI COMMAND 0x%04lx -> 0x%04lx (bus master cleared "
            "for the reset)\n", (unsigned long)cmd_saved,
            (unsigned long)reset_cfg_r16(ad->pciDevice, PCICS_COMMAND));
    } else {
        bug("[amdgpu-reset] no PCI device object — cannot quiesce bus master\n");
    }

    /* smu_msg_v1_send_debug_msg: param, msg, then clear resp. Fire-and-forget —
     * no response poll (the ASIC resets underneath us). */
    amdgpu_wreg32(ad, base + MP1_C2PMSG_53, 0);
    amdgpu_wreg32(ad, base + MP1_C2PMSG_75, DEBUGSMC_MSG_Mode1Reset);
    amdgpu_wreg32(ad, base + MP1_C2PMSG_54, 0);

    /* Warm reset in flight. Match smu_v14_0_2_mode1_reset's fixed 1000 ms
     * no-hardware-access window before any MMIO, then verify RCC below. */
    reset_spin(100000000);

    /* RESTORE PCI STATE before any further MMIO/DMA — Linux's
     * amdgpu_device_load_pci_state() sits here, between the reset and the
     * bootloader wait. */
    if (cfg_saved) {
        reset_cfg_w16(ad->pciDevice, PCICS_COMMAND, cmd_saved);
        bug("[amdgpu-reset] PCI COMMAND restored -> 0x%04lx (MEM=%lu MASTER=%lu)\n",
            (unsigned long)reset_cfg_r16(ad->pciDevice, PCICS_COMMAND),
            (unsigned long)((reset_cfg_r16(ad->pciDevice, PCICS_COMMAND)
                             & PCICMF_MEMDECODE) ? 1 : 0),
            (unsigned long)((reset_cfg_r16(ad->pciDevice, PCICS_COMMAND)
                             & PCICMF_BUSMASTER) ? 1 : 0));
    }

    /* WAIT FOR THE PSP BOOTLOADER before touching anything else.
     *
     * amdgpu_device_mode1_reset() calls amdgpu_psp_wait_for_bootloader()
     * immediately after the reset and BEFORE its memsize poll;
     * psp_v14_0_wait_for_bootloader() polls MP0 C2PMSG_35 for bit 31
     * (0x80000000), up to 10 rounds. We were skipping it entirely and going
     * straight to the memsize check, i.e. proceeding while the PSP bootloader
     * was still re-initialising the ASIC.
     *
     * This is the concrete lead from the Linux register oracle: on a WORKING
     * card GFX_IMU_GFX_RESET_CTRL walks 0x00 -> 0x30 -> 0x7F, and the 0x30 ->
     * 0x7F step lands 3.2s BEFORE soc24_common_hw_init — i.e. during the reset
     * sequence, not during any IP hw_init, and not during PSP firmware load.
     * AROS stops at 0x30 (GRBM_RESETB|DFLL_SRESETB released) and never releases
     * HARD/EA/UTCL2/SDMA/SE_EA — every memory-path block. Completing the
     * bootloader handshake is the step we omit inside that exact window. */
    if (ad->ip.psp.valid && ad->ip.psp.base) {
        ULONG bl = 0, round;
        BOOL ready = FALSE;

        for (round = 0; round < 10 && !ready; round++) {
            for (t = 0; t < 5000; t++) {
                bl = amdgpu_rreg32(ad, ad->ip.psp.base + MODE1_MP0_C2PMSG_35);
                if ((bl & MODE1_PSP_BL_READY) == MODE1_PSP_BL_READY) {
                    ready = TRUE;
                    break;
                }
                reset_spin(200000);   /* ~2ms between polls */
            }
        }
        bug("[amdgpu-reset] PSP bootloader %s — C2PMSG_35=0x%08lx (want bit31) "
            "after %lu rounds\n",
            ready ? "READY" : "NOT READY (proceeding anyway)",
            (unsigned long)bl, (unsigned long)round);
    } else {
        bug("[amdgpu-reset] no MP0/PSP base — cannot wait for the bootloader\n");
    }

    for (t = 0; t < 5000; t++) {
        memsize = amdgpu_rreg32(ad, AMDGPU_mmRCC_CONFIG_MEMSIZE);
        if (memsize != 0xffffffff && memsize != 0)
            break;
        reset_spin(200000);   /* ~2ms between polls */
    }

    if (memsize == 0xffffffff || memsize == 0) {
        bug("[amdgpu-reset] MODE1 TIMEOUT — ASIC did not return (memsize=0x%08lx)\n",
            (unsigned long)memsize);
        return FALSE;
    }

    bug("[amdgpu-reset] MODE1 complete — ASIC back, RCC_CONFIG_MEMSIZE=0x%08lx (%lu MB)\n",
        (unsigned long)memsize, (unsigned long)memsize);
    return TRUE;
}

/*
 * PCI power cycle D0 -> D3hot -> D0.
 *
 * Linux only ever reaches a healthy GC reset release on this part across
 * rmmod -> modprobe, i.e. across a real power-state transition followed by a
 * full init. The GOP hands our card over already in D0, so AROS has never made
 * the card go through D3 before initialising it. See the rationale block at the
 * call site in amdgpu_device.c.
 *
 * Uses the PCI PM capability (ID 0x01) PMCSR PowerState field directly. Timings
 * follow the PCI PM spec: >=10ms settle in D3hot, >=10ms recovery on return to
 * D0 (both generously over-spent here since this runs once at init).
 */
BOOL amdgpu_pci_power_cycle(struct AmdgpuDevice *ad)
{
    OOP_Object *dev;
    UWORD sta, pmcsr, pmc;
    UBYTE cap;
    int guard = 0;

    if (!ad || !ad->pciDevice)
        return FALSE;
    dev = ad->pciDevice;

    sta = reset_cfg_r16(dev, PCICS_STATUS);
    if (!(sta & PCISTF_CAPABILITIES)) {
        bug("[amdgpu-reset] no PCI capability list — cannot power cycle\n");
        return FALSE;
    }

    cap = (UBYTE)(reset_cfg_r8(dev, PCICS_CAP_PTR) & 0xFC);
    while (cap && guard++ < 48) {
        if (reset_cfg_r8(dev, cap) == PCICAP_POWER_MANAGEMENT) {
            pmc   = reset_cfg_r16(dev, cap + 0x02);
            pmcsr = reset_cfg_r16(dev, cap + 0x04);
            bug("[amdgpu-reset] PM cap@0x%02x PMC=0x%04x PMCSR=0x%04x "
                "(state D%u, NO_SOFT_RESET=%u) — power cycling\n",
                (unsigned)cap, (unsigned)pmc, (unsigned)pmcsr,
                (unsigned)(pmcsr & 3), (unsigned)((pmcsr >> 3) & 1));

            /* D0 -> D3hot */
            reset_cfg_w16(dev, cap + 0x04, (UWORD)((pmcsr & ~3u) | 3u));
            reset_spin(30000000);
            bug("[amdgpu-reset]   in D3hot, PMCSR=0x%04x\n",
                (unsigned)reset_cfg_r16(dev, cap + 0x04));

            /* D3hot -> D0. Config space other than PMCSR is preserved across
             * D3hot (unlike D3cold), so no BAR restore is needed here. */
            pmcsr = reset_cfg_r16(dev, cap + 0x04);
            reset_cfg_w16(dev, cap + 0x04, (UWORD)(pmcsr & ~3u));
            reset_spin(60000000);
            bug("[amdgpu-reset]   back in D0, PMCSR=0x%04x\n",
                (unsigned)reset_cfg_r16(dev, cap + 0x04));
            return TRUE;
        }
        cap = (UBYTE)(reset_cfg_r8(dev, cap + 1) & 0xFC);
    }

    bug("[amdgpu-reset] no PCI PM capability found — cannot power cycle\n");
    return FALSE;
}
