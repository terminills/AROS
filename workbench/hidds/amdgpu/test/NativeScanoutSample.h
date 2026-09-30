/* JOB-0084 read-only observer for the already-booted Navi44 driver.
 * Fixed PCI/BAR/DMU/pool identity from cold38 UART; no driver OpenLibrary,
 * MapPCI, KrnMapGlobal, MMIO/config writes, flush, DMA, or repair rendering.
 * In particular MapPCI would remap this high VRAM BAR cache-inhibited: do not
 * call it and disturb the installed driver's WC mapping. PCItoCPU translates
 * only; this observer requires the driver's existing pool mapping.
 * CPU aperture samples are not DCN-fetch proof. Pair them with fresh HDMI.
 */
#include <proto/oop.h>
#include <hidd/pci.h>
#include <hardware/pci.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>

struct NativeScanoutSample {
    OOP_AttrBase attr;
    OOP_Object *pci, *device, *driver;
    volatile const ULONG *mmio;
    unsigned int matches;
    BOOL failed;
};
static struct NativeScanoutSample native_scanout;

static AROS_UFH3(void, native_scanout_find,
    AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(OOP_Object *, device, A2),
    AROS_UFHA(APTR, message, A1))
{
    AROS_USERFUNC_INIT
    struct NativeScanoutSample *s = hook->h_Data;
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

static void native_scanout_close(void)
{
    if (native_scanout.pci) OOP_DisposeObject(native_scanout.pci);
    if (native_scanout.attr)
        OOP_ReleaseAttrBase((STRPTR)IID_Hidd_PCIDevice);
}

static BOOL native_scanout_open(void)
{
    struct NativeScanoutSample *s = &native_scanout;
    struct Hook hook = { 0 };
    IPTR bar0 = 0, size0 = 0, bar5 = 0, size5 = 0, driver = 0;
    struct TagItem reqs[] = {
        { tHidd_PCI_VendorID, 0x1002 }, { tHidd_PCI_Class, 3 }, { TAG_DONE, 0 }
    };
    struct pHidd_PCI_EnumDevices enumerate = {
        .mID = OOP_GetMethodID(IID_Hidd_PCI, moHidd_PCI_EnumDevices),
        .callback = &hook, .requirements = reqs
    };
    struct pHidd_PCIDevice_ReadConfigWord command = {
        .mID = OOP_GetMethodID(IID_Hidd_PCIDevice, moHidd_PCIDevice_ReadConfigWord),
        .reg = PCICS_COMMAND
    };
    s->attr = OOP_ObtainAttrBase((STRPTR)IID_Hidd_PCIDevice);
    if (!s->attr) return FALSE;
    s->pci = OOP_NewObject(NULL, CLID_Hidd_PCI, NULL);
    if (!s->pci) return FALSE;
    hook.h_Entry = (HOOKFUNC)native_scanout_find;
    hook.h_Data = s;
    OOP_DoMethod(s->pci, (OOP_Msg)&enumerate);
    if (s->matches != 1) return FALSE;
    if (!(OOP_DoMethod(s->device, (OOP_Msg)&command) & PCICMF_MEMDECODE))
        return FALSE;
    OOP_GetAttr(s->device, s->attr + aoHidd_PCIDevice_Base0, &bar0);
    OOP_GetAttr(s->device, s->attr + aoHidd_PCIDevice_Size0, &size0);
    OOP_GetAttr(s->device, s->attr + aoHidd_PCIDevice_Base5, &bar5);
    OOP_GetAttr(s->device, s->attr + aoHidd_PCIDevice_Size5, &size5);
    OOP_GetAttr(s->device, s->attr + aoHidd_PCIDevice_Driver, &driver);
    if (bar0 != 0x7000000000ULL || size0 != 0x400000000ULL ||
        bar5 != 0xfc800000UL || size5 != 0x80000UL || !driver)
        return FALSE;
    s->driver = (OOP_Object *)driver;
    s->mmio = HIDD_PCIDriver_PCItoCPU(s->driver, (APTR)bar5);
    if ((IPTR)s->mmio != bar5) return FALSE;
    return TRUE;
}

static BOOL native_scanout_observe(const char *stage, const char *when)
{
    struct NativeScanoutSample *s = &native_scanout;
    /* These are absolute dword indices: captured DMU2=34c0 plus Linux offsets.
     * Require linear ARGB8888, VMID0, full1080p viewport, origin0, pitch1920.
     */
    static const ULONG indices[] = {
        0x3ad3, 0x3add, 0x3adc, 0x3add, 0x3aa5, 0x3aa7,
        0x3ac9, 0x3ad2, 0x3ac7, 0x3aa9, 0x3aab, 0x5049
    };
    ULONG v[12], low, high, flip, upper, lower, upper_bg, lower_bg, i;
    UQUAD mc, off;
    volatile const ULONG *pixels;
    if (s->failed) return FALSE;
    for (i = 0; i < 12; ++i) {
        v[i] = s->mmio[indices[i]];
        if (v[i] == 0xffffffffUL) goto fail;
    }
    if ((v[0] & 0x301UL) || v[1] != v[3] || (v[1] & ~0xffffUL) ||
        (v[4] & 0x7f) != 8 || (v[5] & 7) || (v[6] & 15) ||
        (v[7] & 3) || (v[8] & 0xffff) != 1919 || v[9] ||
        (v[10] & 0x3fff3fffUL) != 0x04380780UL || (v[11] & 0x101UL))
        goto fail;
    mc = ((UQUAD)v[1] << 32) | v[2];
    /* Current plane must be wholly within this driver's captured64MiB pool. */
    if (mc < 0x8001100000ULL || mc > 0x8005100000ULL - 8294400 || (mc & 255))
        goto fail;
    off = mc - 0x8000000000ULL;
    pixels = HIDD_PCIDriver_PCItoCPU(s->driver, (APTR)(IPTR)(0x7000000000ULL + off));
    if ((UQUAD)(IPTR)pixels != 0x7000000000ULL + off) goto fail;
    upper = pixels[32 * 1920 + 300];
    lower = pixels[224 * 1920 + 300];
    upper_bg = pixels[32 * 1920 + 512];
    lower_bg = pixels[224 * 1920 + 512];
    low = s->mmio[0x3adc];
    high = s->mmio[0x3add];
    flip = s->mmio[0x3ad3];
    if (low != v[2] || high != v[1] || flip == 0xffffffffUL || (flip & 0x301UL))
        goto fail;
    LOG("SCANOUT %s %s mc=%08lx:%08lx upper=%08lx lower=%08lx backgrounds=%08lx/%08lx; CPU aperture only, HDMI required",
        stage, when, (unsigned long)(mc >> 32), (unsigned long)(ULONG)mc,
        (unsigned long)upper, (unsigned long)lower,
        (unsigned long)upper_bg, (unsigned long)lower_bg);
    return TRUE;
fail:
    s->failed = TRUE;
    LOG("SCANOUT %s %s unavailable/unstable; stop observer reads, no repair", stage, when);
    return FALSE;
}
