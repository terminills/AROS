/*
 * amdgpu_atom.c — run the VBIOS ATOM ASIC_Init command table.
 *
 * WHY THIS EXISTS (JOB-0084)
 * --------------------------
 * After the LOAD_TOC fix (ae86fcc50f) every PSP firmware load succeeds and the
 * IMU firmware runs (GFX_IMU_SCRATCH_1 = 0xBABEFACE), but the GC memory-path
 * resets stay held forever:
 *     GFX_IMU_GFX_RESET_CTRL = 0x30 = GRBM_RESETB | DFLL_SRESETB
 *     healthy                = 0x7F = + HARD | EA | UTCL2 | SDMA | SE_EA
 * so RLC autoload never completes and GFX never comes up.
 *
 * The Linux register oracle (evidence linux-register-oracle-20260817T043000Z)
 * timestamped the healthy 0x00 -> 0x30 -> 0x7F walk at 3.2s BEFORE
 * soc24_common_hw_init — inside amdgpu_device_init, before ANY ip_hw_init and
 * before the PSP firmware stream. An A/B with the reset disabled entirely
 * proved the GOP already leaves the card at 0x30, so the reset never caused it.
 * The one step Linux takes in that window and we did not is
 * amdgpu_device_asic_init() (amdgpu_device.c:3979), which for
 * GC_HWIP >= IP_VERSION(11,0,0) — ours is 12.0.0 — does:
 *     amdgpu_psp_wait_for_bootloader(adev);          (added in 3be03962ae)
 *     amdgpu_atomfirmware_asic_init(adev, true);     <-- THIS
 * and that ends in amdgpu_atom_execute_table(ctx, ATOM_CMD_INIT = 0, ...).
 * Executing the VBIOS ASIC_Init table is what programs the clocks and memory
 * controller — precisely the blocks still held at 0x30.
 *
 * WHY THIS IS SHORT
 * -----------------
 * The ATOM bytecode interpreter already exists in this tree:
 * atom_bios.c (1176 lines, 161 opcodes, with
 * atom_parse/atom_execute_table/atom_asic_init). It is fully self-contained —
 * it includes only exec/stdint/string and its own header, and never touches
 * radeonsi internals — so amdgpu links the same source rather than duplicating
 * it. Only the card accessors are driver-specific, and they are here.
 *
 * NOTE ON THE "ATOM TABLES ARE STRIPPED" NOTE: that survey
 * (amdgpu_display_survey_atom_commands) asked a MODESET question and correctly
 * found 0/10 display commands present on RDNA4. It never probed command index 0.
 * ASIC_Init IS present on this card — verified on the DUT:
 *     ATOM cmd ASIC_Init [ 0]: PRESENT @0x2664 size=290 frev=2 crev=1
 * which is exactly the revision Linux requires (frev==2, crev>=1).
 */

#include <aros/debug.h>
#include <proto/exec.h>
#include <exec/memory.h>
#include <string.h>

#include "amdgpu_intern.h"
#include "asic_reg/discovery_excerpt.h"   /* AMDGPU_mmMM_INDEX / _DATA */
#include "atom_bios.h"

/* Little-endian VBIOS byte accessors. amdgpu_display.c has equivalents but they
 * are static there; these are local rather than widening that file's surface. */
static UWORD atom_u16(const UBYTE *rom, ULONG off)
{ return (UWORD)(rom[off] | ((UWORD)rom[off + 1] << 8)); }

static ULONG atom_u32(const UBYTE *rom, ULONG off)
{
    return (ULONG)rom[off] | ((ULONG)rom[off + 1] << 8) |
           ((ULONG)rom[off + 2] << 16) | ((ULONG)rom[off + 3] << 24);
}

static BOOL atom_off_ok(ULONG off, ULONG need, ULONG rom_size)
{ return off != 0 && (off + need) <= rom_size; }

/* atom_card_info MUST be first: the interpreter hands the accessors a
 * struct atom_card_info *, and we recover the device by casting back. */
struct AmdgpuAtomCard {
    struct atom_card_info  info;
    struct AmdgpuDevice   *ad;
};

/* ATOM register operands are DWORD indices, same convention as
 * amdgpu_rreg32/amdgpu_wreg32. Registers beyond the direct MMIO aperture go
 * through the MM_INDEX/MM_DATA window (amdgpu_device_mm_access), matching the
 * radeonsi accessors this is modelled on. */
