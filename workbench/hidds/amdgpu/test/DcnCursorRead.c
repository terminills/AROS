/* Read-only cursor idle/pending snapshot v2 on the known JOB-0084 Navi44 DUT.
 * Same fixed PCI/BAR guard as DcnStateRead. No amdgpu OpenLibrary/init,
 * PCI writes, indexed MMIO, DMA, reset, or driver replacement. Three bounded
 * snapshots collect before logging. Raw state does not prove visible pixels.
 * Registers describe the already installed/rebooted driver, never a hot-loaded
 * alternative. No plane/cursor software locks are acquired by this observer.
 */
#define DEBUG 1
#include <aros/debug.h>
#include <proto/oop.h>
#include <proto/dos.h>
#include <hidd/pci.h>
#include <hardware/pci.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>
/* Fixed bases captured on this DUT; offsets from Linux f377d0025eb0
 * dcn_4_1_0_offset.h: DMU2=34c0, DMU3=9000. No indirect selectors. */
#define DCN_STATE_DMU2 0x34c0u
#define DCN_STATE_BAR_BYTES (512u * 1024u)

#define LOG(...) do { bug("[DcnCursorRead] " __VA_ARGS__); bug("\n"); } while (0)

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
    unsigned int pass;
    static const struct { const char *name; ULONG index; } regs[] = {
        { "HUBP0_CNTL", 0x3ab4 },
        { "HUBP0_CLOCK", 0x3ab5 },
        { "HUBPREQ0_EXPANSION_MODE", 0x3ae0 },
        { "HUBPREQ0_CUR0_TTU", 0x3ae7 },
        { "HUBPREQ0_CUR0_TTU_PREFETCH", 0x3ae8 },
        { "HUBPREQ0_CURSOR_SETTINGS", 0x3b13 },
        { "HUBPREQ0_STATUS0", 0x3b21 },
        { "HUBPREQ0_STATUS1", 0x3b22 },
        { "HUBPREQ0_STATUS2", 0x3b23 },
        { "HUBPREQ0_STATUS3", 0x3b24 },
        { "CURSOR0_CONTROL", 0x3b39 },
        { "CURSOR0_ADDRESS", 0x3b3a },
        { "CURSOR0_ADDRESS_HIGH", 0x3b3b },
        { "CURSOR0_SIZE", 0x3b3c },
        { "CURSOR0_POSITION", 0x3b3d },
        { "CURSOR0_HOTSPOT", 0x3b3e },
        { "CURSOR0_POWER", 0x3b41 },
        { "CURSOR0_POWER_STATUS", 0x3b42 },
        { "CURSOR0_3DLUT_CONTROL", 0x3b4a },
        { "DPP0_CONTROL", 0x4185 },
        { "CM_CUR0_CONTROL", 0x41b1 },
        { "CM_CUR0_SCALE_G", 0x41b4 },
        { "CM_CUR0_SCALE_RB", 0x41b5 },
        { "OTG0_POSITION", 0x500a },
        { "OTG0_FRAME", 0x500d },
        { "OTG0_PIPE_UPDATE_STATUS", 0x505e },
        { "MPC_DPP_PENDING_STATUS", 0x92bf },
        { "CUR_VUPDATE_LOCK_SET0", 0x92c5 },
        { "MPC_OUT0_MUX", 0x92f2 },
    };
    ULONG values[sizeof(regs) / sizeof(regs[0])];
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
    LOG("BEGIN v2 read-only sequential snapshot: 1002:7590 rev c0 CMD=%04lx BAR5=%p bytes=%lu DMU2=%x (prior DUT discovery; no plane update lock taken)",
        (unsigned long)command, (APTR)phys, (unsigned long)bytes,
        DCN_STATE_DMU2);
    result = RETURN_OK;
    for (pass = 0; pass < 3; ++pass) {
        /* Collect the fixed group before UART logging changes its timing.
         * Sequential reads are not an atomic cursor-update-lock snapshot. */
        for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
            values[i] = mmio[regs[i].index];
            if (values[i] == 0xffffffffUL) {
                result = RETURN_FAIL;
                break;
            }
        }
        if (result != RETURN_OK) {
            LOG("FAIL unreadable register %s; no more MMIO reads", regs[i].name);
            break;
        }
        for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
            LOG("sample%u %s @DW%05lx = %08lx", pass + 1, regs[i].name,
                (unsigned long)regs[i].index, (unsigned long)values[i]);
        if (pass < 2) Delay(50);
    }
    HIDD_PCIDriver_UnmapPCI(driver, (APTR)mmio, bytes);
    LOG("END; no register/config writes, initialization, or reset");
out:
    if (pci) OOP_DisposeObject(pci);
    if (s.attr) OOP_ReleaseAttrBase((STRPTR)IID_Hidd_PCIDevice);
    Printf((CONST_STRPTR)"DcnCursorRead: result %ld (details on serial)\n", (IPTR)result);
    return result;
}
