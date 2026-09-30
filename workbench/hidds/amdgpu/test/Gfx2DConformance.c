/*
 * Gfx2DConformance - self-verifying 2D conformance gate for amdgpu.
 *
 * Unlike Gfx2DProbe (a selective visual smoke test), this performs each 2D op
 * with KNOWN values and READS BACK the result to log PASS/FAIL on serial -- so
 * it verifies without eyes on the screen. It also renders labelled panels so a
 * ScreenGrabber capture corroborates the readback.
 *
 * Keystone = WritePixelArray/ReadPixelArray round-trip in RECTFMT_ARGB: literal
 * 0xAARRGGBB pixels through the PutImage/GetImage path, read back byte-exact.
 * That isolates pixel-format / byte-order (endian) bugs that pen-based tests
 * cannot (pens resolve unpredictably on truecolor). Alpha is masked off since
 * the scanout is XRGB.
 */
#define DEBUG 1
#include <aros/debug.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <graphics/rastport.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <string.h>
#include <stdio.h>

static ULONG g_pass = 0, g_fail = 0;

static void verdict(CONST_STRPTR name, BOOL ok, ULONG got, ULONG want)
{
    if (ok) g_pass++; else g_fail++;
    bug("[Gfx2DConf] %-26s %s got=0x%08lx want=0x%08lx\n",
        name, ok ? "PASS" : "FAIL", (unsigned long)got, (unsigned long)want);
}

static void label(struct RastPort *rp, WORD x, WORD y, CONST_STRPTR s)
{
    Move(rp, x, y);
    Text(rp, (STRPTR)s, (ULONG)strlen(s));
}

/* Test 1: ARGB pixel-array round-trip (the endian keystone). Distinct per-
 * channel values so a byte swap or dropped channel is unambiguous. */
static void test_pixelarray_argb(struct RastPort *rp, WORD ox, WORD oy)
{
    static const ULONG pat[4] = {
        0xFFEE4444UL,   /* R=EE G=44 B=44 (red)   */
        0xFF4488EEUL,   /* R=44 G=88 B=EE (blue)  */
        0xFF44EE88UL,   /* R=44 G=EE B=88 (green) */
        0xFFFFFFFFUL    /* white */
    };
    ULONG rb[4] = {0,0,0,0};
    WORD i;

    WritePixelArray((APTR)pat, 0, 0, 16, rp, ox, oy, 4, 1, RECTFMT_ARGB);
    ReadPixelArray((APTR)rb, 0, 0, 16, rp, ox, oy, 4, 1, RECTFMT_ARGB);
    /* ARGB32 ULONG is 0xBBGGRRAA -- alpha is the LOW byte. The framebuffer carries
     * no alpha, so a read normalizes it to opaque (0xFF); compare the RGB bytes
     * only by masking the alpha byte off (0xFFFFFF00), not the high byte. NOTE
     * this symmetric round-trip is a WEAK oracle (a self-inverse channel error
     * would still pass) -- the alpha matrix below is the authoritative check. */
    for (i = 0; i < 4; i++) {
        char nm[36];
        BOOL ok = ((rb[i] & 0xFFFFFF00UL) == (pat[i] & 0xFFFFFF00UL));
        snprintf(nm, sizeof(nm), "pixelarray ARGB[%d]", (int)i);
        verdict(nm, ok, rb[i], pat[i]);
    }
}

/* Test 2: raw single-pixel write/read (CyberGfx, no pen). */
static void test_writepixel_raw(struct RastPort *rp, WORD x, WORD y)
{
    static const ULONG pv[3] = { 0x00EE4444UL, 0x004488EEUL, 0x0044EE88UL };
    WORD i;
    for (i = 0; i < 3; i++) {
        ULONG got;
        char nm[32];
        WriteRGBPixel(rp, x + i, y, pv[i]);
        got = ReadRGBPixel(rp, x + i, y) & 0x00FFFFFFUL;
        snprintf(nm, sizeof(nm), "WriteRGBPixel[%d]", (int)i);
        verdict(nm, got == pv[i], got, pv[i]);
    }
}

