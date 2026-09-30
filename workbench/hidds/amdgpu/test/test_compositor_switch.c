/* Actual production toggle with fake allocation/render/switch/disposal.
 * Verifies pointer publication and owned-composite lifetime on rejection,
 * not hardware latching, stack reconstruction or caller-owned bitmap life. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uintptr_t IPTR;
typedef int32_t LONG;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
enum { aHidd_BitMap_TopEdge, aHidd_BitMap_Width, aHidd_BitMap_Height,
       aHidd_BitMap_Displayable, aHidd_BitMap_ModeID, TAG_DONE };
struct TagItem { IPTR ti_Tag, ti_Data; };
typedef struct { IPTR topedge; unsigned disposed; } OOP_Object;
typedef struct { void *basebm; } OOP_Class;
struct HIDDCompositorData {
    OOP_Object *compositedbitmap, *screenbitmap, *topbitmap;
    IPTR screenmodeid;
    struct { int16_t MinX, MinY, MaxX, MaxY; } screenrect;
    BOOL modeschanged, display_update_failed;
    OOP_Object *gfx, *display;
};
static OOP_Class gfx_class;
static OOP_Object old_composite, new_composite, old_top, new_top, gfx, display;
static struct HIDDCompositorData state;
static OOP_Object *expected_old_screen, *expected_old_composite, *expected_candidate;
static unsigned allocations, renders, switches, disposals;
static BOOL allocation_ok, switch_ok, switched;
#define OOP_OCLASS(o) (assert((o) == &gfx), &gfx_class)
#define XSD(cl) (cl)
static void nvlog(const char *fmt, ...) { (void)fmt; }
static void OOP_GetAttr(OOP_Object *o, IPTR attr, IPTR *value)
{
    assert(o == &new_top && attr == aHidd_BitMap_TopEdge);
    *value = o->topedge;
}
static OOP_Object *HIDD_Display_CreateObject(OOP_Object *d, void *base,
                                            const struct TagItem *tags)
{
    assert(d == &display && base == gfx_class.basebm);
    assert(state.screenbitmap == expected_old_screen);
    assert(state.compositedbitmap == expected_old_composite);
    assert(tags[0].ti_Tag == aHidd_BitMap_Width && tags[0].ti_Data == 1920);
    assert(tags[1].ti_Tag == aHidd_BitMap_Height && tags[1].ti_Data == 1080);
    assert(tags[2].ti_Tag == aHidd_BitMap_Displayable && tags[2].ti_Data == TRUE);
    assert(tags[3].ti_Tag == aHidd_BitMap_ModeID && tags[3].ti_Data == 0x110001);
    assert(tags[4].ti_Tag == TAG_DONE);
    assert(!allocations++ && !disposals);
    return allocation_ok ? &new_composite : NULL;
}
static void HIDDCompositorRedrawVisibleScreen(struct HIDDCompositorData *s)
{
    assert(s == &state && !s->display_update_failed);
    assert(s->screenbitmap == expected_candidate);
    assert(s->compositedbitmap == expected_candidate);
    assert(expected_candidate != expected_old_screen && !switches && !disposals);
    ++renders;
}
static BOOL amdgpu_compositor_switch_mode(OOP_Class *cl, OOP_Object *candidate)
{
    assert(cl == &gfx_class && candidate == expected_candidate && candidate);
    assert(state.screenbitmap == expected_old_screen);
    assert(state.compositedbitmap == expected_old_composite);
    assert(!disposals && !candidate->disposed);
    assert(!switches++);
    switched = switch_ok;
    return switch_ok;
}
static void OOP_DisposeObject(OOP_Object *o)
{
    assert(o == &old_composite || o == &new_composite);
    assert(o != state.screenbitmap && o != state.compositedbitmap);
    assert(!o->disposed++);
    if (o == &old_composite) {
        assert(switched && state.screenbitmap == expected_candidate);
    } else {
        assert(!switch_ok && !switched); /* never submitted candidate only */
    }
    ++disposals;
}
#include "compositor_toggle.inc"
static void reset(BOOL changed, LONG edge, BOOL has_composite, BOOL composite_on_screen)
{
    memset(&state, 0, sizeof(state));
    old_composite.disposed = new_composite.disposed = 0;
    gfx_class.basebm = &gfx_class;
    state.gfx = &gfx; state.display = &display;
    state.topbitmap = &new_top;
    new_top.topedge = (IPTR)(intptr_t)edge;
    state.screenrect.MaxX = 1919; state.screenrect.MaxY = 1079;
    state.screenmodeid = 0x110001;
    state.modeschanged = changed;
    expected_old_composite = has_composite ? &old_composite : NULL;
    expected_old_screen = composite_on_screen ? &old_composite : &old_top;
    state.screenbitmap = expected_old_screen;
    state.compositedbitmap = expected_old_composite;
    expected_candidate = edge > 0 ?
        (has_composite && !changed ? &old_composite : &new_composite) : &new_top;
    allocations = renders = switches = disposals = 0;
    allocation_ok = switch_ok = TRUE; switched = FALSE;
}
static void refused(void)
{
    assert(!HIDDCompositorToggleCompositing(&state));
    assert(state.screenbitmap == expected_old_screen);
    assert(state.compositedbitmap == expected_old_composite);
    assert(!old_composite.disposed && state.display_update_failed);
}
int main(void)
{
    reset(TRUE, 20, TRUE, TRUE); allocation_ok = FALSE;
    refused(); assert(allocations == 1 && !renders && !switches && !disposals);
    assert(state.modeschanged); /* retry must still replace incompatible composite */

    reset(TRUE, 20, TRUE, TRUE); switch_ok = FALSE;
    refused(); assert(allocations == 1 && renders == 1 && switches == 1);
    assert(disposals == 1 && new_composite.disposed && state.modeschanged);

    reset(TRUE, 0, TRUE, TRUE); switch_ok = FALSE;
    refused(); assert(!allocations && !renders && switches == 1 && !disposals);

    reset(FALSE, 20, TRUE, FALSE); switch_ok = FALSE;
    refused(); assert(!allocations && renders == 1 && switches == 1 && !disposals);

    reset(FALSE, 20, FALSE, FALSE); switch_ok = FALSE;
    refused(); assert(new_composite.disposed && !old_composite.disposed);

    reset(TRUE, 0, TRUE, TRUE); state.topbitmap = NULL;
    refused(); assert(!allocations && !switches && !disposals);

    reset(TRUE, 20, TRUE, TRUE);
    assert(HIDDCompositorToggleCompositing(&state));
    assert(allocations == 1 && renders == 1 && switches == 1);
    assert(old_composite.disposed && !new_composite.disposed);
    assert(state.screenbitmap == &new_composite && state.compositedbitmap == &new_composite);
    assert(!state.modeschanged && !state.display_update_failed);

    reset(TRUE, 0, TRUE, TRUE);
    assert(HIDDCompositorToggleCompositing(&state));
    assert(!allocations && !renders && switches == 1 && old_composite.disposed);
    assert(state.screenbitmap == &new_top && !state.compositedbitmap);

    reset(FALSE, 0, TRUE, TRUE);
    assert(HIDDCompositorToggleCompositing(&state));
    assert(switches == 1 && !disposals && state.compositedbitmap == &old_composite);

    reset(FALSE, -1, FALSE, FALSE); /* upward offset remains direct */
    assert(HIDDCompositorToggleCompositing(&state));
    assert(!allocations && !renders && switches == 1);

    reset(FALSE, 20, TRUE, TRUE); /* unchanged direct scanout needs no update */
    assert(HIDDCompositorToggleCompositing(&state));
    assert(!allocations && !renders && !switches && !disposals);

    reset(FALSE, 20, TRUE, TRUE); state.display_update_failed = TRUE;
    assert(HIDDCompositorToggleCompositing(&state)); /* retry even same pointer */
    assert(switches == 1 && !state.display_update_failed);

    reset(FALSE, 20, TRUE, TRUE); state.display_update_failed = TRUE; switch_ok = FALSE;
    refused(); assert(switches == 1 && !disposals);
    puts("PASS: actual compositor toggle retains old owned buffer on rejection, handles allocation failure, publishes after success and retries blocked updates");
    return 0;
}
