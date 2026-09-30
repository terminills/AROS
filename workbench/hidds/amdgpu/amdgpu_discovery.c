/*
 * amdgpu.hidd — IP discovery table parser.
 *
 * Modern AMD GPUs (GFX9+, mandatory GFX11/12) describe their own
 * register topology in a binary "IP discovery" table the firmware
 * leaves at the top of VRAM. Parsing it replaces per-ASIC hardcoded
 * register-offset tables: one driver body enumerates any generation's
 * IP blocks and their base addresses from data. This is the concrete
 * "keep current" mechanism of ACD-amdgpu-driver-stack-split.
 *
 * Mirrors the kernel's amdgpu_discovery.c. Wire structures are
 * byte-packed little-endian; every multi-byte read goes through the
 * le_*() helpers so host/target endianness is irrelevant. All offsets
 * are bounds-checked against the fetched blob — a malformed table
 * fails cleanly rather than reading past the buffer.
 */

#define DEBUG 1
#include <aros/debug.h>

#include <proto/exec.h>
#include <string.h>

#include "amdgpu_intern.h"
#include "asic_reg/discovery_excerpt.h"

/* ---- little-endian readers over the raw blob (bounds-checked) ---- */
static inline UWORD le16(const UBYTE *b) { return b[0] | (b[1] << 8); }
static inline ULONG le32(const UBYTE *b)
{
    return (ULONG)b[0] | ((ULONG)b[1] << 8) |
           ((ULONG)b[2] << 16) | ((ULONG)b[3] << 24);
}

/* ---- indirect VRAM read via the BIF MM aperture ----
 * Reads `dwords` 32-bit words from VRAM byte-offset `pos` into buf,
 * using MM_INDEX (bit 31 selects the VRAM window) + MM_INDEX_HI for
 * the high address bits + MM_DATA. Matches amdgpu_device_mm_access. */
void amdgpu_vram_read(struct AmdgpuDevice *ad, UQUAD pos,
                      ULONG *buf, ULONG dwords)
{
    ULONG i;
    ULONG hi = 0xFFFFFFFFUL;

    /* pos is a VRAM BYTE address up to the full VRAM size (16 GiB on the DUT's
     * Navi 44), so it MUST be 64-bit: AROS ULONG is 32-bit even on x86_64, and
     * a 32-bit pos truncates any offset >= 4 GiB — exactly the top-of-VRAM
     * discovery blob. MM_INDEX takes the low 32 bits (bit 31 selects the VRAM
     * window); MM_INDEX_HI takes bits [.. :31]. Matches amdgpu_device_mm_access. */
    /* The MM_INDEX/MM_INDEX_HI/MM_DATA triple is one stateful aperture shared by
     * every VRAM/ATOM indexed access; hold it across the WHOLE walk so another
     * CPU cannot retarget MM_INDEX between our index write and data read
     * (codex TRIG-0686). */
    ObtainSemaphore(&ad->aperture_sem);
    for (i = 0; i < dwords; i++, pos += 4) {
        ULONG hipart = (ULONG)(pos >> 31);
        amdgpu_wreg32(ad, AMDGPU_mmMM_INDEX, (ULONG)pos | 0x80000000UL);
        if (hipart != hi) {
            amdgpu_wreg32(ad, AMDGPU_mmMM_INDEX_HI, hipart);
            hi = hipart;
        }
        buf[i] = amdgpu_rreg32(ad, AMDGPU_mmMM_DATA);
    }
    ReleaseSemaphore(&ad->aperture_sem);
}

/* Mirror of amdgpu_vram_read — write `dwords` words to VRAM byte-offset pos via
 * the same MM_INDEX/MM_DATA window. Used to stage firmware into VRAM so the PSP
 * can DMA it by MC address. */
