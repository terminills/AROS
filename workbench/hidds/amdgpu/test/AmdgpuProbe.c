/*
 * AmdgpuProbe — deliberate trigger for the amdgpu.hidd tier-1 ladder.
 *
 * amdgpu.hidd ships inert: nothing opens it during a normal boot. Opening it
 * here runs its ADD2INITLIB init, which does the real work — PCI enumerate ->
 * identify against the ID table -> map the register aperture -> read VRAM
 * size -> parse the IP-discovery table -> Device/Query readout — logging the
 * whole ladder to the serial console as [amdgpu] lines.
 *
 * Optional LIBRARY=path loads an explicit staged driver on a fresh boot, so a
 * native test need not overwrite the installed baseline. Keep the basename
 * amdgpu.hidd: lddemon opens by basename after resident initialization. An
 * already-resident amdgpu.hidd is reused even when an explicit path is given;
 * use a fresh boot to validate a different build. This runs hardware initialization.
 * This program's own output is just the frame: whether the hidd opened at all.
 * The evidence is the [amdgpu] serial log. Run under QEMU with the V620 passed
 * through:  VFIOPASS=0000:03:00.0 ./run-opal-smp.sh
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <aros/debug.h>

#include <stdio.h>
#include <string.h>

#define PLOG(...) do { printf("[AmdgpuProbe] " __VA_ARGS__); printf("\n"); \
                       bug("[AmdgpuProbe] " __VA_ARGS__); bug("\n"); } while (0)

int main(void)
{
    struct Library *amdgpuBase;
    struct RDArgs *rd;
    IPTR args[1] = { 0 };
    CONST_STRPTR name = "amdgpu.hidd";

    rd = ReadArgs("LIBRARY/K", args, NULL);
    if (!rd) {
        PrintFault(IoErr(), "AmdgpuProbe");
        return 20;
    }
    if (args[0])
        name = (CONST_STRPTR)args[0];
    if (strcmp((const char *)FilePart(name), "amdgpu.hidd") != 0) {
        PLOG("FAIL: LIBRARY must retain the amdgpu.hidd basename (for example RAM:amdgpu.hidd)");
        FreeArgs(rd);
        return 20;
    }

    PLOG("opening %s (its init runs hardware bring-up)...", name);

    amdgpuBase = OpenLibrary(name, 0);
    if (amdgpuBase == NULL) {
        PLOG("FAIL: could not open amdgpu.hidd -- not in the image, or its "
             "init refused (see [amdgpu] serial lines above)");
        FreeArgs(rd);
        return 20;
    }

    PLOG("amdgpu.hidd opened (base=%p). Tier-1 probe results are the [amdgpu] "
         "lines on serial: identify, BAR map, VRAM MB, IP versions.", amdgpuBase);

    CloseLibrary(amdgpuBase);
    FreeArgs(rd);
    PLOG("done");
    return 0;
}
