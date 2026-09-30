/*
 * amdgpu monitor loader — workbench-launch entry for amdgpu.hidd.
 *
 * amdgpu.hidd used to cold-start at residentpri 49 (pre-mount) to do the PSP
 * silicon wake. That ran before dos/intuition existed AND, on the DUT, left the
 * boot display in a state AROS could not adopt (the GPU is awake + the DCN is
 * live-scanning the GOP framebuffer, but nothing binds it as an AROS display, so
 * Wanderer renders to the null Headless driver and the monitor shows nothing).
 *
 * This command lives in DEVS:Monitors and is run by AROSMonDrvs during the
 * Startup-Sequence, exactly like the Headless monitor loader (rom/hidds/gfx/
 * headless/disk_startup.c). By then DOS/SYS: and intuition exist. Opening
 * amdgpu.hidd runs its ADD2INITLIB init = PCI detect -> IP discovery -> PSP wake
 * -> DCN scanout observe. The library is left open on purpose so the driver is
 * not expunged.
 *
 * The display itself is now a real OOP gfx-HIDD registered from amdgpu.hidd's
 * own init (Amdgpu_Init -> AmdgpuGfx_InitDisplay): opening the hidd both wakes
 * the GPU AND publishes a DDRV_BootMode display that adopts the firmware/GOP
 * framebuffer (bootloader.resource VesaInfo). So this loader just OpenLibrary's
 * amdgpu.hidd and leaves it resident; there is nothing to paint here anymore.
 */

#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <exec/libraries.h>

int __nocommandline = 1;

int main(void)
{
    struct Library *AmdgpuBase;

    bug("[amdgpu-mon] workbench-launch loader: opening amdgpu.hidd (post-mount wake)\n");

    /* Opening the hidd runs its ADD2INITLIB init: probe -> discovery -> PSP wake
     * -> DCN observe. Search path first, then DEVS:Drivers as a fallback (same
     * shape as the headless loader). */
    AmdgpuBase = OpenLibrary("amdgpu.hidd", 0);
    if (!AmdgpuBase)
        AmdgpuBase = OpenLibrary("DEVS:Drivers/amdgpu.hidd", 0);

    if (!AmdgpuBase)
    {
        bug("[amdgpu-mon] amdgpu.hidd would not open; nothing to do\n");
        return RETURN_WARN;
    }

    bug("[amdgpu-mon] amdgpu.hidd open @ 0x%p — silicon wake + boot-mode display "
        "registered at workbench launch\n", AmdgpuBase);

    /* Left open on purpose — closing the last ref would expunge the driver. */
    bug("[amdgpu-mon] done; amdgpu.hidd left resident (boot-mode gfx display registered)\n");
    return RETURN_OK;
}