/* Test 3: FillPixelArray solid fill (CyberGfx, known colour). */
static void test_fillpixelarray(struct RastPort *rp, WORD x, WORD y, WORD w, WORD h)
{
    ULONG want = 0x00EE4444UL, got;
    FillPixelArray(rp, x, y, w, h, want);
    got = ReadRGBPixel(rp, x + w/2, y + h/2) & 0x00FFFFFFUL;
    verdict("FillPixelArray centre", got == want, got, want);
}

/* Test 4: BltBitMap copy via ClipBlit (the CopyBox path). */
static void test_clipblit(struct RastPort *rp, WORD x, WORD y)
{
    ULONG want = 0x004488EEUL, got;
    FillPixelArray(rp, x, y, 16, 16, want);
    ClipBlit(rp, x, y, rp, x + 40, y, 16, 16, 0x0C0);
    got = ReadRGBPixel(rp, x + 40 + 8, y + 8) & 0x00FFFFFFUL;
    verdict("ClipBlit copy", got == want, got, want);
}

/* Alpha-blend matrix (codex TRIG-0529 / audit P0-2). Composites src RED over dst
 * BLUE at alpha {0,1,127,128,254,255} via WritePixelArrayAlpha -- the exact path
 * the window decorator uses for title-bar/menu images. This exercises the
 * ChunkyBM GetImage/PutImage ARGB32 conversion (the format-contract bug). Valid
 * channel oracle: green stays 0, red scales with alpha, blue scales inverse --
 * a symmetric round-trip could never catch the channel/alpha rotation. */
static void test_alpha_matrix(struct RastPort *rp, WORD ox, WORD oy)
{
    static const UBYTE alphas[6] = { 0, 1, 127, 128, 254, 255 };
    ULONG fg[16 * 16];
    WORD i, k;
    for (k = 0; k < 6; k++)
    {
        UBYTE A = alphas[k];
        LONG  want_r = ((LONG)0xFF * A) / 255;
        LONG  want_b = ((LONG)0xFF * (255 - A)) / 255;
        WORD  x = ox + k * 20;
        ULONG got; LONG gr, gg, gb; BOOL ok;
        char  nm[24];

        FillPixelArray(rp, x, oy, 16, 16, 0x000000FFUL);          /* dst = blue  */
        /* WritePixelArrayAlpha array is [A,R,G,B] memory = ULONG 0xBBGGRRAA
         * (alpha in the LOW byte). Red@A = R in byte1, alpha in byte0. */
        for (i = 0; i < 16 * 16; i++)
            fg[i] = 0x0000FF00UL | (ULONG)A;                      /* src = red@A */
        WritePixelArrayAlpha((APTR)fg, 0, 0, 16 * 4, rp, x, oy, 16, 16, 0xFFFFFFFFUL);

        got = ReadRGBPixel(rp, x + 8, oy + 8) & 0x00FFFFFFUL;
        gr = (got >> 16) & 0xff; gg = (got >> 8) & 0xff; gb = got & 0xff;
        ok = (gg <= 2) &&
             (gr >= want_r - 3 && gr <= want_r + 3) &&
             (gb >= want_b - 3 && gb <= want_b + 3);
        snprintf(nm, sizeof nm, "alpha A=%3u", (unsigned)A);
        verdict(nm, ok, got, ((ULONG)want_r << 16) | (ULONG)want_b);
    }
    SetAPen(rp, 1); label(rp, ox, oy - 10, "ALPHA MATRIX: red-over-blue A=0..255");
}

