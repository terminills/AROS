/* Native adapter for the tested DCN4.1 pipe0 transaction. No device wake/ME. */
#include <proto/exec.h>
#include <proto/timer.h>
#include <devices/timer.h>
#include "amdgpu_intern.h"
#include "amdgpu_plane.h"

struct NativePlaneIO {
    struct AmdgpuDevice *ad;
    struct Device *timer;
};
static uint32_t native_plane_read(void *context, uint32_t reg)
{
    struct AmdgpuDevice *ad = ((struct NativePlaneIO *)context)->ad;
    return amdgpu_rreg32(ad, ad->dmu_seg[2] + reg);
}
static void native_plane_write(void *context, uint32_t reg, uint32_t value)
{
    struct AmdgpuDevice *ad = ((struct NativePlaneIO *)context)->ad;
    amdgpu_wreg32(ad, ad->dmu_seg[2] + reg, value);
}
static uint64_t native_plane_ticks(void *context)
{
    struct Device *TimerBase = ((struct NativePlaneIO *)context)->timer;
    struct EClockVal time;
    ReadEClock(&time);
    return ((uint64_t)time.ev_hi << 32) | time.ev_lo;
}
enum AmdgpuPlaneResult amdgpu_plane_apply(struct AmdgpuDevice *ad,
                                         const struct AmdgpuPlane *plane)
{
    struct MsgPort *port = NULL;
    struct timerequest *request = NULL;
    struct Device *TimerBase = NULL;
    struct NativePlaneIO native = { ad, NULL };
    struct AmdgpuPlaneIO io = { &native, native_plane_read, native_plane_write,
                               native_plane_ticks, 0 };
    struct EClockVal time;
    ULONG frequency;
    enum AmdgpuPlaneResult result = AMDGPU_PLANE_PRESERVED;

    /* This direct-register port is validated for the current Navi44 DUT only.
     * Do not silently apply its pipe0/float-LB assumptions to another ASIC. */
    if (!ad || ad->vendorid != 0x1002 || ad->deviceid != 0x7590 ||
        ad->revision != 0xc0 || !ad->rmmio || !ad->ip.dmu.valid ||
        !ad->dmu_seg[2] ||
        ((UQUAD)ad->dmu_seg[2] + 0x1b89 + 1) * 4 > ad->rmmio_size)
        return result;
    port = CreateMsgPort();
    if (port) request = (struct timerequest *)CreateIORequest(port, sizeof(*request));
    if (!request || OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ,
                               (struct IORequest *)request, 0)) goto out;
    TimerBase = request->tr_node.io_Device;
    native.timer = TimerBase;
    frequency = ReadEClock(&time);
    if (!frequency) goto out;
    io.timeout_ticks = ((uint64_t)frequency + 4) / 5; /* 200ms, not guessed spins */
    result = amdgpu_plane_update(&io, plane);
out:
    if (TimerBase) CloseDevice((struct IORequest *)request);
    if (request) DeleteIORequest((struct IORequest *)request);
    if (port) DeleteMsgPort(port);
    return result;
}
