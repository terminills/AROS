/*
 * DCN4.1 single-pipe, progressive, linear RGB plane transaction.
 * Local Linux f377d0025eb0: hubp401_program_surface_flip_and_addr,
 * hubp2_is_flip_pending, dpp401_dscl_set_scaler_manual_scale and optc1_lock.
 * No clocks/link training/ME work, no GUI changes. Timing stays unchanged.
 * The I/O interface permits deterministic tests of the actual sequencing.
 * This engine is not safe to integrate unless storage is retained on UNCERTAIN.
 */
#include "amdgpu_plane.h"

#define P_LOCK 0x1b89u
#define P_CONTROL 0x1b43u
#define P_HBLANK 0x1b2bu
#define P_VBLANK 0x1b38u
#define P_FRAME_COUNT 0x1b4du
#define P_FLIP 0x0613u
#define P_ADDR 0x060au
#define P_ADDR_HI 0x060bu
#define P_INUSE 0x061cu
#define P_INUSE_HI 0x061du
#define P_PENDING 0x100u
#define P_FLIP_FIELDS 0x13003u /* update lock, type, stereo mode/in-stereo */
#define P_MAX_POLLS 262144u  /* Also bounded if the supplied clock stops. */

/* RW masks only: status bits in e.g. LB_MEMORY_CTRL are not a write oracle. */
static const struct { uint32_t reg, mask; } plane_fields[] = {
    { 0x05e5, 0x7f },       /* pixel format */
    { 0x05e7, 0x7 },        /* linear swizzle */
    { 0x0609, 0xf },        /* VMID0 physical VRAM */
    { 0x0612, 0x3 },        /* no TMZ or DCC */
    { 0x05e9, UINT32_MAX }, /* viewport start:16 bits per coordinate */
    { 0x05eb, 0x3fff3fff }, /* viewport dimensions */
    { 0x0607, 0xffff },     /* pixel pitch minus1 */
    { 0x0d19, 0x3303 },     /* AutoCal off, single pipe */
    { 0x0d0a, 0x1 },        /* boundary mode off */
    { 0x0d1e, 0x3fff3fff }, /* recout start */
    { 0x0d1f, 0x3fff3fff }, /* recout size */
    { 0x0d20, 0x3fff3fff }, /* MPC active raster size */
    { 0x0d08, 0x7 },        /* RGB unity ratio: SCALING_444_BYPASS=0, NOT6 */
    { 0x0d21, 0x11 },       /* DCN4.1 float LB: no interleave/alpha */
    { 0x0d22, 0x3f03 },     /* full LB memory config0,63 partitions */
};
#define P_FIELDS (sizeof(plane_fields) / sizeof(plane_fields[0]))

/* -1 means the MMIO aperture stopped responding: never write after that. */
static int plane_read(const struct AmdgpuPlaneIO *io, uint32_t reg, uint32_t *v)
{
    *v = io->read(io->context, reg);
    return *v == UINT32_MAX ? -1 : 1;
}

static int plane_write(const struct AmdgpuPlaneIO *io, uint32_t reg,
                       uint32_t mask, uint32_t value)
{
    uint32_t before, after;
    if (plane_read(io, reg, &before) < 0) return -1;
    io->write(io->context, reg, (before & ~mask) | (value & mask));
    if (plane_read(io, reg, &after) < 0) return -1;
    return (after & mask) == (value & mask);
}

static int plane_wait_mask(const struct AmdgpuPlaneIO *io, uint32_t reg,
                           uint32_t mask, uint32_t want)
{
    uint64_t start = io->ticks(io->context);
    uint32_t i, v;
    for (i = 0; i < P_MAX_POLLS; ++i) {
        if (plane_read(io, reg, &v) < 0) return -1;
        if ((v & mask) == want) return 1;
        if (io->ticks(io->context) - start >= io->timeout_ticks) break;
    }
    return 0;
}

