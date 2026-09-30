/* Read only the fixed register list on the known JOB-0084 Navi44 DUT.
 * No driver OpenLibrary, PCI enable, discovery, indexed MMIO, register writes,
 * selector changes, DMA, or reset. Unknown hardware/BAR geometry fails closed.
 * Bases come from this DUT's captured IP discovery, NOT PCI-ID inference.
 * The snapshot is sequential, not atomic; it does not take the plane update lock.
 * Same fixed PCI/BAR guard as GfxStateRead; this separate binary uses only the
 * DCN plane/scaler register allowlist. Raw readback is not proof of visible pixels.
 */
#define DEBUG 1
#include <aros/debug.h>
#include <proto/oop.h>
#include <proto/dos.h>
#include <hidd/pci.h>
#include <hardware/pci.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>
#include "dcn_state_regs.h"

#define LOG(...) do { bug("[DcnStateRead] " __VA_ARGS__); bug("\n"); } while (0)

struct Snapshot {
    OOP_AttrBase attr;
    OOP_Object *device;
    unsigned int matches;
};

static AROS_UFH3(void, find_dut,
    AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(OOP_Object *, device, A2),
    AROS_UFHA(APTR, message, A1))
{
    AROS_USERFUNC_INIT
    struct Snapshot *s = hook->h_Data;
    IPTR vendor = 0, product = 0, revision = 0;
    (void)message;
    OOP_GetAttr(device, s->attr + aoHidd_PCIDevice_VendorID, &vendor);
    OOP_GetAttr(device, s->attr + aoHidd_PCIDevice_ProductID, &product);
    OOP_GetAttr(device, s->attr + aoHidd_PCIDevice_RevisionID, &revision);
    if (vendor == 0x1002 && product == 0x7590 && revision == 0xc0) {
        s->device = device;
        s->matches++;
    }
    AROS_USERFUNC_EXIT
}

int main(int argc, char **argv)
{
    struct Snapshot s = { 0 };
    struct Hook hook = { 0 };
    OOP_Object *pci = NULL, *driver;
    IPTR phys = 0, bytes = 0, driver_attr = 0;
    volatile const ULONG *mmio = NULL;
    LONG result = RETURN_FAIL;
    ULONG command;
    unsigned int i;
    enum {
#define INDEX(name, segment, offset) SAMPLE_##name,
        DCN_STATE_REGS(INDEX)
#undef INDEX
        SAMPLE_COUNT
    };
    ULONG values[SAMPLE_COUNT];
    static const struct { const char *name; ULONG index; } regs[] = {
#define ENTRY(name, segment, offset) \
        { #name, DCN_STATE_DMU2 + (offset) },
        DCN_STATE_REGS(ENTRY)
#undef ENTRY
    };
    struct TagItem reqs[] = {
        { tHidd_PCI_VendorID, 0x1002 },
        { tHidd_PCI_Class, 0x03 },
        { TAG_DONE, 0 }
    };
    struct pHidd_PCI_EnumDevices enumerate = {
        .mID = OOP_GetMethodID(IID_Hidd_PCI, moHidd_PCI_EnumDevices),
        .callback = &hook,
        .requirements = reqs
    };
    struct pHidd_PCIDevice_ReadConfigWord read_command = {
        .mID = OOP_GetMethodID(IID_Hidd_PCIDevice, moHidd_PCIDevice_ReadConfigWord),
        .reg = PCICS_COMMAND
    };
    (void)argv;
    if (argc != 1) {
        LOG("FAIL: no arguments accepted; fixed read-only Navi44 snapshot");
        return RETURN_FAIL;
    }
    s.attr = OOP_ObtainAttrBase((STRPTR)IID_Hidd_PCIDevice);
    if (!s.attr) goto out;
    pci = OOP_NewObject(NULL, CLID_Hidd_PCI, NULL);
    if (!pci) goto out;
    hook.h_Entry = (HOOKFUNC)find_dut;
    hook.h_Data = &s;
    OOP_DoMethod(pci, (OOP_Msg)&enumerate);
    if (s.matches != 1) {
        LOG("FAIL: expected exactly one 1002:7590 rev c0, found %u", s.matches);
        goto out;
    }
    command = OOP_DoMethod(s.device, (OOP_Msg)&read_command);
    OOP_GetAttr(s.device, s.attr + aoHidd_PCIDevice_Base5, &phys);
    OOP_GetAttr(s.device, s.attr + aoHidd_PCIDevice_Size5, &bytes);
    OOP_GetAttr(s.device, s.attr + aoHidd_PCIDevice_Driver, &driver_attr);
    driver = (OOP_Object *)driver_attr;
    if (!(command & PCICMF_MEMDECODE) || !phys || (phys & 4095) ||
        bytes != DCN_STATE_BAR_BYTES || !driver) {
        LOG("FAIL: PCI memory decode/BAR5 geometry/driver precondition");
        goto out;
    }
    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
        if (regs[i].index >= bytes / sizeof(ULONG)) goto out;
    mmio = HIDD_PCIDriver_MapPCI(driver, (APTR)phys, bytes);
    if (!mmio || (APTR)mmio == (APTR)-1) goto out;
    LOG("BEGIN read-only sequential snapshot: 1002:7590 rev c0 CMD=%04lx BAR5=%p bytes=%lu DMU2=%x (prior DUT discovery; no plane update lock taken)",
        (unsigned long)command, (APTR)phys, (unsigned long)bytes,
        DCN_STATE_DMU2);
    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        ULONG value = mmio[regs[i].index];
        values[i] = value;
        LOG("%s @DW%05lx = %08lx", regs[i].name,
            (unsigned long)regs[i].index, (unsigned long)value);
    }
    /* Linux hubp2_is_flip_pending (inherited by DCN4.1) checks both the
     * pending bit and EARLIEST_INUSE vs requested address. The requested
     * address readback by itself does not establish latch or retirement.
     * These raw pairs are sequential observations, not an atomic proof. */
    LOG("FLIP sample pending=%lu requested=%08lx%08lx earliest-in-use=%08lx%08lx (sequential; not visible-pixel or retirement proof)",
        (unsigned long)!!(values[SAMPLE_HUBPREQ0_DCSURF_FLIP_CONTROL] & DCN_STATE_FLIP_PENDING),
        (unsigned long)values[SAMPLE_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH],
        (unsigned long)values[SAMPLE_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS],
        (unsigned long)values[SAMPLE_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH],
        (unsigned long)values[SAMPLE_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE]);
    HIDD_PCIDriver_UnmapPCI(driver, (APTR)mmio, bytes);
    result = RETURN_OK;
    LOG("END; no register/config writes, initialization, or reset");
out:
    if (pci) OOP_DisposeObject(pci);
    if (s.attr) OOP_ReleaseAttrBase((STRPTR)IID_Hidd_PCIDevice);
    Printf((CONST_STRPTR)"DcnStateRead: result %ld (details on serial)\n", (IPTR)result);
    return result;
}
