/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

 * amdgpu.hidd — device identification and the IP-block init ladder.
 *
 * The PCI-ID table is deliberately small and honest: `tested` means
 * validated on this lab's silicon, nothing else. Growing the table is
 * a diff against upstream amdgpu's pciidlist — the keep-current
 * recipe from ACD-amdgpu-driver-stack-split.
 */

#define DEBUG 1
#include <aros/debug.h>
#include <proto/exec.h>
#include <proto/bootloader.h>
#include <aros/bootloader.h>
#include <string.h>

#include "amdgpu_intern.h"
#include "asic_reg/discovery_excerpt.h"
#define AMDGPU_PCIIDS_TABLES
#include "amdgpu_pciids.h"
#include "amdgpu_mmhub.h"
#define AMDGPU_LINUX_IPMAP_TABLE
#include "amdgpu_linux_ipmap.h"

static const struct AmdgpuPciId amdgpu_ids[] = {
    /* GFX10.3 — RDNA2, Navi 21: the lab's Radeon Pro V620 (headless)
       is the tier-1/2 validation card. */
    { 0x73A1, AMDGPU_GFX10_3, FALSE, "Radeon Pro V620 (Navi 21)" },
    { 0x73AE, AMDGPU_GFX10_3, FALSE, "Radeon Pro V620 MxGPU (Navi 21)" },
    { 0x73BF, AMDGPU_GFX10_3, FALSE, "RX 6800/6900 (Navi 21)" },
    { 0x73DF, AMDGPU_GFX10_3, FALSE, "RX 6700 XT (Navi 22)" },
    { 0x73FF, AMDGPU_GFX10_3, FALSE, "RX 6600 (Navi 23)" },

    /* GFX12 — RDNA4, Navi 44/48: the DUT's RX 9060 XT. VRAM readout
       needs the IP-discovery table (NBIO base differs) — tier 1
       identifies it and stops honestly. */
    { 0x7590, AMDGPU_GFX12, FALSE, "RX 9060 XT (Navi 44)" },   /* DUT card: 1002:7590 rev c0, confirmed on serial */
    { 0x7D68, AMDGPU_GFX12, FALSE, "RX 9060 XT (Navi 44)" },
    { 0x7D60, AMDGPU_GFX12, FALSE, "RX 9070 (Navi 48)" },
    { 0x7D67, AMDGPU_GFX12, FALSE, "RX 9070 GRE (Navi 48)" },

    /* GFX11 — RDNA3 (no lab silicon; enumeration only) */
    { 0x744C, AMDGPU_GFX11, FALSE, "RX 7900 (Navi 31)" },
    { 0x747E, AMDGPU_GFX11, FALSE, "RX 7800/7700 (Navi 32)" },
    { 0x7480, AMDGPU_GFX11, FALSE, "RX 7600 (Navi 33)" },

    /* GFX9 — Vega (no lab silicon; enumeration only) */
    { 0x687F, AMDGPU_GFX9, FALSE, "Vega 10" },

    /* GFX8 — Polaris: the decision-record floor. Lab RX 480s exist
       but are UNTESTED and possibly delayed (terminills 2026-07-18);
       DCE 11.2 display + no-PSP firmware path when they arrive. */
    { 0x67DF, AMDGPU_GFX8, FALSE, "RX 470/480/570/580 (Polaris 10)" },
    { 0x67FF, AMDGPU_GFX8, FALSE, "RX 550/560 (Polaris 11)" },

    { 0, 0, FALSE, NULL }
};

const struct AmdgpuPciId *amdgpu_identify(UWORD vendorid, UWORD deviceid)
{
    const struct AmdgpuPciId *id;

    if (vendorid != 0x1002)
        return NULL;
    for (id = amdgpu_ids; id->deviceid; id++)
        if (id->deviceid == deviceid)
            return id;
    return NULL;
}

const char *amdgpu_gfx_level_name(UBYTE level)
{
    switch (level) {
    case AMDGPU_GFX6:    return "GFX6 (GCN1/Southern Islands)";
    case AMDGPU_GFX7:    return "GFX7 (GCN2/Sea Islands)";
    case AMDGPU_GFX8:    return "GFX8 (GCN3-4/Volcanic Islands)";
    case AMDGPU_GFX9:    return "GFX9 (Vega)";
    case AMDGPU_GFX10_1: return "GFX10.1 (RDNA1)";
    case AMDGPU_GFX10_3: return "GFX10.3 (RDNA2)";
    case AMDGPU_GFX11:   return "GFX11 (RDNA3)";
    case AMDGPU_GFX12:   return "GFX12 (RDNA4)";
    default:             return "unknown";
    }
}