static int plane_wait_inuse(const struct AmdgpuPlaneIO *io, uint64_t mc)
{
    uint64_t start = io->ticks(io->context);
    uint32_t i, flip0, flip1, high0, high1, low, lock;
    for (i = 0; i < P_MAX_POLLS; ++i) {
        if (plane_read(io, P_FLIP, &flip0) < 0 ||
            plane_read(io, P_INUSE_HI, &high0) < 0 ||
            plane_read(io, P_INUSE, &low) < 0 ||
            plane_read(io, P_INUSE_HI, &high1) < 0 ||
            plane_read(io, P_FLIP, &flip1) < 0 ||
            plane_read(io, P_LOCK, &lock) < 0) return -1;
        /* VMID must be0. HIGH also contains VMID; do not silently discard it.
         * Two high/flip observations reject a torn or still-pending sample. */
        if (!((flip0 | flip1) & (P_PENDING | 0x201u)) &&
            !(lock & 0x101u) && high0 == high1 && !(high0 & 0xf0000000u) &&
            (((uint64_t)(high0 & 0xffffu) << 32) | low) == mc) return 1;
        if (io->ticks(io->context) - start >= io->timeout_ticks) break;
    }
    return 0;
}

static int plane_address(const struct AmdgpuPlaneIO *io, uint64_t mc)
{
    int r = plane_write(io, P_ADDR_HI, 0xffffu, (uint32_t)(mc >> 32));
    if (r != 1) return r;
    return plane_write(io, P_ADDR, UINT32_MAX, (uint32_t)mc);
}

/* Same-address panning cannot be acknowledged by INUSE alone. Observe two
 * complete frame-counter advances after unlock, then recheck INUSE/pending.
 * DCN4.1 OTG_STATUS_FRAME_COUNT is 24 bits (Linux optc1_get_vblank_counter).
 * This is a bounded latch opportunity, not a replacement for HDMI validation. */
static int plane_wait_frames(const struct AmdgpuPlaneIO *io)
{
    uint64_t start = io->ticks(io->context);
    uint32_t first, now, i;
    if (plane_read(io, P_FRAME_COUNT, &first) < 0) return -1;
    for (i = 0; i < P_MAX_POLLS; ++i) {
        if (plane_read(io, P_FRAME_COUNT, &now) < 0) return -1;
        if (((now - first) & 0xffffffu) >= 2) return 1;
        if (io->ticks(io->context) - start >= io->timeout_ticks) break;
    }
    return 0;
}

static int plane_release(const struct AmdgpuPlaneIO *io, uint32_t flip)
{
    int r = plane_write(io, P_FLIP, P_FLIP_FIELDS, flip & ~1u);
    if (r != 1) return r;
    r = plane_write(io, P_LOCK, 1, 0);
    if (r != 1) return r;
    return plane_wait_mask(io, P_LOCK, 0x101, 0);
}

