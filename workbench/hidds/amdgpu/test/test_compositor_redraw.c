/* Actual production redraw entry points: pending incompatible geometry must
 * not touch the retained composite. Positive controls prove the harness draws
 * when enabled. This is not a raster correctness test. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uintptr_t IPTR;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef int16_t WORD;
typedef int BOOL;
typedef void VOID;
#define TRUE 1
#define FALSE 0
enum { aHidd_BitMap_TopEdge, aHidd_BitMap_LeftEdge,
       aHidd_BitMap_Width, aHidd_BitMap_Height };
typedef struct { IPTR attrs[4]; } OOP_Object;
struct _Rectangle { WORD MinX, MinY, MaxX, MaxY; };
struct StackBitMapNode {
    struct StackBitMapNode *next;
    OOP_Object *bm;
    struct _Rectangle screenvisiblerect;
    BOOL isscreenvisible;
};
struct List { struct StackBitMapNode *head; };
struct HIDDCompositorData {
    OOP_Object *screenbitmap, *compositedbitmap, *gfx, *gc;
    struct _Rectangle screenrect;
    struct List bitmapstack;
    BOOL display_update_failed;
};
#define ForeachNode(list, n) for ((n) = (list)->head; (n); (n) = (n)->next)
static unsigned queries, recalc, searches, copies, fills;
static void OOP_GetAttr(OOP_Object *o, unsigned attr, IPTR *value)
{
    assert(o && attr < 4); ++queries; *value = o->attrs[attr];
}
static BOOL AndRectRect(struct _Rectangle *a, struct _Rectangle *b,
                         struct _Rectangle *out)
{
    out->MinX = a->MinX > b->MinX ? a->MinX : b->MinX;
    out->MinY = a->MinY > b->MinY ? a->MinY : b->MinY;
    out->MaxX = a->MaxX < b->MaxX ? a->MaxX : b->MaxX;
    out->MaxY = a->MaxY < b->MaxY ? a->MaxY : b->MaxY;
    return out->MinX <= out->MaxX && out->MinY <= out->MaxY;
}
static struct StackBitMapNode *HIDDCompositorIsBitMapOnStack(
    struct HIDDCompositorData *s, OOP_Object *bm)
{
    ++searches;
    assert(s->bitmapstack.head && s->bitmapstack.head->bm == bm);
    return s->bitmapstack.head;
}
static void HIDDCompositorRecalculateVisibleRects(struct HIDDCompositorData *s)
{
    assert(!s->display_update_failed); ++recalc;
}
static void HIDD_Gfx_CopyBox(OOP_Object *gfx, OOP_Object *src,
    WORD sx, WORD sy, OOP_Object *dst, WORD dx, WORD dy, WORD w, WORD h, OOP_Object *gc)
{
    (void)gfx; (void)src; (void)sx; (void)sy; (void)dst;
    (void)dx; (void)dy; (void)w; (void)h; (void)gc;
    ++copies;
}
static void HIDD_BM_FillRect(OOP_Object *bm, OOP_Object *gc,
                             WORD x1, WORD y1, WORD x2, WORD y2)
{
    (void)bm; (void)gc; (void)x1; (void)y1; (void)x2; (void)y2;
    ++fills;
}
#include "compositor_redraw.inc"
int main(void)
{
    OOP_Object bm = { { 20, 0, 1920, 1080 } }, composite = { { 0 } };
    struct StackBitMapNode node = { NULL, &bm, { 0, 20, 1919, 1079 }, TRUE };
    struct HIDDCompositorData s = {
        &composite, &composite, NULL, NULL, {0, 0, 1919, 1079}, { &node }, TRUE
    };
    HIDDCompositorRedrawBitmap(&s, &bm, 0, 0, 1920, 1080);
    HIDDCompositorRedrawVisibleScreen(&s);
    assert(!queries && !recalc && !searches && !copies && !fills);
    s.display_update_failed = FALSE;
    HIDDCompositorRedrawBitmap(&s, &bm, 0, 0, 1920, 1080);
    assert(copies == 1 && searches == 1 && queries == 2);
    HIDDCompositorRedrawVisibleScreen(&s);
    assert(recalc == 1 && copies == 2 && fills == 1);
    puts("PASS: actual compositor redraw paths do zero queries/copies/fills for blocked geometry; enabled controls still draw");
    return 0;
}