void amdgpu_identify_chip(struct AmdgpuDevice *ad)
{
    const struct AmdgpuPciIdEntry *e;

    ad->chip = AMDGPU_CHIP_IP_DISCOVERY;
    ad->chip_flags = 0;
    for (e = amdgpu_pciids; e->deviceid; e++) {
        if (e->deviceid == ad->deviceid) {
            ad->chip = e->chip;
            ad->chip_flags = e->flags;
            break;
        }
    }
    ad->family = amdgpu_chip_info[ad->chip].family;
    if (ad->chip != AMDGPU_CHIP_IP_DISCOVERY) {
        ad->gfx_level = amdgpu_chip_info[ad->chip].gfx_level;
        if (!ad->name) {
            const char *c = amdgpu_chip_info[ad->chip].name;
            ULONG i = 0;
            const char *p = "AMD GPU (";
            while (*p && i < sizeof(ad->namebuf) - 2) ad->namebuf[i++] = *p++;
            while (*c && i < sizeof(ad->namebuf) - 2) ad->namebuf[i++] = *c++;
            ad->namebuf[i++] = ')';
            ad->namebuf[i] = '\0';
            ad->name = ad->namebuf;
        }
    }
    D(bug("[amdgpu] %04lx: chip %s, family %s, %s%s\n",
          (unsigned long)ad->deviceid, amdgpu_chip_info[ad->chip].name, ad->family,
          amdgpu_gfx_level_name(ad->gfx_level),
          (ad->chip_flags & AMDGPU_PCIID_APU) ? ", APU" : ""));
}

/* GFX level of a chip found through IP discovery, from its GC version. */
static UBYTE gfx_level_from_gc(const struct AmdgpuIpBase *gc)
{
    if (!gc->valid)
        return AMDGPU_GFX_UNKNOWN;
    switch (gc->major) {
    case 9:  return AMDGPU_GFX9;
    case 10: return (gc->minor >= 3) ? AMDGPU_GFX10_3 : AMDGPU_GFX10_1;
    case 11: return AMDGPU_GFX11;
    case 12: return AMDGPU_GFX12;
    default: return AMDGPU_GFX_UNKNOWN;
    }
}

/* ------------------------------------------------------------------ */
/* The IP-block ladder. Mirrors the kernel's amdgpu_device_ip_init():
 * each hardware IP initialises in a fixed order, and each tier of the
 * clean bring-up implements more of them. A NULL hw_init is an honest
 * "not yet" — logged, never faked. */

