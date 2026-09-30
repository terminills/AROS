/* JOB-0084 native VRAM fragmentation discriminator.
 * Public AllocBitMap/FreeBitMap friends of the already cold-booted AMDGPU
 * 1080p screen. Never initializes/replaces a driver, selects a new scanout,
 * pokes hardware or touches another bitmap's pixels. Bounded to nine
 * 8294400-byte candidates and one double-height candidate; not OS RAM stress.
 * This diagnoses allocation residency, not display/modeset correctness.
 */
#define DEBUG 1
#include <aros/debug.h>
#include <exec/types.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>

#define LOG(...) do { bug("[AmdgpuVramReuseProbe] " __VA_ARGS__); bug("\n"); } while (0)
#define COUNT 9
#define ROW 7680
#define SMALL (ROW * 1080UL)
#define POOL_LOW 0x7001100000ULL
#define POOL_HIGH 0x7005100000ULL
struct Allocation { struct BitMap *bm; IPTR base; ULONG size, color; };
static struct Allocation blocks[COUNT], large;
static UBYTE pixels[32 * 32 * 4];

static BOOL overlap(const struct Allocation *a, const struct Allocation *b)
{
    if (!a->bm || !b->bm) return FALSE;
    return a->base <= b->base ? b->base - a->base < a->size :
                               a->base - b->base < b->size;
}
static BOOL resident(const struct Allocation *a)
{
    return a->base >= POOL_LOW && a->base < POOL_HIGH &&
           a->size <= POOL_HIGH - a->base && !(a->base & 255);
}
static BOOL make(struct Allocation *a, struct BitMap *friend, ULONG height)
{
    APTR address = NULL, handle;
    ULONG row = 0, bpp = 0, width = 0, got_height = 0;
    a->bm = AllocBitMap(1920, height, 24, BMF_REQUESTVMEM, friend);
    if (!a->bm) { LOG("allocation NULL height=%lu", (unsigned long)height); return FALSE; }
    handle = LockBitMapTags(a->bm, LBMI_BASEADDRESS, (IPTR)&address,
        LBMI_BYTESPERROW, (IPTR)&row, LBMI_BYTESPERPIX, (IPTR)&bpp,
        LBMI_WIDTH, (IPTR)&width, LBMI_HEIGHT, (IPTR)&got_height, TAG_DONE);
    if (!handle) { LOG("FAIL direct-access metadata"); return FALSE; }
    /* No graphics calls while locked, and no direct pixel writes at all. */
    a->base = (IPTR)address;
    UnLockBitMap(handle);
    if (!address || row != ROW || bpp != 4 || width != 1920 || got_height != height) {
        LOG("FAIL geometry row=%lu bpp=%lu size=%lux%lu",
            (unsigned long)row, (unsigned long)bpp, (unsigned long)width,
            (unsigned long)got_height);
        return FALSE;
    }
    a->size = row * height;
    LOG("allocated bitmap=%p base=%p size=%lu resident=%ld", a->bm,
        (APTR)a->base, (unsigned long)a->size, (long)resident(a));
    return TRUE;
}
static void discard(struct Allocation *a)
{
    if (a->bm) FreeBitMap(a->bm);
    a->bm = NULL;
}
static BOOL mark(struct Allocation *a, BOOL write)
{
    struct RastPort rp;
    ULONG i, bad = 0;
    InitRastPort(&rp);
    rp.BitMap = a->bm;
    if (write) FillPixelArray(&rp, 0, 0, 32, 32, a->color);
    if (ReadPixelArray(pixels, 0, 0, 32 * 4, &rp, 0, 0, 32, 32,
                       RECTFMT_ARGB) != 1024) return FALSE;
    for (i = 0; i < 1024; ++i) {
        const UBYTE *p = pixels + 4 * i;
        ULONG got = ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
        if (got != a->color) ++bad;
    }
    LOG("sentinel bitmap=%p want=%08lx pixels=1024 bad=%lu", a->bm,
        (unsigned long)a->color, (unsigned long)bad);
    return !bad;
}

