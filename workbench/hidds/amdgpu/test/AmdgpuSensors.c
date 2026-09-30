/* Native read-only client of amdgpu.hidd's sensor query (amdgpu_query_sensors,
 * ABI 0.13). Prints live clocks, activity, power, temperatures and fan from the
 * SMU metrics table, as read by the resident driver under its own locks.
 * Refuses to load an absent driver; opening the resident module only takes a
 * library reference. No register access, mailbox use or GPU submission here.
 *
 *   AmdgpuSensors            one sample
 *   AmdgpuSensors COUNT=10   ten samples, one per second
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

/* Explicit base and generated inline calls: do NOT link -lamdgpu/autoinit,
 * which could open the driver before main's already-resident guard. */
struct Library *AmdgpuBase;
#define LOG(...) do { printf("[AmdgpuSensors] " __VA_ARGS__); printf("\n"); \
                       bug("[AmdgpuSensors] " __VA_ARGS__); bug("\n"); } while (0)

int main(void)
{
    struct Library *resident;
    struct AmdgpuDevice *ad;
    struct AmdgpuSensorInfo s;
    struct RDArgs *rd;
    IPTR args[1] = { 0 };
    LONG count = 1, i, r;
    LONG result = RETURN_FAIL;

    rd = ReadArgs((CONST_STRPTR)"COUNT/N", args, NULL);
    if (rd) {
        if (args[0]) count = *(LONG *)args[0];
        FreeArgs(rd);
    }
    if (count < 1) count = 1;

    Forbid();
    resident = (struct Library *)FindName(&SysBase->LibList, "amdgpu.hidd");
    Permit();
    if (!resident) {
        LOG("REFUSED: driver is not already resident; no OpenLibrary attempted");
        return result;
    }
    AmdgpuBase = OpenLibrary("amdgpu.hidd", 0);
    if (!AmdgpuBase) return result;
    /* amdgpu.conf's "version 0.13" is version 0, revision 13: the sensor
     * query exists from revision 13 on. */
    if (AmdgpuBase->lib_Version == 0 && AmdgpuBase->lib_Revision < 13) {
        LOG("REFUSED: resident amdgpu.hidd is %u.%u, the sensor query needs 0.13",
            AmdgpuBase->lib_Version, AmdgpuBase->lib_Revision);
        goto out;
    }
    if (amdgpu_get_device_count() < 1 || !(ad = amdgpu_get_device(0))) {
        LOG("FAIL: no device");
        goto out;
    }
    for (i = 0; i < count; i++) {
        if (i) Delay(50);
        r = amdgpu_query_sensors(ad, &s, sizeof(s));
        if (r) {
            LOG("query failed: %ld", (long)r);
            goto out;
        }
        LOG("#%lu GFXCLK %lu (avg %lu) MHz  MEM %lu  SOC %lu  FCLK %lu | GFX %lu%%  MEM %lu%% | "
            "%lu W | edge %lu C  hotspot %lu C  mem %lu C | fan %lu%% %lu rpm",
            (unsigned long)s.sample_counter, (unsigned long)s.gfxclk_cur,
            (unsigned long)s.gfxclk_avg, (unsigned long)s.uclk_cur,
            (unsigned long)s.socclk_cur, (unsigned long)s.fclk_cur,
            (unsigned long)s.gfx_activity, (unsigned long)s.mem_activity,
            (unsigned long)s.board_power, (unsigned long)s.temp_edge,
            (unsigned long)s.temp_hotspot, (unsigned long)s.temp_mem,
            (unsigned long)s.fan_pwm, (unsigned long)s.fan_rpm);
    }
    result = RETURN_OK;
out:
    CloseLibrary(AmdgpuBase);
    return result;
}