static BOOL ip_common_init(struct AmdgpuDevice *ad)
{
    /* Generations with a fixed NBIO base (GFX10.x) read VRAM size
       directly. GFX11/12 need the discovery table to locate NBIO —
       so we fetch discovery FIRST using a bootstrap: the memsize
       register lives at a discovery-independent offset early, but
       the table itself sits at top-of-VRAM which needs the size. The
       kernel breaks this cycle by reading mmRCC_CONFIG_MEMSIZE at a
       fixed absolute dword (0xde3) that is valid before discovery. */
    ULONG boot_mb = amdgpu_rreg32(ad, AMDGPU_mmRCC_CONFIG_MEMSIZE);

    if (ad->vram_bar_phys && !ad->vram_bar_size && boot_mb) {
        UQUAD sz = 1;

        while (sz < ((UQUAD)boot_mb << 20))
            sz <<= 1;
        ad->vram_bar_size = (IPTR)sz;
        bug("[amdgpu] %s: BAR0 size from VRAM size: %lu MB\n", ad->name,
            (unsigned long)(sz >> 20));
    }

    if (boot_mb && amdgpu_discovery_init(ad, boot_mb)) {
        UBYTE level = gfx_level_from_gc(&ad->ip.gc);

        D(bug("[amdgpu] %s: IP discovery parsed\n", ad->name));
        if (level != AMDGPU_GFX_UNKNOWN && level != ad->gfx_level) {
            D(bug("[amdgpu] %s: GC %u.%u.%u -> %s\n", ad->name,
                  ad->ip.gc.major, ad->ip.gc.minor, ad->ip.gc.revision,
                  amdgpu_gfx_level_name(level)));
            ad->gfx_level = level;
        }
    }

    /* nv_need_reset_on_init(): a dGPU whose PSP OS is already running (a warm
     * reboot, or a previous driver instance) is mode1-reset before init, so
     * the PSP bootloader can load our firmware from scratch. */
    if (ad->ip.psp.valid && ad->ip.psp.major == 11 &&
        !(ad->chip_flags & AMDGPU_PCIID_APU) &&
        amdgpu_rreg32(ad, ad->ip.psp.base + 0x0091 /* MP0 C2PMSG_81 */) != 0) {
        BOOL reset;

        /* nv_asic_reset_method(): BACO when the SMU can do it, else mode1 */
        bug("[amdgpu] %s: PSP OS already running — resetting before init\n", ad->name);
        reset = amdgpu_baco_reset(ad);
        if (!reset)
            reset = amdgpu_mode1_reset(ad);
        /* The reset takes the chip back to its power-on state: post it again,
           as amdgpu_device_init() does after amdgpu_asic_reset() */
        if (reset && !amdgpu_atom_asic_init(ad))
            bug("[amdgpu] %s: VBIOS ASIC_Init after reset did not run\n", ad->name);
    }

    ad->vram_mb = amdgpu_get_vram_mb(ad);
    if (!ad->vram_mb)
        ad->vram_mb = boot_mb;   /* discovery-independent fallback */

    D(bug("[amdgpu] %s: %s, VRAM %lu MB%s\n", ad->name,
          amdgpu_gfx_level_name(ad->gfx_level), (unsigned long)ad->vram_mb,
          ad->vram_mb ? "" : " (VRAM size unreadable)"));

#if AMDGPU_MODE1_RESET_ON_INIT
    /* Reset the GOP-initialised ASIC to a clean state BEFORE the rest of the IP
     * chain (gmc/psp/gfx/dcn) re-inits, so the PSP's GFX-load path and the GFX
     * block itself come up as Linux expects (not write-blocked). Discovery is
     * already parsed above and mode1 preserves PCI config + IP bases, so no
     * re-map / re-discovery is needed. The live scanout dies here until DCN
     * re-modesets. Non-fatal: a failed reset falls through to the old adopt path. */
    if (ad->ip.smu.valid)
        amdgpu_mode1_reset(ad);
#endif
    return TRUE;
}

/* ---- which IP versions each block implements ----
 *
 * Linux amdgpu drives every GPU from Southern Islands on by picking, per IP
 * block, the implementation for that block's version. This driver does the
 * same, and a block whose version has no implementation here is skipped
 * rather than run with another generation's register map. The log line
 * names the Linux file that drives it, so a report from any card says what
 * is missing. GPUs before Navi 1x have no IP discovery table; Linux uses
 * fixed per-family register maps for them, which this driver does not have
 * yet, so every hardware block is skipped there. */

const char *amdgpu_linux_ip_impl(UBYTE block, const struct AmdgpuIpBase *ip)
{
    const struct AmdgpuLinuxIp *e;

    if (!ip || !ip->valid)
        return NULL;
    for (e = amdgpu_linux_ipmap; e->name; e++)
        if (e->block == block && e->major == ip->major &&
            e->minor == ip->minor && e->revision == ip->revision)
            return e->name;
    return NULL;
}

/* A block runs when this driver has the Linux implementation its version
   maps to (the names in `have`, NULL-terminated). Linux keys GMC and MES off
   the GC version and the rest off their own, so `key` may differ from `ip`. */
static BOOL gate_block(struct AmdgpuDevice *ad, const char *block,
                       const struct AmdgpuIpBase *ip, UBYTE linux_block,
                       const struct AmdgpuIpBase *key, const char * const *have)
{
    const char *impl;

    if (!ip->valid) {
        bug("[amdgpu] %s: %s not found by IP discovery (family %s: Linux uses "
            "fixed per-family register maps there) — skipped\n", ad->name, block,
            ad->family ? ad->family : "?");
        return FALSE;
    }
    impl = amdgpu_linux_ip_impl(linux_block, key ? key : ip);
    for (; impl && *have; have++)
        if (!strcmp(impl, *have))
            return TRUE;
    bug("[amdgpu] %s: %s %u.%u.%u not implemented yet (Linux: %s) — skipped\n",
        ad->name, block, ip->major, ip->minor, ip->revision,
        impl ? impl : "no match in amdgpu_discovery.c");
    return FALSE;
}