/* ---- amdgpu DMA PutImage/GetImage path (TRIG-0536 / TRIG-0537) -------------
 * The SDMA DMA upload/download fires for a >= 64 KiB image on a VRAM-resident
 * (displayable) bitmap. cybergfx WritePixelArray(RECTFMT_ARGB) issues an ARGB32
 * PutImage, which the driver now DMA-accelerates (byteswap into the cached
 * scratch, then SDMA to VRAM); ReadPixelArray issues an ARGB32 GetImage. A
 * simple-refresh window renders straight into the screen VRAM bitmap, so ONE
 * large WritePixelArray/ReadPixelArray round-trip exercises the path. Whether it
 * ACTUALLY ran (vs silent CPU fallback) is proven separately by the driver's
 * "[amdgpu-sdma] DMA upload/download" serial counters (TRIG-0537).
 *
 * CONVENTION (matches the endian keystone above): a readback normalizes the
 * alpha byte to opaque, and per that test alpha is the byte cleared by masking
 * 0xFFFFFF00. So carry the position-dependent pattern in the top 3 bytes and pin
 * the low (alpha) byte to 0xFF; compare masked 0xFFFFFF00. A wrong
 * stride/offset/byteswap corrupts the pattern bytes and fails. 176x176 = 121 KB
 * is safely over the 64 KiB gate. */
#define DMA_W     176
#define DMA_H     176
#define DMA_PIX(v)  ((((ULONG)(v) & 0x00FFFFFFUL) << 8) | 0x000000FFUL)
#define DMA_MASK    0xFFFFFF00UL

static void test_dma_roundtrip(struct RastPort *rp, WORD ox, WORD oy)
{
    ULONG *src = AllocVec((ULONG)DMA_W * DMA_H * 4, MEMF_ANY);
    ULONG *rb  = AllocVec((ULONG)DMA_W * DMA_H * 4, MEMF_CLEAR);
    ULONG x, y, bad = 0, firstbad = 0xFFFFFFFFUL, gv = 0, wv = 0;   /* ULONG is 32-bit here */

    if (!src || !rb) { bug("[Gfx2DConf] DMA roundtrip alloc failed\n");
                       if (src) FreeVec(src); if (rb) FreeVec(rb);
                       verdict("DMA-roundtrip-alloc", FALSE, 0, 1); return; }

    for (y = 0; y < DMA_H; y++)
        for (x = 0; x < DMA_W; x++)
            src[y * DMA_W + x] = DMA_PIX(((x & 0xFF) << 16) | ((y & 0xFF) << 8) | ((x ^ y) & 0xFF));

    /* one call -> one PutImage(DMA_W x DMA_H) >= gate -> SDMA upload */
    WritePixelArray((APTR)src, 0, 0, DMA_W * 4, rp, ox, oy, DMA_W, DMA_H, RECTFMT_ARGB);
    ReadPixelArray ((APTR)rb,  0, 0, DMA_W * 4, rp, ox, oy, DMA_W, DMA_H, RECTFMT_ARGB);

    for (y = 0; y < DMA_H; y++)
        for (x = 0; x < DMA_W; x++) {
            ULONG a = src[y * DMA_W + x] & DMA_MASK;
            ULONG b = rb [y * DMA_W + x] & DMA_MASK;
            if (a != b) { if (firstbad == 0xFFFFFFFFUL) { firstbad = y * DMA_W + x; gv = b; wv = a; } bad++; }
        }

    if (bad) bug("[Gfx2DConf] DMA-roundtrip first mismatch idx=%lu got=%08lx want=%08lx (%lu bad)\n",
                 (unsigned long)firstbad, (unsigned long)gv, (unsigned long)wv, (unsigned long)bad);
    verdict("DMA-roundtrip 176x176", bad == 0, (ULONG)bad, 0);

    FreeVec(src); FreeVec(rb);
    SetAPen(rp, 1); label(rp, ox, oy - 10, "DMA PutImage/GetImage 176x176 (SDMA >=64KiB)");
}

/* Concurrent stress for the shared-scratch race (codex TRIG-0536): N tasks each
 * hammer the DMA path on a DISJOINT screen region with a task-unique pattern,
 * looping. The regions never overlap, so the shared SDMA scratch is the resource
 * under contention -- if its lock were missing, one task would read another's
 * pixels back and mismatch. All tasks passing = scratch serialized. NOTE: this
 * is bounded by whatever locking cybergfx itself does around WritePixelArray; the
 * driver's DMA counters (TRIG-0537) confirm the transfers reached SDMA. */
