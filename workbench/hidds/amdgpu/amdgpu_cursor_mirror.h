#ifndef AMDGPU_CURSOR_MIRROR_H
#define AMDGPU_CURSOR_MIRROR_H

#include <exec/types.h>

/* Temporary native acceptance stage: mirror the real AROS shape at x+64,
 * retaining CursorFB ownership until hardware presentation is demonstrated.
 * No cursor/plane registers are touched before an AMD-owned screen is selected.
 */
#define AMDGPU_MIRROR_DIM 64
#define AMDGPU_MIRROR_BYTES (AMDGPU_MIRROR_DIM * AMDGPU_MIRROR_DIM * 4)
struct AmdgpuGfx_staticdata;
struct Device;
struct AmdgpuCursorMirror {
    APTR cpu;
    UQUAD mc;
    struct MsgPort *port;
    struct timerequest *request;
    struct Device *timer;
    ULONG frequency;
    ULONG width, height;
    LONG x, y, xoffset, yoffset;
    BOOL started, dirty, valid, failed;
    BOOL timing_warned;
    BOOL dlg_mode_checked;
    ULONG ratio_q19, delivery_q10;
    ULONG measured_ratio, clock_htotal, clock_vtotal, clock_timer;
    ULONG reports;
    ULONG shape_checks, visibility_reports;
    ULONG argb[AMDGPU_MIRROR_DIM * AMDGPU_MIRROR_DIM];
    ULONG readback[AMDGPU_MIRROR_DIM * AMDGPU_MIRROR_DIM];
};

void amdgpu_cursor_mirror_prepare(struct AmdgpuGfx_staticdata *xsd);
void amdgpu_cursor_mirror_update(struct AmdgpuGfx_staticdata *xsd, BOOL visible);

#endif