void amdgpu_vram_write(struct AmdgpuDevice *ad, UQUAD pos,
                       const ULONG *buf, ULONG dwords)
{
    ULONG i;
    ULONG hi = 0xFFFFFFFFUL;

    /* Same shared-aperture serialization as amdgpu_vram_read (codex TRIG-0686):
     * a packet-stage vram_write here must not be split by another CPU's MM_INDEX
     * retarget, or the ring receives corrupt dwords (RPTR wedges at 0x04). */
    ObtainSemaphore(&ad->aperture_sem);
    for (i = 0; i < dwords; i++, pos += 4) {
        ULONG hipart = (ULONG)(pos >> 31);
        amdgpu_wreg32(ad, AMDGPU_mmMM_INDEX, (ULONG)pos | 0x80000000UL);
        if (hipart != hi) {
            amdgpu_wreg32(ad, AMDGPU_mmMM_INDEX_HI, hipart);
            hi = hipart;
        }
        amdgpu_wreg32(ad, AMDGPU_mmMM_DATA, buf[i]);
    }
    ReleaseSemaphore(&ad->aperture_sem);
}

static void record_base(struct AmdgpuDevice *ad, UWORD hw_id, UBYTE instance,
                        UBYTE maj, UBYTE min, UBYTE rev, ULONG base0)
{
    struct AmdgpuIpBase *slot = NULL;

    switch (hw_id) {
    case AMDGPU_GC_HWID:    slot = &ad->ip.gc;    break;
    case AMDGPU_NBIF_HWID:  slot = &ad->ip.nbio;  break;
    case AMDGPU_MMHUB_HWID: slot = &ad->ip.mmhub; break;
    case AMDGPU_ATHUB_HWID: slot = &ad->ip.athub; break;
    case AMDGPU_OSSSYS_HWID:slot = &ad->ip.osssys;break;
    case AMDGPU_MP0_HWID:   slot = &ad->ip.psp;   break;
    case AMDGPU_MP1_HWID:   slot = &ad->ip.smu;   break;
    case AMDGPU_SDMA0_HWID: slot = &ad->ip.sdma0; break;
    case AMDGPU_DMU_HWID:   slot = &ad->ip.dmu;   break;
    case AMDGPU_DF_HWID:    slot = &ad->ip.df;    break;
    case AMDGPU_VCN_HWID:   slot = &ad->ip.vcn;   break;
    default: break;
    }
    /* first instance wins for the slots we track (instance 0) */
    if (slot && !slot->valid) {
        slot->base = base0;
        slot->major = maj;
        slot->minor = min;
        slot->revision = rev;
        slot->instance = instance;
        slot->valid = TRUE;
    }
}

/*
 * Parse the discovery blob already fetched into ad->disc_bin (size
 * ad->disc_size). Fills ad->ip.* base addresses. Returns TRUE if the
 * table validated and at least the NBIO/GC blocks were found.
 */