static ULONG atom_reg_read(struct atom_card_info *info, uint32_t reg)
{
    struct AmdgpuAtomCard *c = (struct AmdgpuAtomCard *)info;
    struct AmdgpuDevice *ad = c->ad;

    if (!ad || !ad->rmmio)
        return 0xFFFFFFFFUL;

    if (((IPTR)reg << 2) < ad->rmmio_size)
        return amdgpu_rreg32(ad, reg);

    /* MM_INDEX/MM_DATA aperture — shared with amdgpu_vram_read/write; take the
     * per-device aperture lock across the index+data pair (codex TRIG-0686). */
    {
        ULONG v;
        ObtainSemaphore(&ad->aperture_sem);
        amdgpu_wreg32(ad, AMDGPU_mmMM_INDEX, (ULONG)(reg << 2));
        (void)amdgpu_rreg32(ad, AMDGPU_mmMM_INDEX);
        v = amdgpu_rreg32(ad, AMDGPU_mmMM_DATA);
        ReleaseSemaphore(&ad->aperture_sem);
        return v;
    }
}

static void atom_reg_write(struct atom_card_info *info, uint32_t reg, uint32_t val)
{
    struct AmdgpuAtomCard *c = (struct AmdgpuAtomCard *)info;
    struct AmdgpuDevice *ad = c->ad;

    if (!ad || !ad->rmmio)
        return;

    if (((IPTR)reg << 2) < ad->rmmio_size) {
        amdgpu_wreg32(ad, reg, val);
        return;
    }

    /* MM_INDEX/MM_DATA aperture — same per-device lock (codex TRIG-0686). */
    ObtainSemaphore(&ad->aperture_sem);
    amdgpu_wreg32(ad, AMDGPU_mmMM_INDEX, (ULONG)(reg << 2));
    (void)amdgpu_rreg32(ad, AMDGPU_mmMM_INDEX);
    amdgpu_wreg32(ad, AMDGPU_mmMM_DATA, val);
    ReleaseSemaphore(&ad->aperture_sem);
}

/* MC goes through the same path; PLL is a stub on Navi+ exactly as Linux
 * amdgpu leaves it (no ATOM PLL space on these parts). */
static ULONG atom_mc_read(struct atom_card_info *info, uint32_t reg)
{ return atom_reg_read(info, reg); }
static void atom_mc_write(struct atom_card_info *info, uint32_t reg, uint32_t val)
{ atom_reg_write(info, reg, val); }
static ULONG atom_pll_read(struct atom_card_info *info, uint32_t reg)
{ (void)info; (void)reg; return 0; }
static void atom_pll_write(struct atom_card_info *info, uint32_t reg, uint32_t val)
{ (void)info; (void)reg; (void)val; }

/*
 * Run the VBIOS ASIC_Init command table. Returns TRUE when the table executed;
 * FALSE (non-fatal) when there is no usable VBIOS or the table is absent, so a
 * card without it still boots to the display path.
 */