enum AmdgpuPlaneResult amdgpu_plane_update(const struct AmdgpuPlaneIO *io,
                                          const struct AmdgpuPlane *p)
{
    uint32_t old[P_FIELDS], want[P_FIELDS];
    uint32_t control, lock, flip, high, low, hb, vb;
    uint32_t i, rx, ry;
    uint64_t old_mc;
    int r;

    if (!io || !io->read || !io->write || !io->ticks || !io->timeout_ticks ||
        !p || !p->mc || (p->mc & 255) || p->mc > UINT64_C(0xffffffffffff) ||
        !p->width || p->width > 0x3fff || !p->height || p->height > 0x3fff ||
        !p->raster_width || p->raster_width > 0x3fff ||
        !p->raster_height || p->raster_height > 0x3fff ||
        p->width > p->raster_width || p->height > p->raster_height ||
        p->pitch_pixels < p->width || p->pitch_pixels > 0x10000 ||
        p->source_x > 0xffff || p->source_y > 0xffff ||
        (uint64_t)p->source_x + p->width > p->pitch_pixels ||
        (uint64_t)p->source_y + p->height > p->backing_height ||
        (p->bytes_per_pixel != 2 && p->bytes_per_pixel != 4))
        return AMDGPU_PLANE_PRESERVED;

    if (plane_read(io, P_CONTROL, &control) < 0 ||
        plane_read(io, P_LOCK, &lock) < 0 ||
        plane_read(io, P_FLIP, &flip) < 0 ||
        plane_read(io, P_HBLANK, &hb) < 0 ||
        plane_read(io, P_VBLANK, &vb) < 0 ||
        plane_read(io, P_ADDR_HI, &high) < 0 ||
        plane_read(io, P_ADDR, &low) < 0) return AMDGPU_PLANE_UNCERTAIN;
    if (!(control & 1) || (lock & 0x101) || (flip & (P_PENDING | 0x201)) ||
        (high & ~0xffffu) || (low & 255) ||
        (hb & 0xffff) < (hb >> 16) || (vb & 0xffff) < (vb >> 16) ||
        (hb & 0xffff) - (hb >> 16) != p->raster_width ||
        (vb & 0xffff) - (vb >> 16) != p->raster_height)
        return AMDGPU_PLANE_PRESERVED;
    old_mc = ((uint64_t)high << 32) | low;
    r = plane_wait_inuse(io, old_mc);
    if (r != 1) return r < 0 ? AMDGPU_PLANE_UNCERTAIN : AMDGPU_PLANE_PRESERVED;

    r = plane_write(io, P_LOCK, 1, 1);
    if (r < 0) return AMDGPU_PLANE_UNCERTAIN;
    if (r == 1) r = plane_wait_mask(io, P_LOCK, 0x101, 0x101);
    if (r != 1) {
        if (r < 0 || plane_write(io, P_LOCK, 1, 0) != 1 ||
            plane_wait_mask(io, P_LOCK, 0x101, 0) != 1)
            return AMDGPU_PLANE_UNCERTAIN;
        return AMDGPU_PLANE_PRESERVED;
    }

    /* Snapshot under our hardware lock, before any plane mutation. */
    for (i = 0; i < P_FIELDS; ++i)
        if (plane_read(io, plane_fields[i].reg, &old[i]) < 0)
            return AMDGPU_PLANE_UNCERTAIN;

    rx = (p->raster_width - p->width) / 2;
    ry = (p->raster_height - p->height) / 2;
    want[0] = p->bytes_per_pixel == 2 ? 3 : 8;
    want[1] = want[2] = want[3] = 0;
    want[4] = (p->source_y << 16) | p->source_x;
    want[5] = (p->height << 16) | p->width;
    want[6] = p->pitch_pixels - 1;
    want[7] = want[8] = 0;
    want[9] = (ry << 16) | rx;
    want[10] = want[5];
    want[11] = (p->raster_height << 16) | p->raster_width;
    want[12] = want[13] = 0;
    want[14] = 0x3f00;

    /* Also hold the surface-address latch. LOW is not a commit until the
     * whole plane has verified and both owned locks are released. */
    r = plane_write(io, P_FLIP, P_FLIP_FIELDS, 1);
    if (r < 0) return AMDGPU_PLANE_UNCERTAIN;
    if (r != 1) goto rollback;
    for (i = 0; i < P_FIELDS; ++i) {
        r = plane_write(io, plane_fields[i].reg, plane_fields[i].mask, want[i]);
        if (r < 0) return AMDGPU_PLANE_UNCERTAIN;
        if (r != 1) goto rollback;
    }
    r = plane_address(io, p->mc);
    if (r < 0) return AMDGPU_PLANE_UNCERTAIN;
    if (r != 1) goto rollback;
    if (plane_release(io, 0) != 1 ||
        (old[4] != want[4] && plane_wait_frames(io) != 1) ||
        plane_wait_inuse(io, p->mc) != 1)
        return AMDGPU_PLANE_UNCERTAIN;
    return AMDGPU_PLANE_ACTIVE;

rollback:
    /* Only rollback while our locks still prevent a partial live update.
     * If restoration itself cannot be verified, do not release any storage. */
    for (i = 0; i < P_FIELDS; ++i)
        if (plane_write(io, plane_fields[i].reg, plane_fields[i].mask, old[i]) != 1)
            return AMDGPU_PLANE_UNCERTAIN;
    if (plane_address(io, old_mc) != 1 || plane_release(io, flip) != 1 ||
        plane_wait_inuse(io, old_mc) != 1) return AMDGPU_PLANE_UNCERTAIN;
    return AMDGPU_PLANE_PRESERVED;
}