static BOOL nbio_supported(struct AmdgpuDevice *ad)
{
    static const char * const have[] = { "nbio_v2_3", "nbif_v6_3_1", NULL };
    return gate_block(ad, "NBIO", &ad->ip.nbio, AMDGPU_LINUX_NBIO, NULL, have);
}

static BOOL gmc_supported(struct AmdgpuDevice *ad)
{
    if (amdgpu_mmhub_regs(ad))
        return TRUE;
    if (!ad->ip.mmhub.valid) {
        bug("[amdgpu] %s: MMHUB not found by IP discovery (family %s) — skipped\n",
            ad->name, ad->family ? ad->family : "?");
        return FALSE;
    }
    bug("[amdgpu] %s: MMHUB %u.%u.%u has no register map yet (Linux: %s) — skipped\n",
        ad->name, ad->ip.mmhub.major, ad->ip.mmhub.minor, ad->ip.mmhub.revision,
        amdgpu_linux_ip_impl(AMDGPU_LINUX_GMC, &ad->ip.gc) ?
            amdgpu_linux_ip_impl(AMDGPU_LINUX_GMC, &ad->ip.gc) : "?");
    return FALSE;
}

static BOOL psp_supported(struct AmdgpuDevice *ad)
{
    static const char * const have[] = { "psp_v11_0", "psp_v14_0", NULL };
    return gate_block(ad, "PSP (MP0)", &ad->ip.psp, AMDGPU_LINUX_PSP, NULL, have);
}

static BOOL smu_supported(struct AmdgpuDevice *ad)
{
    static const char * const have[] = { "smu_v14_0", NULL };
    return gate_block(ad, "SMU (MP1)", &ad->ip.smu, AMDGPU_LINUX_SMU, NULL, have);
}

static BOOL gfx_supported(struct AmdgpuDevice *ad)
{
    static const char * const have[] = { "gfx_v12_0", NULL };
    return gate_block(ad, "GC", &ad->ip.gc, AMDGPU_LINUX_GC, NULL, have);
}

static BOOL sdma_supported(struct AmdgpuDevice *ad)
{
    static const char * const have[] = { "sdma_v7_0", NULL };
    return gate_block(ad, "SDMA", &ad->ip.sdma0, AMDGPU_LINUX_SDMA, NULL, have);
}

static BOOL vcn_supported(struct AmdgpuDevice *ad)
{
    const struct AmdgpuIpBase *ip = &ad->ip.vcn;

    if (ip->valid && ip->major == 5 && ip->minor == 0)
        return TRUE;
    if (!ip->valid)
        bug("[amdgpu] %s: VCN not found by IP discovery - skipped\n", ad->name);
    else
        bug("[amdgpu] %s: VCN %u.%u.%u not implemented yet (Linux: vcn_v%u_%u) - skipped\n",
            ad->name, ip->major, ip->minor, ip->revision, ip->major, ip->minor);
    return FALSE;
}

static BOOL dcn_supported(struct AmdgpuDevice *ad)
{
    const struct AmdgpuIpBase *ip = &ad->ip.dmu;

    if (ip->valid && ip->major == 4)
        return TRUE;
    if (!ip->valid)
        bug("[amdgpu] %s: DCN not found by IP discovery (family %s) — skipped\n",
            ad->name, ad->family ? ad->family : "?");
    else
        bug("[amdgpu] %s: DCN %u.%u.%u not implemented yet (Linux: amdgpu_dm, "
            "DC resource for DCN %u.%u.%u) — skipped\n", ad->name,
            ip->major, ip->minor, ip->revision, ip->major, ip->minor, ip->revision);
    return FALSE;
}

static const struct AmdgpuIpBlock amdgpu_ip_blocks[] = {
    { "common",  ip_common_init,      NULL },
    { "nbio",    amdgpu_nbio_hw_init, nbio_supported }, /* Linux soc24 common register init */
    { "gmc",     amdgpu_gmc_hw_init,  gmc_supported },  /* memory controller — observe aperture map */
    { "ih",      NULL,                NULL },           /* interrupt handler — CS tier */
    { "psp",     amdgpu_psp_hw_init,  psp_supported },  /* firmware security processor — the gateway */
    { "smu",     amdgpu_smu_hw_init,  smu_supported },  /* MP1 mailbox — display/pixel clock (Stage 3 P0) */
    { "gfx",     amdgpu_gfx_hw_init,  gfx_supported },  /* GFX12 engine — IMU power-up (P0), CP rings next */
    { "sdma",    amdgpu_sdma_hw_init, sdma_supported }, /* copy engine — the 2D blit path */
    { "vcn",     amdgpu_vcn_hw_init,  vcn_supported },  /* video codec engine */
    { "dcn",     amdgpu_dcn_hw_init,  dcn_supported },  /* display — observe the live scanout */
    { NULL, NULL, NULL }
};