struct DmaStress {
    struct RastPort *rp;
    WORD   ox, oy;
    ULONG  seed;
    ULONG  iters;
    volatile LONG *done;
    volatile LONG *fail;
    volatile LONG *stop;   /* cooperative early-stop (codex TRIG-0538) */
};

static void dma_stress_task(void)
{
    struct DmaStress *a = (struct DmaStress *)(FindTask(NULL)->tc_UserData);
    ULONG *src = AllocVec((ULONG)DMA_W * DMA_H * 4, MEMF_ANY);
    ULONG *rb  = AllocVec((ULONG)DMA_W * DMA_H * 4, MEMF_ANY);
    ULONG it, x, y;

    if (src && rb) {
        for (it = 0; it < a->iters; it++) {
            if (*a->stop) break;   /* parent asked us to wind down early */
            for (y = 0; y < DMA_H; y++)
                for (x = 0; x < DMA_W; x++)
                    src[y * DMA_W + x] =
                        DMA_PIX(a->seed ^ (x * 2654435761UL) ^ (y * 40503UL) ^ (it * 2246822519UL));
            WritePixelArray((APTR)src, 0, 0, DMA_W * 4, a->rp, a->ox, a->oy, DMA_W, DMA_H, RECTFMT_ARGB);
            ReadPixelArray ((APTR)rb,  0, 0, DMA_W * 4, a->rp, a->ox, a->oy, DMA_W, DMA_H, RECTFMT_ARGB);
            for (y = 0; y < DMA_H; y++)
                for (x = 0; x < DMA_W; x++)
                    if ((src[y * DMA_W + x] & DMA_MASK) != (rb[y * DMA_W + x] & DMA_MASK)) {
                        *a->fail = 1;
                        bug("[Gfx2DConf] DMA-STRESS seed=%08lx MISMATCH it=%lu (%lu,%lu) got=%08lx want=%08lx\n",
                            (unsigned long)a->seed, (unsigned long)it, (unsigned long)x, (unsigned long)y,
                            (unsigned long)(rb[y * DMA_W + x] & DMA_MASK),
                            (unsigned long)(src[y * DMA_W + x] & DMA_MASK));
                        goto stop;
                    }
        }
    } else *a->fail = 1;
stop:
    if (src) FreeVec(src);
    if (rb)  FreeVec(rb);
    Forbid(); (*a->done)++; Permit();
}

static void test_dma_concurrent(struct RastPort *rp, WORD ox, WORD oy)
{
    enum { N = 4 };
    /* STATIC so a task that outlives the wait cannot use-after-free the
     * args/counters (codex TRIG-0537). Called once. */
    static struct DmaStress arg[N];
    static volatile LONG done, fail, stop;
    WORD i, spawned = 0, waited;

    done = 0; fail = 0; stop = 0;
    for (i = 0; i < N; i++) {
        arg[i].rp = rp; arg[i].ox = ox + i * (DMA_W + 8); arg[i].oy = oy;
        arg[i].seed = 0xA5000000UL ^ ((ULONG)(i + 1) * 0x9E3779B1UL);
        arg[i].iters = 12; arg[i].done = &done; arg[i].fail = &fail; arg[i].stop = &stop;
        if (CreateNewProcTags(NP_Entry, (IPTR)dma_stress_task,
                              NP_Name, (IPTR)"dma-stress",
                              NP_UserData, (IPTR)&arg[i],
                              NP_StackSize, 32768, TAG_DONE) != NULL)
            spawned++;
        else { bug("[Gfx2DConf] DMA-concurrent: CreateNewProc %d FAILED\n", (int)i); fail = 1; }
    }
    /* Bounded DIAGNOSTIC wait (~30 s). If it overruns, ask workers to wind down
     * cooperatively -- but then JOIN unconditionally: workers hold this RPort and
     * run code from our segment, so we must NOT return/unload while any is alive
     * (codex TRIG-0538). A worker wedged inside WritePixelArray would hang here,
     * which correctly surfaces that driver hang rather than hiding it behind a
     * use-after-free. */
    for (waited = 0; done < spawned && waited < 300; waited++) Delay(5);
    if (done < spawned) {
        bug("[Gfx2DConf] DMA-concurrent slow: done=%ld/%d after 30s, signalling stop + joining\n",
            (long)done, (int)spawned);
        fail = 1;
        stop = 1;
        while (done < spawned) Delay(5);   /* mandatory join */
    }

    verdict("DMA-concurrent 4x12 disjoint", (fail == 0 && spawned == N), (ULONG)fail, 0);
    SetAPen(rp, 1); label(rp, ox, oy - 10, "DMA CONCURRENT: 4 tasks x 12, disjoint regions");
}

