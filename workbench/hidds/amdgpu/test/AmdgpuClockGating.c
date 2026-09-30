/* Turn amdgpu.hidd's GFX clock gating on or off at run time
 * (amdgpu_set_gfx_clock_gating, ABI 0.14). Gating is off after boot; this is
 * how it is A/B-tested on hardware with C:AmdgpuSensors and GfxStateRead
 * before it becomes the default. Refuses to load an absent driver.
 *
 *   AmdgpuClockGating ON
 *   AmdgpuClockGating OFF
 */
#define DEBUG 1
#define __INLINE_AMDGPU_STACKCALL__ 1
#include <aros/debug.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/amdgpu.h>
#include <stdio.h>
#include <string.h>

struct Library *AmdgpuBase;
#define LOG(...) do { printf("[AmdgpuClockGating] " __VA_ARGS__); printf("\n"); \
                       bug("[AmdgpuClockGating] " __VA_ARGS__); bug("\n"); } while (0)

int main(void)
{
    struct Library *resident;
    struct AmdgpuDevice *ad;
    struct RDArgs *rd;
    IPTR args[1] = { 0 };
    LONG enable = -1, r, result = RETURN_FAIL;

    rd = ReadArgs((CONST_STRPTR)"STATE/A", args, NULL);
    if (rd) {
        if (!strcmp((char *)args[0], "ON") || !strcmp((char *)args[0], "on")) enable = 1;
        if (!strcmp((char *)args[0], "OFF") || !strcmp((char *)args[0], "off")) enable = 0;
        FreeArgs(rd);
    }
    if (enable < 0) {
        LOG("usage: AmdgpuClockGating ON|OFF");
        return result;
    }

    Forbid();
    resident = (struct Library *)FindName(&SysBase->LibList, "amdgpu.hidd");
    Permit();
    if (!resident) {
        LOG("REFUSED: driver is not already resident; no OpenLibrary attempted");
        return result;
    }
    AmdgpuBase = OpenLibrary("amdgpu.hidd", 0);
    if (!AmdgpuBase) return result;
    if (AmdgpuBase->lib_Version == 0 && AmdgpuBase->lib_Revision < 14) {
        LOG("REFUSED: resident amdgpu.hidd is %u.%u, clock gating control needs 0.14",
            AmdgpuBase->lib_Version, AmdgpuBase->lib_Revision);
        goto out;
    }
    if (amdgpu_get_device_count() < 1 || !(ad = amdgpu_get_device(0))) {
        LOG("FAIL: no device");
        goto out;
    }
    r = amdgpu_set_gfx_clock_gating(ad, (ULONG)enable);
    LOG("GFX clock gating %s -> %ld", enable ? "ON" : "OFF", (long)r);
    if (!r) result = RETURN_OK;
out:
    CloseLibrary(AmdgpuBase);
    return result;
}