BOOL amdgpu_device_init(struct AmdgpuBase *base, struct AmdgpuDevice *ad)
{
    const struct AmdgpuIpBlock *ip;

    if (!amdgpu_mmio_map(base, ad)) {
        D(bug("[amdgpu] %s: register BAR mapping failed\n", ad->name));
        return FALSE;
    }

    /* VBIOS ASIC_Init runs BEFORE the IP ladder, mirroring Linux:
     * amdgpu_device_init() calls amdgpu_device_asic_init() (amdgpu_device.c:3979)
     * ahead of amdgpu_device_ip_init(), and the Linux register oracle timestamps
     * the healthy GFX_IMU_GFX_RESET_CTRL 0x30 -> 0x7F release 3.2s BEFORE
     * soc24_common_hw_init — i.e. exactly here, not inside any IP block.
     * Running it LATE (from C:GfxResetProbe, after full init) executed cleanly
     * and returned 0 but did NOT move the register, so position is the variable
     * still under test. Non-fatal either way: a card that cannot run the table
     * still boots the display path. */
    /* PCI power cycle D0 -> D3hot -> D0 BEFORE anything else touches the card.
     *
     * Every Linux capture of the healthy GFX_IMU_GFX_RESET_CTRL 0x00 -> 0x30 ->
     * 0x7F walk was taken across rmmod -> modprobe. Measured on the DUT: after
     * rmmod every GC register reads 0xFFFFFFFF, i.e. the card is POWERED DOWN in
     * D3; modprobe then returns it to D0 and runs a full init on freshly-powered
     * silicon. Our card never sees that — the GOP hands it over already in D0.
     *
     * Probing the transition alone (C:GfxResetProbe d3d0, on an already
     * initialised card) DID restart the IMU: GFX_IMU_SCRATCH_1 was caught
     * transiently at 0xB000B000 before settling back to 0xBABEFACE, and PMCSR
     * walked 0x0000 -> 0x0003 -> 0x0000. But RESET_CTRL stayed 0x30, because no
     * firmware reload followed. Doing it HERE is the faithful ordering: power
     * cycle first, then the whole PSP/IP init runs against freshly-powered
     * silicon exactly as Linux's modprobe does.
     *
     * Note PMCSR read 0x0000 with NO_SOFT_RESET=0 on this boot, so Linux's
     * pci_pm_reset() eligibility WOULD accept this transition as a reset —
     * unlike the 0x0008/NO_SOFT_RESET=1 codex measured on 2026-08-15. */
    if (!amdgpu_pci_power_cycle(ad))
        bug("[amdgpu] %s: D3hot/D0 power cycle did not run\n", ad->name);

    if (!amdgpu_atom_asic_init(ad))
        bug("[amdgpu] %s: VBIOS ASIC_Init did not run\n", ad->name);

    for (ip = amdgpu_ip_blocks; ip->name; ip++) {
        if (!ip->hw_init) {
            D(bug("[amdgpu] %s: ip block '%s' not implemented (planned)\n",
                  ad->name, ip->name));
            continue;
        }
        if (ip->supported && !ip->supported(ad))
            continue;
        amdgpu_gfx_state_snapshot(ad, ip->name);
        bug("[amdgpu] ip block '%s' -> enter\n", ip->name);
        if (!ip->hw_init(ad)) {
            D(bug("[amdgpu] %s: ip block '%s' FAILED\n", ad->name, ip->name));
            return FALSE;
        }
        bug("[amdgpu] ip block '%s' -> ok\n", ip->name);
    }
    bug("[amdgpu] IP ladder complete\n");
    /* Late init, as Linux amdgpu_device_ip_late_init: clock gating goes on only
     * after every block is up. */
    if (!amdgpu_boot_option("nocg"))
        amdgpu_gfx_set_clock_gating(ad, TRUE);
    else
        bug("[amdgpu] GFX clock gating off (AMDGPU=nocg)\n");
    return TRUE;
}

BOOL amdgpu_boot_option(const char *word)
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
            strstr(node->ln_Name + 7, word))
            return TRUE;
    }
    return FALSE;
}