static BOOL amdgpu_discovery_parse(struct AmdgpuDevice *ad)
{
    const UBYTE *bin = ad->disc_bin;
    ULONG total = ad->disc_size;
    ULONG sig, tbl_off, ihdr_sig;
    UWORD num_dies, d;
    BOOL is64;

    if (total < sizeof(struct amdgpu_binary_header))
        return FALSE;

    sig = le32(bin + 0);
    if (sig != AMDGPU_BINARY_SIGNATURE) {
        D(bug("[amdgpu] discovery: bad binary signature 0x%08lx\n",
              (unsigned long)sig));
        return FALSE;
    }

    /* binary_header.table_list[IP_DISCOVERY].offset — table_list starts
       after the 12-byte fixed head (sig4 + 4x u16). */
    {
        ULONG tl = 12 + AMDGPU_IP_DISCOVERY * 8;   /* table_info is 8 bytes */
        if (tl + 8 > total)
            return FALSE;
        tbl_off = le16(bin + tl);                  /* table_info.offset */
    }
    if (tbl_off + 4 + 4 > total)
        return FALSE;

    ihdr_sig = le32(bin + tbl_off);
    if (ihdr_sig != AMDGPU_DISCOVERY_TABLE_SIGNATURE) {
        D(bug("[amdgpu] discovery: bad table signature 0x%08lx\n",
              (unsigned long)ihdr_sig));
        return FALSE;
    }

    /* ip_discovery_header: sig(4) ver(2) size(2) id(4) num_dies(2)@12
       die_info[16] {die_id(2) die_offset(2)}@14  then the 64-bit flag union.
       NOTE die_info[].die_offset is ABSOLUTE from the blob start (matches the
       kernel's `discovery_bin + die_offset`), not relative to tbl_off. */
    num_dies = le16(bin + tbl_off + 12);
    if (num_dies > 16)
        num_dies = 16;

    /* base_addr_64_bit flag: v4 tables set it right after die_info[16], i.e.
       14 (head through num_dies) + 16*4 = 78 bytes into the table. */
    {
        ULONG flag_off = tbl_off + 14 + 16 * 4;
        is64 = (flag_off < total) && (bin[flag_off] & 0x1);
    }

    D(bug("[amdgpu] discovery: %u die(s), %u-bit base addresses\n",
          num_dies, is64 ? 64 : 32));

    for (d = 0; d < num_dies; d++) {
        ULONG di = tbl_off + 14 + d * 4;
        UWORD die_off, num_ips, i;
        ULONG p;

        if (di + 4 > total)
            break;
        die_off = le16(bin + di + 2);              /* die_info.die_offset */
        p = die_off;                                /* ABSOLUTE from blob start */
        if (p + 4 > total)
            continue;

        /* die_header: die_id(2) num_ips(2) */
        num_ips = le16(bin + p + 2);
        p += 4;

        for (i = 0; i < num_ips; i++) {
            UWORD hw_id;
            UBYTE inst, num_base, maj, min, rev;
            ULONG base0 = 0;
            ULONG head = p;

            if (head + 8 > total)
                return TRUE;   /* truncated: keep what we parsed */

            hw_id    = le16(bin + head + 0);
            inst     = bin[head + 2];
            num_base = bin[head + 3];
            maj      = bin[head + 4];
            min      = bin[head + 5];
            rev      = bin[head + 6];
            /* byte 7 = sub_revision:4 | variant:4 (unused here) */

            p = head + 8;
            if (num_base) {
                ULONG stride = is64 ? 8 : 4;
                if (p + stride > total)
                    return TRUE;
                base0 = le32(bin + p);   /* low 32 bits = register base */
                p += (ULONG)num_base * stride;
            }
            /* Dump EVERY IP block with ALL its base segments — a block like DMU
             * (display) has several segments and the DCN scanout registers live in
             * one of the later ones, not base0. This shows whether they fall inside
             * our 512 KB register BAR or need indirect (SMN) access. */
            {
                ULONG st2 = is64 ? 8 : 4;
                ULONG bb[6] = { 0, 0, 0, 0, 0, 0 };
                UBYTE nb2 = num_base > 6 ? 6 : num_base, bi;
                for (bi = 0; bi < nb2; bi++)
                    if (head + 8 + bi * st2 + 4 <= total)
                        bb[bi] = le32(bin + head + 8 + bi * st2);
                D(bug("[amdgpu-disc]   hw_id=%3u inst=%u v%u.%u.%u nb=%u bases: "
                      "%lx %lx %lx %lx %lx %lx\n",
                      hw_id, inst, maj, min, rev, num_base,
                      (unsigned long)bb[0], (unsigned long)bb[1], (unsigned long)bb[2],
                      (unsigned long)bb[3], (unsigned long)bb[4], (unsigned long)bb[5]));
                /* keep the display block's whole segment map for the DCN read */
                if (hw_id == AMDGPU_DMU_HWID) {
                    UBYTE k;
                    for (k = 0; k < 6; k++)
                        ad->dmu_seg[k] = bb[k];
                }
                /* keep the GC block's whole segment map for the GFX IP block —
                   the IMU/CP_MEC/RLC registers are BASE_IDX 1 (gc_seg[1]). */
                if (hw_id == AMDGPU_GC_HWID) {
                    UBYTE k;
                    for (k = 0; k < 6; k++)
                        ad->gc_seg[k] = bb[k];
                }
                /* MMHUB + OSSSYS full segment maps — MES SET_HW_RESOURCES hands the
                   scheduler reg_offset[MMHUB/OSSSYS][0][0..4] to program VM/interrupt. */
                if (hw_id == AMDGPU_MMHUB_HWID) {
                    UBYTE k;
                    for (k = 0; k < 6; k++)
                        ad->mmhub_seg[k] = bb[k];
                }
                if (hw_id == AMDGPU_OSSSYS_HWID) {
                    UBYTE k;
                    for (k = 0; k < 6; k++)
                        ad->osssys_seg[k] = bb[k];
                }
                /* Navi44 NBIO registers use BASE_IDX 5. Preserve the complete
                   segment map so the indirect register path can resolve it. */
                if (hw_id == AMDGPU_NBIF_HWID) {
                    UBYTE k;
                    for (k = 0; k < 6; k++)
                        ad->nbio_seg[k] = bb[k];
                }
            }
            record_base(ad, hw_id, inst, maj, min, rev, base0);
        }
    }
    return ad->ip.nbio.valid || ad->ip.gc.valid;
}

