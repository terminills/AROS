/* Consume the GMC/PSP map instead of imposing a tiny bitmap-pool quota.
 * No global BAR mapping and no changes to display ownership or GPU state.
 * AI_DECISION_CHAIN: MEMORY-QUERY-STATUS.md
 */
#define DEBUG 1
#include <aros/debug.h>
#include <string.h>
#include "amdgpu_intern.h"
#include "amdgpu_gfx_support.h"
#include "amdgpu_vram.h"

#define MAP_DEVICES 8
#define PAGE 4096ULL
#define MIB (1ULL << 20)
static struct AmdgpuVramMap maps[MAP_DEVICES];

static struct AmdgpuVramMap *find_map(struct AmdgpuDevice *ad)
{
    ULONG i;
    for (i = 0; i < MAP_DEVICES; i++)
        if (maps[i].ad == ad) return &maps[i];
    return NULL;
}

static BOOL reserve(struct AmdgpuVramMap *m, UQUAD start, UQUAD size,
                    const char *name)
{
    UQUAD end;
    ULONG i, j;
    if (!size) return TRUE;
    if (!m->valid || m->sealed || start >= m->total || size > m->total - start)
        goto bad;
    end = (start + size + PAGE - 1) & ~(PAGE - 1);
    start &= ~(PAGE - 1);
    if (end > m->total) goto bad;
    bug("[amdgpu-vram-map] reserve %s offset=%llx end=%llx\n", name,
        (unsigned long long)start, (unsigned long long)end);
    /* Merge overlapping/adjacent exclusions, never double-count capacity. */
    for (i = 0; i < m->count;) {
        struct AmdgpuVramRange *r = &m->reserved[i];
        if (start > r->end || end < r->start) { i++; continue; }
        if (r->start < start) start = r->start;
        if (r->end > end) end = r->end;
        for (j = i + 1; j < m->count; j++) m->reserved[j - 1] = m->reserved[j];
        m->count--;
        i = 0;
    }
    if (m->count == AMDGPU_VRAM_RANGES) goto bad;
    i = m->count++;
    while (i && m->reserved[i - 1].start > start) {
        m->reserved[i] = m->reserved[i - 1]; i--;
    }
    m->reserved[i].start = start; m->reserved[i].end = end;
    return TRUE;
bad:
    m->valid = FALSE;
    bug("[amdgpu-vram-map] REFUSED invalid reservation %s\n", name);
    return FALSE;
}

void amdgpu_vram_record_gmc(struct AmdgpuDevice *ad, ULONG base, ULONG top)
{
    struct AmdgpuVramMap *m = find_map(ad);
    ULONG i;
    UQUAD end;
    if (!ad || (m && m->sealed)) return;
    if (!m) {
        for (i = 0; i < MAP_DEVICES && maps[i].ad; i++) {}
        if (i == MAP_DEVICES) return;
        m = &maps[i];
    }
    memset(m, 0, sizeof(*m)); m->ad = ad;
    if (base == 0xffffffffUL || top == 0xffffffffUL || !ad->vram_mb ||
        !ad->vram_bar_phys || !ad->vram_bar_size) return;
    m->mc_base = (UQUAD)(base & 0xffffffUL) << 24;
    end = ((UQUAD)(top & 0xffffffUL) + 1) << 24;
    if (end <= m->mc_base) return;
    m->mc_bytes = end - m->mc_base;
    m->total = (UQUAD)ad->vram_mb << 20;
    /* NBIO's usable size can be smaller than the MC window or physical BAR.
     * Never extend allocation merely because the CPU BAR is larger. */
    if (m->total > m->mc_bytes) m->total = m->mc_bytes;
    m->visible = m->total < ad->vram_bar_size ? m->total : ad->vram_bar_size;
    m->visible &= ~(PAGE - 1);
    if (!m->visible || (UQUAD)ad->vram_bar_phys > (UQUAD)~(IPTR)0 - (m->visible - 1))
        return;
    m->valid = TRUE;
}

void amdgpu_vram_record_psp(struct AmdgpuDevice *ad, ULONG index,
    BOOL completed, ULONG status, UQUAD offset, ULONG size)
{
    struct AmdgpuVramMap *m = find_map(ad);
    if (!m || m->sealed || index > 1) return;
    if (!completed || status || !m->valid) { m->valid = FALSE; return; }
    /* Linux psp_update_fw_reservation treats these as VRAM byte OFFSETS.
     * The primary reservation must terminate at the NBIO usable VRAM top. */
    if (!index && (!size || offset >= m->total || size != m->total - offset)) {
        m->valid = FALSE; return;
    }
    if (reserve(m, offset, ((UQUAD)size + MIB - 1) & ~(MIB - 1),
                index ? "PSP-extended" : "PSP-firmware"))
        m->psp_seen |= 1UL << index;
}

static ULONG le16(const UBYTE *p) { return p[0] | ((ULONG)p[1] << 8); }
static ULONG le32(const UBYTE *p) { return le16(p) | (le16(p + 2) << 16); }

static const UBYTE *table(struct AmdgpuDevice *ad, ULONG off, ULONG minimum)
{
    ULONG size;
    if (!off || off > ad->rom_size || ad->rom_size - off < 4) return NULL;
    size = le16(ad->rom_base + off);
    if (size < minimum || size > ad->rom_size - off) return NULL;
    return ad->rom_base + off;
}