int main(void)
{
    struct Window *win;
    struct IntuiMessage *imsg;
    struct RastPort *rp;
    BOOL running = TRUE;

    bug("[Gfx2DConf] === amdgpu 2D CONFORMANCE gate starting ===\n");
    win = OpenWindowTags(NULL,
        WA_Left, 60, WA_Top, 60, WA_Width, 900, WA_Height, 560,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE,
        WA_Title, (IPTR)"amdgpu 2D conformance gate (any key / Ctrl-C / close to quit)", TAG_DONE);
    if (!win) { bug("[Gfx2DConf] OpenWindow failed\n"); return 20; }
    rp = win->RPort;

    SetAPen(rp, 0);
    RectFill(rp, 0, 0, win->Width - 1, win->Height - 1);
    SetAPen(rp, 1);
    label(rp, 16, 24, "amdgpu 2D conformance gate -- readback PASS/FAIL on serial");

    test_pixelarray_argb(rp, 20, 40);       /* endian keystone: WRITE+READ ARGB */
    test_writepixel_raw(rp, 20, 60);
    test_fillpixelarray(rp, 20, 80, 60, 40);
    test_clipblit(rp, 20, 140);
    test_alpha_matrix(rp, 20, 470);         /* decorator alpha path (P0-2) */
    test_dma_roundtrip(rp, 700, 200);       /* SDMA DMA PutImage/GetImage >=64KiB */
    test_dma_concurrent(rp, 10, 380);       /* scratch-race stress (TRIG-0536) */

    /* (Removed: WriteArray->rawRead / rawFill->ReadArray. They compared an ARGB32
     * ULONG against a CyberGraphics 0x00RRGGBB value as if the two conventions
     * were byte-identical -- the exact false contract this driver bug was; codex
     * flagged them as invalid colour oracles. The alpha matrix + WriteRGBPixel/
     * FillPixelArray/ClipBlit/red-PEN cover the same paths validly.) */

    /* DIAGNOSTIC: big boxes drawn via WritePixelArray (PutImage path) with pure
     * white / blue / red. PutImage writes byte-exact to VideoData (readback
     * proves it). So if WHITE shows yellow here, the SCANOUT drops blue at
     * display; if white shows white, the scanout is fine and any yellow is in
     * the pen/RectFill path only. */
    {
        /* CORRECT ARGB32 values (A8R8G8B8 in memory): red = A=FF,R=FF,G=0,B=0
         * which as an LE ULONG is 0x0000FFFF (NOT 0xFFFF0000 — that is cyan). */
        ULONG rowW[160], rowB[160], rowR[160];
        WORD i, yy;
        for (i = 0; i < 160; i++) { rowW[i] = 0xFFFFFFFFUL; rowB[i] = 0xFF0000FFUL; rowR[i] = 0x0000FFFFUL; }
        for (yy = 0; yy < 50; yy++) {
            WritePixelArray((APTR)rowW, 0, 0, 0, rp, 300, 160 + yy, 160, 1, RECTFMT_ARGB);
            WritePixelArray((APTR)rowB, 0, 0, 0, rp, 480, 160 + yy, 160, 1, RECTFMT_ARGB);
            WritePixelArray((APTR)rowR, 0, 0, 0, rp, 660, 160 + yy, 160, 1, RECTFMT_ARGB);
        }
        SetAPen(rp, 1);
        label(rp, 300, 150, "PutImage ARGB (correct values): WHITE / BLUE / RED");
    }
    /* GENUINE RED via 3 independent paths — settle whether red ever renders red.
     * All should be pure red. */
    {
        WORD i, yy;
        FillPixelArray(rp, 300, 250, 160, 50, 0x00FF0000UL);   /* CyberGfx native red block */
        for (yy = 0; yy < 50; yy++)
            for (i = 0; i < 160; i++)
                WriteRGBPixel(rp, 480 + i, 250 + yy, 0x00FF0000UL); /* raw red block */
        SetAPen(rp, 1);
        label(rp, 300, 240, "GENUINE RED (0x00FF0000): FillPixelArray / WriteRGBPixel");
    }
    /* PEN-PATH red: obtain a red pen and RectFill with it — tests SetAPen->MapColor
     * ->pixel (the path the pen-4 box uses). Also log what the screen's palette
     * pen 4 actually is, to settle whether pen-4-black is a driver bug or just the
     * running palette. */
    {
        struct ColorMap *cm = win->WScreen ? win->WScreen->ViewPort.ColorMap : NULL;
        if (cm) {
            LONG rp4 = ObtainBestPenA(cm, 0xFFFFFFFFUL, 0, 0, NULL);  /* pure red */
            ULONG p4rgb[3] = {0,0,0};
            GetRGB32(cm, 4, 1, (ULONG *)p4rgb);
            bug("[Gfx2DConf] screen pen4 RGB = %08lx %08lx %08lx ; ObtainBestPen(red)=%ld\n",
                (unsigned long)p4rgb[0], (unsigned long)p4rgb[1], (unsigned long)p4rgb[2], (long)rp4);
            if (rp4 >= 0) {
                ULONG got;
                SetAPen(rp, rp4);
                RectFill(rp, 660, 250, 819, 299);           /* red-PEN RectFill */
                got = ReadRGBPixel(rp, 740, 275) & 0x00FFFFFFUL;
                verdict("red-PEN RectFill", got == 0x00FF0000UL, got, 0x00FF0000UL);
                ReleasePen(cm, rp4);
            }
        }
        SetAPen(rp, 1);
        label(rp, 660, 240, "RED via a PEN (ObtainBestPen)");
    }

    /* Visible pen panels for the ScreenGrabber capture. */
    SetAPen(rp, 2); RectFill(rp, 300, 60, 460, 120);
    SetAPen(rp, 1); label(rp, 306, 92, "pen 2");
    SetAPen(rp, 3); RectFill(rp, 480, 60, 640, 120);
    SetAPen(rp, 1); label(rp, 486, 92, "pen 3 blue?");
    SetAPen(rp, 4); RectFill(rp, 660, 60, 820, 120);
    SetAPen(rp, 1); label(rp, 666, 92, "pen 4 red?");

    bug("[Gfx2DConf] === SUMMARY: %lu PASS, %lu FAIL ===\n",
        (unsigned long)g_pass, (unsigned long)g_fail);
    label(rp, 16, 520,
        (g_fail == 0) ? "ALL READBACK TESTS PASSED (see serial)"
                      : "READBACK FAILURES -- see serial [Gfx2DConf] lines");

    {
        ULONG winsig = 1UL << win->UserPort->mp_SigBit;
        while (running) {
            ULONG sigs = Wait(winsig | SIGBREAKF_CTRL_C);
            if (sigs & SIGBREAKF_CTRL_C)   /* Break / Ctrl-C from the shell/netshell */
                running = FALSE;
            if (sigs & winsig) {
                while ((imsg = (struct IntuiMessage *)GetMsg(win->UserPort)) != NULL) {
                    if (imsg->Class == IDCMP_CLOSEWINDOW ||
                        imsg->Class == IDCMP_VANILLAKEY)   /* any key quits */
                        running = FALSE;
                    ReplyMsg((struct Message *)imsg);
                }
            }
        }
    }
    CloseWindow(win);
    bug("[Gfx2DConf] complete\n");
    return 0;
}
