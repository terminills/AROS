/* Actual driver publication methods. Fake OOP callbacks assert lock order,
   successful-latch gating and hidden-cursor target changes. No GUI/GPU access. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int BOOL;
typedef void VOID;
typedef uintptr_t IPTR;
typedef unsigned OOP_MethodID;
typedef void *OOP_Msg;
#define TRUE 1
#define FALSE 0
#define TAG_DONE 0
#define aHidd_BitMap_Visible 1
#define aHidd_BitMap_Display 2
#define IID_Hidd_Display 3
enum { moHidd_Display_Show = 10, moHidd_Display_SetCursorVisible,
       moHidd_Display_SetCursorShape, moHidd_Display_SetCursorPos };
typedef struct Object { struct Object *display; BOOL visible; } OOP_Object;
struct SignalSemaphore { unsigned held; };
struct HWData {
    struct SignalSemaphore display_lock;
    BOOL pool_lock_ready, plane_faulted, cursor_requested_visible;
    unsigned scanout_active_size;
};
typedef struct Class OOP_Class;
struct AmdgpuGfx_staticdata {
    struct HWData data;
    OOP_Object *display, *visible;
    OOP_Class *displayclass;
};
struct Class { struct AmdgpuGfx_staticdata *sd; };
#define XSD(cl) ((cl)->sd)
struct TagItem { IPTR ti_Tag, ti_Data; };
struct pHidd_Display_Show { OOP_MethodID mID; OOP_Object *bitMap; unsigned flags; };
struct pHidd_Display_SetCursorVisible { OOP_MethodID mID; BOOL visible; };
struct pHidd_Display_SetCursorShape { OOP_MethodID mID; };
struct pHidd_Display_SetCursorPos { OOP_MethodID mID; };
static struct AmdgpuGfx_staticdata sd;
static OOP_Class cls = { &sd };
static OOP_Object display, oldbm, newbm, foreign;
static unsigned fblock, lock_count, unlock_count, plane_calls, attrs, shows, clears;
static BOOL select_ok, fault_on_select, cursor_visible;
static OOP_Object *cursor_target;
static void ObtainSemaphore(struct SignalSemaphore *sem)
{ assert(sem == &sd.data.display_lock && !fblock); ++sem->held; ++lock_count; }
static void ReleaseSemaphore(struct SignalSemaphore *sem)
{ assert(sem == &sd.data.display_lock && sem->held && !fblock); --sem->held; ++unlock_count; }
static void lock_fb(struct AmdgpuGfx_staticdata *s)
{ assert(s == &sd && s->data.display_lock.held && !fblock); fblock = 1; }
static void unlock_fb(struct AmdgpuGfx_staticdata *s)
{ assert(s == &sd && fblock); fblock = 0; }
#define LOCK_FRAMEBUFFER(s) lock_fb(s)
#define UNLOCK_FRAMEBUFFER(s) unlock_fb(s)
static IPTR OOP_GET(OOP_Object *bm, unsigned attr)
{ assert(attr == aHidd_BitMap_Display); return (IPTR)bm->display; }
static OOP_MethodID OOP_GetMethodID(unsigned iid, unsigned method)
{ assert(iid == IID_Hidd_Display); return method; }
static void OOP_SetAttrs(OOP_Object *bm, struct TagItem *tags)
{
    assert(!fblock && sd.data.display_lock.held && !cursor_visible);
    assert(tags[0].ti_Tag == aHidd_BitMap_Visible && tags[1].ti_Tag == TAG_DONE);
    bm->visible = tags[0].ti_Data; ++attrs;
    /* Model current CursorFB callbacks, not just the unwrapped bitmap. */
    if (bm->visible) cursor_target = bm;
    else if (cursor_target == bm) cursor_target = NULL;
}
static IPTR OOP_DoSuperMethod(OOP_Class *cl, OOP_Object *o, OOP_Msg message)
{
    OOP_MethodID method = *(OOP_MethodID *)message;
    assert(cl == &cls && o == &display && !fblock && sd.data.display_lock.held);
    if (method == moHidd_Display_SetCursorVisible) {
        cursor_visible = ((struct pHidd_Display_SetCursorVisible *)message)->visible;
        assert(!cursor_visible || (cursor_target && !sd.data.plane_faulted));
    } else if (method == moHidd_Display_Show) {
        OOP_Object *bm = ((struct pHidd_Display_Show *)message)->bitMap;
        assert(!cursor_visible);
        assert(!bm || (bm == &newbm && bm->visible && select_ok));
        cursor_target = bm; ++shows;
        return (IPTR)bm;
    } else {
        assert(method == moHidd_Display_SetCursorShape || method == moHidd_Display_SetCursorPos);
    }
    return TRUE;
}
static BOOL amdgpu_select_plane(OOP_Class *cl, OOP_Object *bm)
{
    assert(cl == &cls && bm == &newbm && !fblock && sd.data.display_lock.held);
    assert(!cursor_visible && sd.visible == &oldbm && oldbm.visible && !newbm.visible);
    ++plane_calls;
    if (fault_on_select) sd.data.plane_faulted = TRUE;
    if (select_ok) sd.data.scanout_active_size = 8192;
    return select_ok;
}
static void ClearBuffer(struct HWData *hw)
{
    assert(hw == &sd.data && fblock && !sd.visible && !cursor_target && !cursor_visible);
    if (!hw->plane_faulted) ++clears;
}
#include "display_publish.inc"
static void reset(void)
{
    memset(&sd, 0, sizeof(sd));
    memset(&newbm, 0, sizeof(newbm));
    oldbm.display = newbm.display = &display; oldbm.visible = TRUE;
    sd.display = &display; sd.displayclass = &cls; sd.visible = &oldbm;
    sd.data.pool_lock_ready = TRUE; sd.data.cursor_requested_visible = TRUE;
    sd.data.scanout_active_size = 4096;
    cursor_target = &oldbm; cursor_visible = select_ok = TRUE; fault_on_select = FALSE;
    fblock = lock_count = unlock_count = plane_calls = attrs = shows = clears = 0;
}
static void balanced(void)
{ assert(!fblock && !sd.data.display_lock.held && lock_count == unlock_count); }
int main(void)
{
    struct pHidd_Display_Show show = { moHidd_Display_Show, &newbm, 0 };
    struct pHidd_Display_SetCursorVisible vis = { moHidd_Display_SetCursorVisible, TRUE };
    struct pHidd_Display_SetCursorShape shape = { moHidd_Display_SetCursorShape };
    struct pHidd_Display_SetCursorPos pos = { moHidd_Display_SetCursorPos };
    reset(); assert(amdgpu_compositor_switch_mode(&cls, &newbm));
    assert(sd.visible == &newbm && !oldbm.visible && newbm.visible);
    assert(cursor_visible && cursor_target == &newbm && plane_calls == 1 && attrs == 2 && shows == 1);
    balanced();
    reset(); assert(AmdgpuGfxDisplay__Hidd_Display__Show(&cls, &display, &show) == &newbm); balanced();
    reset(); select_ok = FALSE;
    assert(AmdgpuGfxDisplay__Hidd_Display__Show(&cls, &display, &show) == &oldbm);
    assert(!attrs && !shows && cursor_target == &oldbm && cursor_visible); balanced();
    reset(); select_ok = FALSE; fault_on_select = TRUE;
    assert(!amdgpu_compositor_switch_mode(&cls, &newbm));
    assert(sd.visible == &oldbm && !attrs && !shows && !cursor_visible); balanced();
    AmdgpuGfxDisplay__Hidd_Display__SetCursorVisible(&cls, &display, &vis);
    assert(sd.data.cursor_requested_visible && !cursor_visible); balanced();
    reset(); show.bitMap = NULL;
    assert(!AmdgpuGfxDisplay__Hidd_Display__Show(&cls, &display, &show));
    assert(!oldbm.visible && !sd.visible && !cursor_target && !cursor_visible && clears == 1);
    assert(sd.data.scanout_active_size == 4096); balanced();
    reset(); sd.data.scanout_active_size = 0;
    AmdgpuGfxDisplay__Hidd_Display__Show(&cls, &display, &show);
    assert(!clears); balanced();
    reset(); sd.data.pool_lock_ready = FALSE;
    assert(!amdgpu_compositor_switch_mode(&cls, &newbm));
    assert(!lock_count && !plane_calls && !attrs); balanced();
    reset(); newbm.display = &foreign;
    assert(!amdgpu_compositor_switch_mode(&cls, &newbm));
    assert(!lock_count && !plane_calls && !attrs); balanced();
    reset(); vis.visible = FALSE;
    AmdgpuGfxDisplay__Hidd_Display__SetCursorVisible(&cls, &display, &vis);
    assert(!sd.data.cursor_requested_visible && !cursor_visible);
    assert(AmdgpuGfxDisplay__Hidd_Display__SetCursorShape(&cls, &display, &shape));
    assert(AmdgpuGfxDisplay__Hidd_Display__SetCursorPos(&cls, &display, &pos));
    balanced();
    puts("PASS: actual driver Show/Visible/cursor publication, confirmed-latch gating, rollback retention, uncertain cursor suppression, detach and lock ordering");
    return 0;
}