BOOL amdgpu_atom_asic_init(struct AmdgpuDevice *ad)
{
    struct AmdgpuAtomCard card;
    struct atom_context *ctx;
    ULONG rc_before, rc_after;
    int ret;

    if (!ad || !ad->rmmio)
        return FALSE;

    if (!amdgpu_display_fetch_vbios(ad) || !ad->rom_base || !ad->rom_size) {
        bug("[amdgpu-atom] no VBIOS image — cannot run ASIC_Init\n");
        return FALSE;
    }

    memset(&card, 0, sizeof(card));
    card.ad             = ad;
    card.info.reg_read  = atom_reg_read;
    card.info.reg_write = atom_reg_write;
    card.info.mc_read   = atom_mc_read;
    card.info.mc_write  = atom_mc_write;
    card.info.pll_read  = atom_pll_read;
    card.info.pll_write = atom_pll_write;

    ctx = atom_parse(&card.info, ad->rom_base);
    if (!ctx) {
        bug("[amdgpu-atom] atom_parse failed — VBIOS is not an ATOM image?\n");
        return FALSE;
    }

    /* The GFX_IMU reset register is the whole point of running this: log it
     * either side so a DUT capture shows immediately whether ASIC_Init is what
     * walks it from 0x30 to 0x7F. gc_seg[1] may not be resolved yet if this
     * runs before discovery, so guard the read. */
    rc_before = ad->gc_seg[1]
              ? amdgpu_rreg32(ad, ad->gc_seg[1] + 0x40bc) : 0xFFFFFFFFUL;

    bug("[amdgpu-atom] *** executing VBIOS ASIC_Init (ATOM_CMD_INIT) — "
        "GFX_IMU_GFX_RESET_CTRL before = 0x%08lx ***\n",
        (unsigned long)rc_before);

    /* Build the parameter block the ATOMFIRMWARE way, NOT radeonsi's legacy way.
     *
     * radeonsi's atom_asic_init() sources defSCLK/defMCLK through the ATOM v1
     * data-table offsets, which on this RDNA4 VBIOS read back as zero — it
     * bails with "zero clocks, skipping" (observed on the DUT). RDNA4 carries
     * atomfirmware v2.1 data tables, which is exactly why Linux has a separate
     * amdgpu_atomfirmware_asic_init(). Mirror that instead:
     *   master data table (v2_1): firmwareinfo is entry index 4, and the list
     *     starts after the 4-byte atom_common_table_header.
     *   atom_firmware_info_v3_1: header(4) + firmware_revision(4), so
     *     bootup_sclk_in10khz is at +8 and bootup_mclk_in10khz at +12.
     *   asic_init_ps_allocation_v2_1: engineparam (sclk:24 | engineflag:8),
     *     memparam (mclk:24 | memflag:8), then reserved[16] => 72 bytes.
     *     engineflag = b3NORMAL_ENGINE_INIT = 0; memflag = 0 for fb_reset=true.
     * params_size is in DWORDS for this interpreter (ps_size vs ps_shift=ps/4). */
    {
        const UBYTE *rom = ad->rom_base;
        ULONG fwi, sclk = 0, mclk = 0;
        ULONG ps[32];

        fwi = atom_u16(rom, ctx->data_table + 4 + (4 * 2));
        if (fwi && atom_off_ok(fwi, 16, ad->rom_size)) {
            sclk = atom_u32(rom, fwi + 8);
            mclk = atom_u32(rom, fwi + 12);
        }
        bug("[amdgpu-atom] atomfirmware firmwareinfo @0x%lx: bootup_sclk=%lu0kHz "
            "bootup_mclk=%lu0kHz\n", (unsigned long)fwi,
            (unsigned long)sclk, (unsigned long)mclk);
        if (fwi && atom_off_ok(fwi, 40, ad->rom_size)) {
            ULONG d;
            bug("[amdgpu-atom]   fwinfo hdr: size=%u frev=%u crev=%u\n",
                (unsigned)atom_u16(rom, fwi), (unsigned)rom[fwi + 2],
                (unsigned)rom[fwi + 3]);
            for (d = 0; d < 32; d += 16)
                bug("[amdgpu-atom]   +0x%02lx: %08lx %08lx %08lx %08lx\n",
                    (unsigned long)d,
                    (unsigned long)atom_u32(rom, fwi + d),
                    (unsigned long)atom_u32(rom, fwi + d + 4),
                    (unsigned long)atom_u32(rom, fwi + d + 8),
                    (unsigned long)atom_u32(rom, fwi + d + 12));
        }

        /* DO NOT bail on zero clocks. This VBIOS genuinely reports
         * bootup_sclk = bootup_mclk = 0 (firmware_info v3.5, dumped on the DUT:
         * hdr size=172 frev=3 crev=5, +0x08 and +0x0C both 0x00000000, with
         * bios_scratch_reg_startaddr=0x4c at +0x18 confirming the layout). On
         * RDNA4 the SMU owns clocks, so the field is vestigial.
         * amdgpu_atomfirmware_asic_init() does NOT check for zero — it builds
         * the parameter block from whatever is there and executes. radeonsi's
         * legacy atom_asic_init() DOES bail ("zero clocks, skipping"), which is
         * why reusing it verbatim was wrong here. Match Linux. */
        if (!sclk || !mclk)
            bug("[amdgpu-atom] bootup clocks are zero — proceeding anyway, "
                "which is exactly what amdgpu_atomfirmware_asic_init() does\n");

        memset(ps, 0, sizeof(ps));
        ps[0] = (sclk & 0x00FFFFFFUL);   /* engineflag b3NORMAL_ENGINE_INIT = 0 */
        ps[1] = (mclk & 0x00FFFFFFUL);   /* memflag 0 (fb_reset = true)        */

        ret = atom_execute_table(ctx, ATOM_CMD_INIT, ps, 18);
    }

    rc_after = ad->gc_seg[1]
             ? amdgpu_rreg32(ad, ad->gc_seg[1] + 0x40bc) : 0xFFFFFFFFUL;

    bug("[amdgpu-atom] *** ASIC_Init returned %d — GFX_IMU_GFX_RESET_CTRL "
        "0x%08lx -> 0x%08lx %s ***\n",
        ret, (unsigned long)rc_before, (unsigned long)rc_after,
        (rc_after == 0x7FUL) ? "<<< RELEASED (0x7F) >>>"
                             : (rc_after != rc_before) ? "<-- MOVED" : "(unchanged)");

    atom_destroy(ctx);
    return (ret == 0);
}