int main(void)
{
    struct Screen *prior = LockPubScreen(NULL);
    ULONG n = 0, i, j, pair = COUNT;
    IPTR hole = 0;
    BOOL exhausted = FALSE, alias = FALSE, reuse = FALSE, ok = TRUE;
    LONG result = RETURN_FAIL;
    if (!prior) return result;
    LOG("BEGIN v1 prior mode=%08lx size=%dx%d; scanout is never changed",
        (unsigned long)GetVPModeID(&prior->ViewPort), prior->Width, prior->Height);
    if (GetVPModeID(&prior->ViewPort) != 0x00110000 ||
        prior->Width != 1920 || prior->Height != 1080 ||
        GetBitMapAttr(prior->RastPort.BitMap, BMA_DEPTH) != 24) goto out;
    for (n = 0; n < COUNT; ++n) {
        if (!make(&blocks[n], prior->RastPort.BitMap, 1080)) goto out;
        for (j = 0; j < n; ++j)
            if (overlap(&blocks[n], &blocks[j])) alias = TRUE;
        if (alias) { LOG("FAIL overlapping live allocations; no writes or ambiguous frees"); goto out; }
        if (!resident(&blocks[n])) {
            exhausted = TRUE;
            discard(&blocks[n]);
            break;
        }
        blocks[n].color = 0x00205080UL + n * 0x00110307UL;
        if (!mark(&blocks[n], TRUE)) { ok = FALSE; goto out; }
    }
    LOG("pressure bounded resident_count=%lu system_fallback=%ld",
        (unsigned long)n, (long)exhausted);
    if (!exhausted || n < 4) { LOG("INCOMPLETE insufficient bounded pressure"); goto out; }
    /* Require two physically adjacent interior allocations, not the bump tail.
     * Holding a later range distinguishes interior merging from tail reclaim. */
    for (i = 0; i + 2 < n; ++i) {
        if (blocks[i].base + SMALL != blocks[i + 1].base) continue;
        for (j = i + 2; j < n; ++j)
            if (blocks[j].base >= blocks[i + 1].base + SMALL) break;
        if (j < n) { pair = i; break; }
    }
    if (pair == COUNT) { LOG("INCOMPLETE no adjacent interior pair"); goto out; }
    hole = blocks[pair].base;
    LOG("release adjacent interior pair=%lu base=%p size=%lu; later allocation retained",
        (unsigned long)pair, (APTR)hole, (unsigned long)(2 * SMALL));
    discard(&blocks[pair]);
    discard(&blocks[pair + 1]);
    if (!make(&large, prior->RastPort.BitMap, 2160)) goto out;
    for (i = 0; i < n; ++i) if (overlap(&large, &blocks[i])) alias = TRUE;
    if (alias) { LOG("FAIL large overlaps live allocation; no writes or ambiguous frees"); goto out; }
    reuse = resident(&large) && large.base == hole;
    LOG("INTERIOR-REUSE %s expected=%p actual=%p resident=%ld",
        reuse ? "PASS" : "FAIL", (APTR)hole, (APTR)large.base, (long)resident(&large));
    large.color = 0x00eeaa33UL;
    if (!mark(&large, TRUE)) ok = FALSE;
    for (i = 0; i < n; ++i)
        if (blocks[i].bm && !mark(&blocks[i], FALSE)) ok = FALSE;
    result = ok ? (reuse ? RETURN_OK : RETURN_WARN) : RETURN_ERROR;
out:
    if (!alias) {
        discard(&large);
        for (i = COUNT; i > 0; --i) discard(&blocks[i - 1]);
        if (result == RETURN_OK || result == RETURN_WARN) {
            if (make(&large, prior->RastPort.BitMap, 1080)) {
                BOOL recovered = resident(&large);
                large.color = 0x0044bb66UL;
                if (!recovered || !mark(&large, TRUE)) result = RETURN_ERROR;
                LOG("AFTER-CLEANUP resident=%ld", (long)recovered);
            } else result = RETURN_ERROR;
            discard(&large);
        }
    }
    UnlockPubScreen(NULL, prior);
    LOG("END result=%ld alias=%ld; owned allocations %s, no scanout writes",
        (long)result, (long)alias, alias ? "retained for safety" : "released");
    return result;
}