BOOL amdgpu_discovery_init(struct AmdgpuDevice *ad, ULONG vram_mb)
{
    UQUAD pos;
    ULONG dwords;

    if (!ad->rmmio || !vram_mb)
        return FALSE;

    /* blob sits DISCOVERY_TMR_OFFSET below the top of VRAM. Compute the byte
     * address in 64-bit: (vram_mb << 20) is up to ~16 GiB and MUST NOT be done
     * in 32-bit ULONG arithmetic (AROS ULONG is 32-bit) or it wraps and we read
     * the blob from ~4 GiB in, yielding a bogus signature. */
    pos = ((UQUAD)vram_mb << 20) - AMDGPU_DISCOVERY_TMR_OFFSET;
    ad->disc_size = AMDGPU_DISCOVERY_TMR_SIZE;
    dwords = ad->disc_size >> 2;

    ad->disc_bin = AllocMem(ad->disc_size, MEMF_ANY);
    if (!ad->disc_bin)
        return FALSE;

    amdgpu_vram_read(ad, pos, (ULONG *)ad->disc_bin, dwords);

    if (!amdgpu_discovery_parse(ad)) {
        FreeMem(ad->disc_bin, ad->disc_size);
        ad->disc_bin = NULL;
        ad->disc_size = 0;
        return FALSE;
    }

    D(bug("[amdgpu] discovery bases: NBIO 0x%05lx GC 0x%05lx MMHUB 0x%05lx "
          "OSS 0x%05lx PSP 0x%05lx SDMA0 0x%05lx DMU 0x%05lx\n",
          (unsigned long)ad->ip.nbio.base, (unsigned long)ad->ip.gc.base,
          (unsigned long)ad->ip.mmhub.base, (unsigned long)ad->ip.osssys.base,
          (unsigned long)ad->ip.psp.base, (unsigned long)ad->ip.sdma0.base,
          (unsigned long)ad->ip.dmu.base));
    if (ad->ip.gc.valid)
        D(bug("[amdgpu] GC IP version %u.%u.%u\n",
              ad->ip.gc.major, ad->ip.gc.minor, ad->ip.gc.revision));
    if (ad->ip.nbio.valid)
        D(bug("[amdgpu] NBIF IP version %u.%u.%u, segment5=0x%08lx\n",
              ad->ip.nbio.major, ad->ip.nbio.minor, ad->ip.nbio.revision,
              (unsigned long)ad->nbio_seg[5]));
    return TRUE;
}