static BOOL reserve_atom(struct AmdgpuVramMap *m)
{
    struct AmdgpuDevice *ad = m->ad;
    const UBYTE *rom = ad->rom_base, *header, *master, *fw;
    UQUAD size, protect;
    if (!rom || ad->rom_size < 0x4a || rom[0] != 0x55 || rom[1] != 0xaa)
        return FALSE;
    header = table(ad, le16(rom + 0x48), 36);
    if (!header || memcmp(header + 4, "ATOM", 4) || header[2] != 2 ||
        (header[3] != 2 && header[3] != 3)) return FALSE;
    master = table(ad, le16(header + 32), 14); /* firmwareinfo index4 */
    if (!master || master[2] != 2 || master[3] != 1) return FALSE;
    fw = table(ad, le16(master + 12), 88);
    if (!fw || fw[2] != 3 || (fw[3] != 4 && fw[3] != 5)) return FALSE;
    size = (UQUAD)le32(fw + 84) << 10;
    protect = fw[3] == 5 ? (UQUAD)le32(fw + 20) << 10 : 0;
    if (size > m->total || protect > m->total) return FALSE;
    if (!reserve(m, m->total - size, size, "ATOM-firmware") ||
        !reserve(m, m->total - protect, protect, "ATOM-protected")) return FALSE;
    if (le32(fw + 16) & 0x400) {
        UQUAD off;
        if (size > m->total - MIB) return FALSE;
        off = (m->total - size - MIB + MIB - 1) & ~(MIB - 1);
        if (!reserve(m, off, PAGE, "GDDR6-training")) return FALSE;
    }
    return TRUE;
}

const struct AmdgpuVramMap *amdgpu_vram_seal(struct HWData *hw)
{
    struct AmdgpuVramMap *m = hw ? find_map(hw->ad) : NULL;
    UQUAD display_end, used = 0;
    ULONG i;
    if (!m || !m->valid) return NULL;
    if (m->sealed) return m;
    /* Existing private VRAM workspaces are deliberately excluded as one
     * interval from PSP staging256MiB through CP fence770MiB+1MiB. These
     * addresses belong to the existing PSP/GART/SMU/SDMA/GFX firmware paths;
     * this change does NOT relocate or lend their intervening workspace. */
    if (m->psp_seen != 3 || !reserve_atom(m) || m->total <= MIB ||
        !reserve(m, 256 * MIB, (771 - 256) * MIB, "legacy-driver-workspace") ||
        !reserve(m, m->total - MIB, MIB, "runtime-DB-and-discovery")) goto bad;
    /* Own no part of the established GOP/framebuffer/cursor/bitmap arena.
     * Reserving the complete prefix also covers the old GOP scanout. */
    if (!hw->vram_pool_size || hw->vram_pool_mc < m->mc_base ||
        hw->vram_pool_mc - m->mc_base >= m->total ||
        hw->vram_pool_size > m->total - (hw->vram_pool_mc - m->mc_base)) goto bad;
    display_end = hw->vram_pool_mc - m->mc_base + hw->vram_pool_size;
    if (!reserve(m, 0, display_end, "GOP-display-cursor-bitmap-arena")) goto bad;
    for (i = 0; i < m->count; i++) {
        UQUAD end = m->reserved[i].end < m->visible ? m->reserved[i].end : m->visible;
        if (m->reserved[i].start < end) used += end - m->reserved[i].start;
    }
    if (used >= m->visible) goto bad;
    m->usable = m->visible - used;
    m->sealed = TRUE;
    bug("[amdgpu-vram-map] sealed-v1 MC=%llx total=%llu visible=%llu usable=%llu exclusions=%lu; per-BO CPU maps\n",
        (unsigned long long)m->mc_base, (unsigned long long)m->total,
        (unsigned long long)m->visible, (unsigned long long)m->usable, (unsigned long)m->count);
    return m;
bad:
    m->valid = FALSE;
    bug("[amdgpu-vram-map] REFUSED incomplete/invalid reservation map; display unchanged\n");
    return NULL;
}

BOOL amdgpu_vram_find(const struct AmdgpuVramMap *m, UQUAD size,
    UQUAD alignment, const struct AmdgpuVramRange *busy, ULONG count,
    UQUAD *offset)
{
    UQUAD start, next;
    ULONG i;
    if (!m || !m->valid || !m->sealed || !offset || !size || !alignment ||
        (alignment & (alignment - 1)) || size > m->visible || (count && !busy)) return FALSE;
    /* Allocate top-down. The low display arena stays isolated and native
     * large-BAR tests immediately exercise real offsets above4GiB. */
    start = (m->visible - size) & ~(alignment - 1);
    for (;;) {
        next = start;
        for (i = 0; i < m->count + count; i++) {
            const struct AmdgpuVramRange *r = i < m->count ? &m->reserved[i] : &busy[i - m->count];
            if (r->end < r->start || r->end > m->visible) {
                /* Immutable exclusions can extend beyond the visible BAR. */
                if (i >= m->count) return FALSE;
            }
            if (r->start < start + size && r->end > start) {
                UQUAD before;
                if (r->start < size) return FALSE;
                before = (r->start - size) & ~(alignment - 1);
                if (before < next) next = before;
            }
        }
        if (next == start) { *offset = start; return TRUE; }
        start = next;
    }
}
