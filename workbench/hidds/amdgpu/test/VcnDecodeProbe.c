/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Decode one H.264 frame on VCN through amdgpu.hidd's public BO, VA and
    VCN submission calls, the way radeonsi's radeon_vcn_dec.c drives the
    unified queue with a DPB_MAX_RES decoder, and write the picture out.

        VcnDecodeProbe FILE=<annexb.h264> [OUT=<prefix>] [PLAY]

    Every picture in the file is decoded in order (I, P and B slices, frame
    pictures, one SPS/PPS); each is written as <prefix>-<n>.ppm, or with
    PLAY shown in display order in a window, looping until it is closed.
*/
#define DEBUG 1
#define __INLINE_AMDGPU_STACKCALL__ 1
#include <aros/debug.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/amdgpu.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>
#include <intuition/intuition.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "vcn_dec_msg.h"

struct Library *AmdgpuBase;

#define LOG(...) do { printf("[VcnDecode] " __VA_ARGS__); printf("\n"); \
                       bug("[VcnDecode] " __VA_ARGS__); bug("\n"); } while (0)

#define ALIGN(v, a)         (((v) + (a) - 1) & ~((a) - 1))
#define FB_BUFFER_OFFSET    0x2000
#define FB_BUFFER_SIZE      2048
#define IT_SCALING_SIZE     992
#define NUM_H264_REFS       17
#define DB_ALIGNMENT        64
#define DT_PITCH_ALIGN      256

/* ---- H.264 parameter sets ---------------------------------------------- */

struct Bits
{
    const UBYTE *p;
    ULONG size, pos;
};

static ULONG bits_u(struct Bits *b, ULONG n)
{
    ULONG v = 0;

    while (n--) {
        ULONG bit = 0;

        if (b->pos < b->size * 8)
            bit = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
        b->pos++;
        v = (v << 1) | bit;
    }
    return v;
}

static ULONG bits_ue(struct Bits *b)
{
    ULONG zeros = 0;

    while (!bits_u(b, 1) && zeros < 32)
        zeros++;
    return ((1UL << zeros) - 1) + bits_u(b, zeros);
}

static LONG bits_se(struct Bits *b)
{
    ULONG v = bits_ue(b);

    return (v & 1) ? (LONG)((v + 1) / 2) : -(LONG)(v / 2);
}

static BOOL bits_more_rbsp(struct Bits *b)
{
    ULONG last = b->size * 8;

    while (last > 0 && !((b->p[(last - 1) >> 3] >> (7 - ((last - 1) & 7))) & 1))
        last--;
    return b->pos + 1 < last;
}

static ULONG unescape(const UBYTE *in, ULONG n, UBYTE *out)
{
    ULONG i, o = 0, zeros = 0;

    for (i = 0; i < n; i++) {
        if (zeros >= 2 && in[i] == 3) {
            zeros = 0;
            continue;
        }
        out[o++] = in[i];
        zeros = in[i] ? 0 : zeros + 1;
    }
    return o;
}

struct Sps
{
    ULONG profile_idc, level_idc, chroma_format_idc, bit_depth_luma_minus8,
          bit_depth_chroma_minus8, log2_max_frame_num_minus4, pic_order_cnt_type,
          log2_max_poc_lsb_minus4, delta_pic_order_always_zero, max_num_ref_frames,
          gaps_allowed, width_mbs, height_map_units, frame_mbs_only, mb_adaptive,
          direct_8x8;
};

struct Pps
{
    ULONG entropy, bottom_field_poc, num_slice_groups_minus1, slice_group_map_type,
          num_ref_idx_l0_minus1, num_ref_idx_l1_minus1, weighted_pred, weighted_bipred,
          deblocking_ctrl, constrained_intra, redundant_pic_cnt, transform_8x8;
    LONG pic_init_qp_minus26, pic_init_qs_minus26, chroma_qp_index_offset,
         second_chroma_qp_index_offset;
};

static BOOL parse_sps(struct Bits *b, struct Sps *s)
{
    ULONG i;

    memset(s, 0, sizeof(*s));
    s->profile_idc = bits_u(b, 8);
    bits_u(b, 8);
    s->level_idc = bits_u(b, 8);
    bits_ue(b);
    s->chroma_format_idc = 1;
    if (s->profile_idc == 100 || s->profile_idc == 110 || s->profile_idc == 122 ||
        s->profile_idc == 244 || s->profile_idc == 44 || s->profile_idc == 83 ||
        s->profile_idc == 86 || s->profile_idc == 118 || s->profile_idc == 128) {
        s->chroma_format_idc = bits_ue(b);
        if (s->chroma_format_idc == 3)
            bits_u(b, 1);
        s->bit_depth_luma_minus8 = bits_ue(b);
        s->bit_depth_chroma_minus8 = bits_ue(b);
        bits_u(b, 1);
        if (bits_u(b, 1)) {
            LOG("SPS scaling matrices are not supported by this probe");
            return FALSE;
        }
    }
    s->log2_max_frame_num_minus4 = bits_ue(b);
    s->pic_order_cnt_type = bits_ue(b);
    if (s->pic_order_cnt_type == 0)
        s->log2_max_poc_lsb_minus4 = bits_ue(b);
    else if (s->pic_order_cnt_type == 1) {
        ULONG n;

        s->delta_pic_order_always_zero = bits_u(b, 1);
        bits_se(b);
        bits_se(b);
        n = bits_ue(b);
        for (i = 0; i < n; i++)
            bits_se(b);
    }
    s->max_num_ref_frames = bits_ue(b);
    s->gaps_allowed = bits_u(b, 1);
    s->width_mbs = bits_ue(b) + 1;
    s->height_map_units = bits_ue(b) + 1;
    s->frame_mbs_only = bits_u(b, 1);
    if (!s->frame_mbs_only)
        s->mb_adaptive = bits_u(b, 1);
    s->direct_8x8 = bits_u(b, 1);
    return TRUE;
}

static BOOL parse_pps(struct Bits *b, struct Pps *p)
{
    memset(p, 0, sizeof(*p));
    bits_ue(b);
    bits_ue(b);
    p->entropy = bits_u(b, 1);
    p->bottom_field_poc = bits_u(b, 1);
    p->num_slice_groups_minus1 = bits_ue(b);
    if (p->num_slice_groups_minus1) {
        LOG("slice groups are not supported by this probe");
        return FALSE;
    }
    p->num_ref_idx_l0_minus1 = bits_ue(b);
    p->num_ref_idx_l1_minus1 = bits_ue(b);
    p->weighted_pred = bits_u(b, 1);
    p->weighted_bipred = bits_u(b, 2);
    p->pic_init_qp_minus26 = bits_se(b);
    p->pic_init_qs_minus26 = bits_se(b);
    p->chroma_qp_index_offset = bits_se(b);
    p->deblocking_ctrl = bits_u(b, 1);
    p->constrained_intra = bits_u(b, 1);
    p->redundant_pic_cnt = bits_u(b, 1);
    p->second_chroma_qp_index_offset = p->chroma_qp_index_offset;
    if (bits_more_rbsp(b)) {
        p->transform_8x8 = bits_u(b, 1);
        if (bits_u(b, 1)) {
            LOG("PPS scaling matrices are not supported by this probe");
            return FALSE;
        }
        p->second_chroma_qp_index_offset = bits_se(b);
    }
    return TRUE;
}

/* ---- GPU buffers -------------------------------------------------------- */

struct Buf
{
    ULONG bo, va, size;
    UQUAD addr;
    UBYTE *cpu;
};

static struct AmdgpuDevice *ad;
static BOOL quiet;

static BOOL buf_alloc(struct Buf *b, ULONG size)
{
    APTR cpu = NULL;

    memset(b, 0, sizeof(*b));
    b->size = ALIGN(size, 4096);
    if (amdgpu_bo_alloc(ad, b->size, 4096, AMDGPU_BO_DOMAIN_GTT, 0, &b->bo) ||
        amdgpu_bo_cpu_map(ad, b->bo, &cpu) ||
        amdgpu_va_range_alloc(ad, b->size, 4096, 0, &b->va, &b->addr) ||
        amdgpu_bo_va_bind(ad, b->bo, b->va)) {
        LOG("FAIL allocating a %lu byte buffer", (unsigned long)b->size);
        return FALSE;
    }
    b->cpu = cpu;
    memset(b->cpu, 0, b->size);
    return TRUE;
}

static void buf_free(struct Buf *b)
{
    if (b->va && b->bo)
        amdgpu_bo_va_unbind(ad, b->va);
    if (b->va)
        amdgpu_va_range_free(ad, b->va);
    if (b->cpu)
        amdgpu_bo_cpu_unmap(ad, b->bo);
    if (b->bo)
        amdgpu_bo_free(ad, b->bo);
    memset(b, 0, sizeof(*b));
}

static void buf_flush(struct Buf *b)
{
    CacheClearE(b->cpu, b->size, CACRF_ClearD);
}

/* ---- IB construction (radeon_vcn_dec.c send_cmd, unified queue) -------- */

struct Ib
{
    struct Buf buf;
    ULONG *dw, n, size_slot;
    rvcn_decode_buffer_t *db;
};

static void ib_begin(struct Ib *ib)
{
    rvcn_decode_ib_package_t *pkg;

    ib->dw = (ULONG *)ib->buf.cpu;
    memset(ib->dw, 0, 1024);
    ib->n = 0;
    ib->dw[ib->n++] = RADEON_VCN_ENGINE_INFO_SIZE;
    ib->dw[ib->n++] = RADEON_VCN_ENGINE_INFO;
    ib->dw[ib->n++] = RADEON_VCN_ENGINE_TYPE_DECODE;
    ib->size_slot = ib->n++;
    pkg = (rvcn_decode_ib_package_t *)&ib->dw[ib->n];
    pkg->package_size = sizeof(rvcn_decode_buffer_t) + sizeof(rvcn_decode_ib_package_t);
    pkg->package_type = RDECODE_IB_PARAM_DECODE_BUFFER;
    ib->n += 2;
    ib->db = (rvcn_decode_buffer_t *)&ib->dw[ib->n];
    ib->n += sizeof(rvcn_decode_buffer_t) / 4;
}

static BOOL ib_submit(struct Ib *ib, const char *what)
{
    ULONG fence = 0, done = 0, poll;
    LONG r;

    ib->dw[ib->size_slot] = (ib->n - ib->size_slot + 3) * 4;
    buf_flush(&ib->buf);
    r = amdgpu_vcn_submit_ib(ad, ib->buf.addr, ib->n, &fence);
    if (r) {
        LOG("FAIL %s: amdgpu_vcn_submit_ib returned %ld", what, (long)r);
        return FALSE;
    }
    for (poll = 0; poll < 20500 && !done; poll++) {
        if (amdgpu_vcn_query_fence(ad, fence, &done))
            break;
        if (!done && poll >= 20000)
            Delay(1);
    }
    if (!quiet || !done)
        LOG("%s: fence %lu %s after %lu polls", what, (unsigned long)fence,
            done ? "signalled" : "TIMED OUT", (unsigned long)poll);
    return done;
}

#define DB_SET(flag, lo, hi, a) do { ib.db->valid_buf_flag |= (flag); \
                                     ib.db->hi = (ULONG)((a) >> 32); \
                                     ib.db->lo = (ULONG)(a); } while (0)

#define MAX_SLICES   4096
#define MAX_PICTURES 1024

struct Slice
{
    ULONG start, end;
};

struct Picture
{
    ULONG first_slice, slices;
    ULONG nal_ref_idc, idr, slice_type, frame_num, poc_lsb, num_ref_idx_l0_minus1,
          num_ref_idx_l1_minus1;
    LONG poc;
};

struct Ref
{
    ULONG frame_num;
    LONG poc;
};

static UBYTE *stream, *rbsp;
static struct Slice slices[MAX_SLICES];
static struct Picture pics[MAX_PICTURES];
static ULONG nslices, npics;

static void parse_slice_header(const struct Sps *sps, const struct Pps *pps, ULONG start,
                               ULONG end, struct Picture *p)
{
    struct Bits b;
    ULONG n = unescape(stream + start + 1, end - start - 1, rbsp), type;

    b.p = rbsp;
    b.size = n;
    b.pos = 0;
    p->nal_ref_idc = (stream[start] >> 5) & 3;
    p->idr = (stream[start] & 0x1f) == 5;
    bits_ue(&b);
    type = bits_ue(&b) % 5;
    p->slice_type = type;
    bits_ue(&b);
    p->frame_num = bits_u(&b, sps->log2_max_frame_num_minus4 + 4);
    if (p->idr)
        bits_ue(&b);
    if (sps->pic_order_cnt_type == 0) {
        p->poc_lsb = bits_u(&b, sps->log2_max_poc_lsb_minus4 + 4);
        if (pps->bottom_field_poc)
            bits_se(&b);
    } else if (sps->pic_order_cnt_type == 1 && !sps->delta_pic_order_always_zero) {
        bits_se(&b);
        if (pps->bottom_field_poc)
            bits_se(&b);
    }
    if (pps->redundant_pic_cnt)
        bits_ue(&b);
    if (type == 1)
        bits_u(&b, 1);
    p->num_ref_idx_l0_minus1 = pps->num_ref_idx_l0_minus1;
    p->num_ref_idx_l1_minus1 = pps->num_ref_idx_l1_minus1;
    if (type == 0 || type == 1 || type == 3) {
        if (bits_u(&b, 1)) {
            p->num_ref_idx_l0_minus1 = bits_ue(&b);
            if (type == 1)
                p->num_ref_idx_l1_minus1 = bits_ue(&b);
        }
    }
}

/* Picture order count, H.264 8.2.1.1 (type 0) and 8.2.1.3 (type 2), frames. */
static void compute_poc(const struct Sps *sps)
{
    ULONG max_poc_lsb = 1UL << (sps->log2_max_poc_lsb_minus4 + 4);
    ULONG max_frame_num = 1UL << (sps->log2_max_frame_num_minus4 + 4);
    LONG prev_msb = 0, prev_lsb = 0, frame_num_offset = 0, prev_frame_num = 0;
    ULONG i;

    for (i = 0; i < npics; i++) {
        struct Picture *p = &pics[i];

        if (sps->pic_order_cnt_type == 0) {
            LONG msb;

            if (p->idr)
                prev_msb = prev_lsb = 0;
            if ((LONG)p->poc_lsb < prev_lsb && prev_lsb - (LONG)p->poc_lsb >= (LONG)max_poc_lsb / 2)
                msb = prev_msb + max_poc_lsb;
            else if ((LONG)p->poc_lsb > prev_lsb && (LONG)p->poc_lsb - prev_lsb > (LONG)max_poc_lsb / 2)
                msb = prev_msb - max_poc_lsb;
            else
                msb = prev_msb;
            p->poc = msb + p->poc_lsb;
            if (p->nal_ref_idc) {
                prev_msb = msb;
                prev_lsb = p->poc_lsb;
            }
        } else {
            if (p->idr)
                frame_num_offset = 0;
            else if (prev_frame_num > (LONG)p->frame_num)
                frame_num_offset += max_frame_num;
            p->poc = p->idr ? 0 : 2 * (frame_num_offset + p->frame_num) - (p->nal_ref_idc ? 0 : 1);
            prev_frame_num = p->frame_num;
        }
    }
}

static void nv12_to_rgb(UBYTE *rgb, const UBYTE *nv12, ULONG pitch, ULONG aligned_h,
                        ULONG width, ULONG height)
{
    ULONG x, y;

    for (y = 0; y < height; y++) {
        const UBYTE *yp = nv12 + y * pitch;
        const UBYTE *uv = nv12 + pitch * aligned_h + (y / 2) * pitch;
        UBYTE *row = rgb + y * width * 3;

        for (x = 0; x < width; x++) {
            LONG c = 298 * ((LONG)yp[x] - 16);
            LONG d = (LONG)uv[x & ~1] - 128, e = (LONG)uv[(x & ~1) + 1] - 128;
            LONG r = (c + 409 * e + 128) >> 8;
            LONG g = (c - 100 * d - 208 * e + 128) >> 8;
            LONG bl = (c + 516 * d + 128) >> 8;

            row[x * 3 + 0] = r < 0 ? 0 : r > 255 ? 255 : r;
            row[x * 3 + 1] = g < 0 ? 0 : g > 255 ? 255 : g;
            row[x * 3 + 2] = bl < 0 ? 0 : bl > 255 ? 255 : bl;
        }
    }
}

#define PLAY_SLOTS 18

struct Shown
{
    UBYTE *rgb;
    LONG poc;
    BOOL used;
};

static struct Shown shown[PLAY_SLOTS];
static struct Window *win;
static ULONG presented;

static void present_one(ULONG width, ULONG height)
{
    LONG best = -1;
    ULONG i;

    for (i = 0; i < PLAY_SLOTS; i++)
        if (shown[i].used && (best < 0 || shown[i].poc < shown[best].poc))
            best = i;
    if (best < 0)
        return;
    WritePixelArray(shown[best].rgb, 0, 0, width * 3, win->RPort, win->BorderLeft,
                    win->BorderTop, width, height, RECTFMT_RGB);
    shown[best].used = FALSE;
    presented++;
    Delay(2);
}

static ULONG queued(void)
{
    ULONG i, c = 0;

    for (i = 0; i < PLAY_SLOTS; i++)
        c += shown[i].used;
    return c;
}

static BOOL close_requested(void)
{
    struct IntuiMessage *m;
    BOOL quit = FALSE;

    while ((m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
        if (m->Class == IDCMP_CLOSEWINDOW)
            quit = TRUE;
        ReplyMsg((struct Message *)m);
    }
    return quit;
}

static void write_ppm(CONST_STRPTR name, const UBYTE *nv12, ULONG pitch, ULONG aligned_h,
                      ULONG width, ULONG height)
{
    UBYTE *row = AllocVec(width * 3, MEMF_ANY);
    BPTR out;
    ULONG x, y;
    char head[32];
    int hl;

    if (!row)
        return;
    if (!(out = Open(name, MODE_NEWFILE))) {
        FreeVec(row);
        return;
    }
    hl = sprintf(head, "P6\n%lu %lu\n255\n", (unsigned long)width, (unsigned long)height);
    Write(out, head, hl);
    for (y = 0; y < height; y++) {
        const UBYTE *yp = nv12 + y * pitch;
        const UBYTE *uv = nv12 + pitch * aligned_h + (y / 2) * pitch;

        for (x = 0; x < width; x++) {
            LONG c = 298 * ((LONG)yp[x] - 16);
            LONG d = (LONG)uv[x & ~1] - 128, e = (LONG)uv[(x & ~1) + 1] - 128;
            LONG r = (c + 409 * e + 128) >> 8;
            LONG g = (c - 100 * d - 208 * e + 128) >> 8;
            LONG bl = (c + 516 * d + 128) >> 8;

            row[x * 3 + 0] = r < 0 ? 0 : r > 255 ? 255 : r;
            row[x * 3 + 1] = g < 0 ? 0 : g > 255 ? 255 : g;
            row[x * 3 + 2] = bl < 0 ? 0 : bl > 255 ? 255 : bl;
        }
        Write(out, row, width * 3);
    }
    Close(out);
    FreeVec(row);
}

int main(void)
{
    struct Library *resident;
    struct RDArgs *rd;
    IPTR args[3] = { 0, 0, 0 };
    BOOL play;
    ULONG pass = 0;
    struct DateStamp t0, t1;
    CONST_STRPTR file, prefix;
    BPTR fh;
    LONG stream_len = 0;
    struct Sps sps;
    struct Pps pps;
    BOOL have_sps = FALSE, have_pps = FALSE;
    ULONG i, n, decoded = 0;
    ULONG width, height, fs_in_mb, refs, image_size, dpb_size, ctx_size;
    ULONG dt_pitch, dt_height, dt_size, stream_handle;
    struct Buf session = { 0 }, msgbuf = { 0 }, bs = { 0 }, dpb = { 0 }, ctx = { 0 }, dt = { 0 };
    struct Ib ib = { { 0 } };
    struct Ref reflist[16];
    ULONG nrefs = 0;
    rvcn_dec_message_header_t *hdr;
    int result = RETURN_FAIL;

    rd = ReadArgs((CONST_STRPTR)"FILE/A,OUT,PLAY/S", args, NULL);
    if (!rd) {
        printf("usage: VcnDecodeProbe FILE=<annexb.h264> [OUT=<prefix>] [PLAY]\n");
        return RETURN_FAIL;
    }
    file = (CONST_STRPTR)args[0];
    prefix = args[1] ? (CONST_STRPTR)args[1] : (CONST_STRPTR)"RAM:vcn-frame";
    play = args[2] != 0;
    quiet = play;

    Forbid();
    resident = (struct Library *)FindName(&SysBase->LibList, "amdgpu.hidd");
    Permit();
    if (!resident || !(AmdgpuBase = OpenLibrary("amdgpu.hidd", 0)) ||
        AmdgpuBase->lib_Revision < 15) {
        LOG("amdgpu.hidd 0.15 is not running");
        goto args;
    }
    if (amdgpu_get_device_count() < 1 || !(ad = amdgpu_get_device(0))) {
        LOG("no amdgpu device");
        goto lib;
    }

    if (!(fh = Open(file, MODE_OLDFILE))) {
        LOG("cannot open %s", file);
        goto lib;
    }
    Seek(fh, 0, OFFSET_END);
    stream_len = Seek(fh, 0, OFFSET_BEGINNING);
    stream = AllocVec(stream_len + 4, MEMF_ANY | MEMF_CLEAR);
    rbsp = AllocVec(stream_len + 4, MEMF_ANY);
    if (!stream || !rbsp || Read(fh, stream, stream_len) != stream_len) {
        Close(fh);
        LOG("cannot read %s", file);
        goto mem;
    }
    Close(fh);

    /* Walk Annex-B NAL units: parse the parameter sets, group slices into
       pictures (a slice with first_mb_in_slice 0 starts a new one). */
    for (i = 0; i + 3 < (ULONG)stream_len; ) {
        ULONG start, end;
        UBYTE type;
        struct Bits b;

        if (!(stream[i] == 0 && stream[i + 1] == 0 && stream[i + 2] == 1)) {
            i++;
            continue;
        }
        start = i + 3;
        for (end = start; end + 2 < (ULONG)stream_len; end++)
            if (stream[end] == 0 && stream[end + 1] == 0 &&
                (stream[end + 2] == 1 || (stream[end + 2] == 0 && end + 3 < (ULONG)stream_len &&
                                          stream[end + 3] == 1)))
                break;
        if (end + 2 >= (ULONG)stream_len)
            end = stream_len;
        type = stream[start] & 0x1f;
        n = unescape(stream + start + 1, end - start - 1, rbsp);
        b.p = rbsp;
        b.size = n;
        b.pos = 0;
        if (type == 7)
            have_sps = parse_sps(&b, &sps);
        else if (type == 8)
            have_pps = parse_pps(&b, &pps);
        else if ((type == 5 || type == 1) && have_sps && have_pps && nslices < MAX_SLICES) {
            if (bits_ue(&b) == 0) {
                if (npics == MAX_PICTURES)
                    break;
                pics[npics].first_slice = nslices;
                parse_slice_header(&sps, &pps, start, end, &pics[npics]);
                npics++;
            }
            if (npics) {
                slices[nslices].start = start;
                slices[nslices].end = end;
                nslices++;
                pics[npics - 1].slices++;
            }
        }
        i = end;
    }
    if (!have_sps || !have_pps || !npics) {
        LOG("FAIL: stream needs an SPS, a PPS and a picture (sps=%d pps=%d pictures=%lu)",
            have_sps, have_pps, (unsigned long)npics);
        goto mem;
    }
    if (!sps.frame_mbs_only || sps.chroma_format_idc != 1 || sps.bit_depth_luma_minus8) {
        LOG("FAIL: only 8-bit 4:2:0 progressive streams are supported here");
        goto mem;
    }
    compute_poc(&sps);

    width = sps.width_mbs * 16;
    height = sps.height_map_units * 16;
    LOG("stream: profile %lu level %lu %lux%lu %s, %lu pictures in %lu slices",
        (unsigned long)sps.profile_idc, (unsigned long)sps.level_idc,
        (unsigned long)width, (unsigned long)height, pps.entropy ? "CABAC" : "CAVLC",
        (unsigned long)npics, (unsigned long)nslices);

    /* Sizes as calc_dpb_size()/calc_ctx_size_h264_perf() compute them. */
    fs_in_mb = sps.width_mbs * ALIGN(sps.height_map_units, 2);
    refs = 184320 / fs_in_mb + 1;
    if (sps.level_idc == 30) refs = 8100 / fs_in_mb + 1;
    else if (sps.level_idc == 31) refs = 18000 / fs_in_mb + 1;
    else if (sps.level_idc == 32) refs = 20480 / fs_in_mb + 1;
    else if (sps.level_idc == 41) refs = 32768 / fs_in_mb + 1;
    else if (sps.level_idc == 42) refs = 34816 / fs_in_mb + 1;
    else if (sps.level_idc == 50) refs = 110400 / fs_in_mb + 1;
    if (refs > NUM_H264_REFS)
        refs = NUM_H264_REFS;
    if (refs < sps.max_num_ref_frames + 1)
        refs = sps.max_num_ref_frames + 1;
    image_size = ALIGN(width, DB_ALIGNMENT) * ALIGN(height, DB_ALIGNMENT);
    image_size = ALIGN(image_size + image_size / 2, 1024);
    dpb_size = image_size * refs;
    ctx_size = refs * ALIGN(fs_in_mb * 192, 256);

    dt_pitch = ALIGN(width, DT_PITCH_ALIGN);
    dt_height = ALIGN(height, 16);
    dt_size = dt_pitch * dt_height * 3 / 2;

    if (!buf_alloc(&bs, stream_len + 256 + 3 * nslices) ||
        !buf_alloc(&session, RDECODE_SESSION_CONTEXT_SIZE) ||
        !buf_alloc(&msgbuf, FB_BUFFER_OFFSET + FB_BUFFER_SIZE + IT_SCALING_SIZE) ||
        !buf_alloc(&dpb, dpb_size) || !buf_alloc(&ctx, ctx_size) ||
        !buf_alloc(&dt, dt_size) || !buf_alloc(&ib.buf, 4096))
        goto bufs;

    if (play) {
        for (i = 0; i < PLAY_SLOTS; i++)
            if (!(shown[i].rgb = AllocVec(width * height * 3, MEMF_ANY))) {
                LOG("FAIL: no memory for the frame queue");
                goto bufs;
            }
        win = OpenWindowTags(NULL, WA_Title, (IPTR)"VCN H.264", WA_InnerWidth, width,
                             WA_InnerHeight, height, WA_DragBar, TRUE, WA_DepthGadget, TRUE,
                             WA_CloseGadget, TRUE, WA_Activate, TRUE,
                             WA_IDCMP, IDCMP_CLOSEWINDOW, TAG_DONE);
        if (!win) {
            LOG("FAIL: cannot open the playback window");
            goto bufs;
        }
    }

    stream_handle = 0x41524f53 ^ (ULONG)(IPTR)FindTask(NULL);
    hdr = (rvcn_dec_message_header_t *)msgbuf.cpu;

    /* create */
    {
        rvcn_dec_message_create_t *create = (APTR)(msgbuf.cpu + sizeof(*hdr));

        hdr->header_size = sizeof(*hdr);
        hdr->total_size = sizeof(*hdr) + sizeof(*create);
        hdr->num_buffers = 1;
        hdr->msg_type = RDECODE_MSG_CREATE;
        hdr->stream_handle = stream_handle;
        hdr->index[0].message_id = RDECODE_MESSAGE_CREATE;
        hdr->index[0].offset = sizeof(*hdr);
        hdr->index[0].size = sizeof(*create);
        create->stream_type = RDECODE_CODEC_H264_PERF;
        create->width_in_samples = width;
        create->height_in_samples = height;
        buf_flush(&msgbuf);

        ib_begin(&ib);
        DB_SET(RDECODE_CMDBUF_FLAGS_SESSION_CONTEXT_BUFFER, session_contex_buffer_address_lo,
               session_contex_buffer_address_hi, session.addr);
        DB_SET(RDECODE_CMDBUF_FLAGS_MSG_BUFFER, msg_buffer_address_lo,
               msg_buffer_address_hi, msgbuf.addr);
        if (!ib_submit(&ib, "create"))
            goto bufs;
    }

again:
    DateStamp(&t0);
    presented = 0;
    nrefs = 0;
    for (n = 0; n < npics; n++) {
        struct Picture *p = &pics[n];
        rvcn_dec_message_index_t *index_codec;
        rvcn_dec_message_decode_t *decode;
        rvcn_dec_message_avc_t *avc;
        rvcn_dec_feedback_header_t *fb = (APTR)(msgbuf.cpu + FB_BUFFER_OFFSET);
        UBYTE *it = msgbuf.cpu + FB_BUFFER_OFFSET + FB_BUFFER_SIZE;
        ULONG sizes, offset_decode, offset_codec, bs_bytes = 0, touched = 0;
        char name[128];

        if (p->idr) {
            nrefs = 0;
            while (play && queued())
                present_one(width, height);
        }
        for (i = 0; i < p->slices; i++) {
            struct Slice *sl = &slices[p->first_slice + i];

            bs.cpu[bs_bytes++] = 0;
            bs.cpu[bs_bytes++] = 0;
            bs.cpu[bs_bytes++] = 1;
            memcpy(bs.cpu + bs_bytes, stream + sl->start, sl->end - sl->start);
            bs_bytes += sl->end - sl->start;
        }
        memset(bs.cpu + bs_bytes, 0, ALIGN(bs_bytes, 128) - bs_bytes);
        buf_flush(&bs);
        if (!play) {
            memset(dt.cpu, 0x80, dt_size);
            buf_flush(&dt);
        }

        memset(msgbuf.cpu, 0, msgbuf.size);
        sizes = sizeof(*hdr);
        index_codec = (APTR)(msgbuf.cpu + sizes);
        sizes += sizeof(*index_codec);
        offset_decode = sizes;
        decode = (APTR)(msgbuf.cpu + sizes);
        sizes += sizeof(*decode);
        offset_codec = sizes;
        avc = (APTR)(msgbuf.cpu + sizes);

        hdr->header_size = sizeof(*hdr);
        hdr->total_size = sizes + sizeof(*avc);
        hdr->msg_type = RDECODE_MSG_DECODE;
        hdr->stream_handle = stream_handle;
        hdr->status_report_feedback_number = n + 1;
        hdr->num_buffers = 2;
        hdr->index[0].message_id = RDECODE_MESSAGE_DECODE;
        hdr->index[0].offset = offset_decode;
        hdr->index[0].size = sizeof(*decode);
        index_codec->message_id = RDECODE_MESSAGE_AVC;
        index_codec->offset = offset_codec;
        index_codec->size = sizeof(*avc);

        decode->stream_type = RDECODE_CODEC_H264_PERF;
        decode->width_in_samples = width;
        decode->height_in_samples = height;
        decode->bsd_size = ALIGN(bs_bytes, 128);
        decode->dpb_size = dpb_size;
        decode->dt_size = dt.size;
        decode->hw_ctxt_size = ctx.size;
        decode->sw_ctxt_size = RDECODE_SESSION_CONTEXT_SIZE;
        decode->db_pitch = ALIGN(width, DB_ALIGNMENT);
        decode->db_swizzle_mode = RDECODE_VCN5_256B_D;
        decode->db_array_mode = RDECODE_ARRAY_MODE_ADDRLIB_SEL_GFX11;
        decode->dt_pitch = dt_pitch;
        decode->dt_uv_pitch = dt_pitch / 2;
        decode->dt_swizzle_mode = RDECODE_SW_MODE_LINEAR;
        decode->dt_array_mode = RDECODE_ARRAY_MODE_ADDRLIB_SEL_GFX11;
        decode->dt_chroma_top_offset = dt_pitch * dt_height;
        decode->dt_chroma_bottom_offset = dt_pitch * dt_height;
        decode->mif_wrc_en = 1;

        avc->profile = sps.profile_idc == 66 ? RDECODE_H264_PROFILE_BASELINE :
                       sps.profile_idc == 77 ? RDECODE_H264_PROFILE_MAIN :
                                               RDECODE_H264_PROFILE_HIGH;
        avc->level = sps.level_idc;
        avc->sps_info_flags =
            (sps.direct_8x8 << RDECODE_SPS_INFO_H264_DIRECT_8X8_INFERENCE_FLAG_SHIFT) |
            (sps.mb_adaptive << RDECODE_SPS_INFO_H264_MB_ADAPTIVE_FRAME_FIELD_FLAG_SHIFT) |
            (sps.frame_mbs_only << RDECODE_SPS_INFO_H264_FRAME_MBS_ONLY_FLAG_SHIFT) |
            (sps.delta_pic_order_always_zero << RDECODE_SPS_INFO_H264_DELTA_PIC_ORDER_ALWAYS_ZERO_FLAG_SHIFT) |
            (sps.gaps_allowed << RDECODE_SPS_INFO_H264_GAPS_IN_FRAME_NUM_VALUE_ALLOWED_FLAG_SHIFT) |
            (1 << RDECODE_SPS_INFO_H264_EXTENSION_SUPPORT_FLAG_SHIFT);
        avc->pps_info_flags = (pps.transform_8x8 << 0) | (pps.redundant_pic_cnt << 1) |
                              (pps.constrained_intra << 2) | (pps.deblocking_ctrl << 3) |
                              (pps.weighted_bipred << 4) | (pps.weighted_pred << 6) |
                              (pps.bottom_field_poc << 7) | (pps.entropy << 8);
        avc->chroma_format = sps.chroma_format_idc;
        avc->bit_depth_luma_minus8 = sps.bit_depth_luma_minus8;
        avc->bit_depth_chroma_minus8 = sps.bit_depth_chroma_minus8;
        avc->log2_max_frame_num_minus4 = sps.log2_max_frame_num_minus4;
        avc->pic_order_cnt_type = sps.pic_order_cnt_type;
        avc->log2_max_pic_order_cnt_lsb_minus4 = sps.log2_max_poc_lsb_minus4;
        avc->num_ref_frames = sps.max_num_ref_frames;
        avc->pic_init_qp_minus26 = pps.pic_init_qp_minus26;
        avc->pic_init_qs_minus26 = pps.pic_init_qs_minus26;
        avc->chroma_qp_index_offset = pps.chroma_qp_index_offset;
        avc->second_chroma_qp_index_offset = pps.second_chroma_qp_index_offset;
        avc->num_ref_idx_l0_active_minus1 = p->num_ref_idx_l0_minus1;
        avc->num_ref_idx_l1_active_minus1 = p->num_ref_idx_l1_minus1;
        memset(avc->scaling_list_4x4, 16, sizeof(avc->scaling_list_4x4));
        memset(avc->scaling_list_8x8, 16, sizeof(avc->scaling_list_8x8));
        memcpy(it, avc->scaling_list_4x4, 6 * 16);
        memcpy(it + 96, avc->scaling_list_8x8, 2 * 64);
        avc->frame_num = p->frame_num;
        avc->curr_field_order_cnt_list[0] = p->poc;
        avc->curr_field_order_cnt_list[1] = p->poc;
        for (i = 0; i < nrefs; i++) {
            avc->frame_num_list[i] = reflist[i].frame_num;
            avc->field_order_cnt_list[i][0] = reflist[i].poc;
            avc->field_order_cnt_list[i][1] = reflist[i].poc;
        }
        avc->decoded_pic_idx = p->frame_num;

        fb->header_size = sizeof(*fb);
        fb->total_size = sizeof(*fb);
        buf_flush(&msgbuf);

        ib_begin(&ib);
        DB_SET(RDECODE_CMDBUF_FLAGS_SESSION_CONTEXT_BUFFER, session_contex_buffer_address_lo,
               session_contex_buffer_address_hi, session.addr);
        DB_SET(RDECODE_CMDBUF_FLAGS_MSG_BUFFER, msg_buffer_address_lo,
               msg_buffer_address_hi, msgbuf.addr);
        DB_SET(RDECODE_CMDBUF_FLAGS_DPB_BUFFER, dpb_buffer_address_lo,
               dpb_buffer_address_hi, dpb.addr);
        DB_SET(RDECODE_CMDBUF_FLAGS_CONTEXT_BUFFER, context_buffer_address_lo,
               context_buffer_address_hi, ctx.addr);
        DB_SET(RDECODE_CMDBUF_FLAGS_BITSTREAM_BUFFER, bitstream_buffer_address_lo,
               bitstream_buffer_address_hi, bs.addr);
        DB_SET(RDECODE_CMDBUF_FLAGS_DECODING_TARGET_BUFFER, target_buffer_address_lo,
               target_buffer_address_hi, dt.addr);
        DB_SET(RDECODE_CMDBUF_FLAGS_FEEDBACK_BUFFER, feedback_buffer_address_lo,
               feedback_buffer_address_hi, msgbuf.addr + FB_BUFFER_OFFSET);
        DB_SET(RDECODE_CMDBUF_FLAGS_IT_SCALING_BUFFER, it_sclr_table_buffer_address_lo,
               it_sclr_table_buffer_address_hi, msgbuf.addr + FB_BUFFER_OFFSET + FB_BUFFER_SIZE);
        sprintf(name, "decode %lu (%s, frame_num %lu, poc %ld, %lu refs)", (unsigned long)n,
                p->idr ? "IDR" : p->slice_type == 2 ? "I" : p->slice_type == 0 ? "P" : "B",
                (unsigned long)p->frame_num, (long)p->poc, (unsigned long)nrefs);
        if (!ib_submit(&ib, name))
            break;

        CacheClearE(msgbuf.cpu, msgbuf.size, CACRF_InvalidateD);
        CacheClearE(dt.cpu, dt.size, CACRF_InvalidateD);
        if (play) {
            for (i = 0; i < PLAY_SLOTS && shown[i].used; i++)
                ;
            if (i < PLAY_SLOTS) {
                nv12_to_rgb(shown[i].rgb, dt.cpu, dt_pitch, dt_height, width, height);
                shown[i].poc = p->poc;
                shown[i].used = TRUE;
            }
            while (queued() > sps.max_num_ref_frames)
                present_one(width, height);
            decoded++;
            if (close_requested())
                break;
            goto refs;
        }
        for (i = 0; i < dt_pitch * height; i++)
            if (dt.cpu[i] != 0x80)
                touched++;
        LOG("picture %lu: feedback status %lu value %lu errorBits 0x%lx; %lu of %lu luma bytes written, centre Y=%u",
            (unsigned long)n, (unsigned long)fb->status, (unsigned long)fb->value,
            (unsigned long)fb->errorBits, (unsigned long)touched,
            (unsigned long)(dt_pitch * height), dt.cpu[(height / 2) * dt_pitch + width / 2]);
        sprintf(name, "%s-%lu.ppm", prefix, (unsigned long)n);
        write_ppm(name, dt.cpu, dt_pitch, dt_height, width, height);
        if (touched)
            decoded++;

refs:
        if (p->nal_ref_idc) {
            if (nrefs == sps.max_num_ref_frames && nrefs) {
                memmove(&reflist[0], &reflist[1], (nrefs - 1) * sizeof(reflist[0]));
                nrefs--;
            }
            if (nrefs < 16) {
                reflist[nrefs].frame_num = p->frame_num;
                reflist[nrefs].poc = p->poc;
                nrefs++;
            }
        }
    }
    if (play) {
        LONG ms;

        while (queued())
            present_one(width, height);
        DateStamp(&t1);
        ms = ((t1.ds_Days - t0.ds_Days) * 86400 * 50 + (t1.ds_Minute - t0.ds_Minute) * 3000 +
              (t1.ds_Tick - t0.ds_Tick)) * 20;
        LOG("pass %lu: %lu pictures decoded, %lu shown in %ld ms", (unsigned long)++pass,
            (unsigned long)n, (unsigned long)presented, (long)ms);
        if (n == npics && !close_requested())
            goto again;
        result = RETURN_OK;
    } else {
        LOG("%lu of %lu pictures produced output (written as %s-<n>.ppm)",
            (unsigned long)decoded, (unsigned long)npics, prefix);
        result = decoded == npics ? RETURN_OK : RETURN_WARN;
    }

    /* destroy */
    memset(msgbuf.cpu, 0, msgbuf.size);
    hdr->header_size = sizeof(*hdr);
    hdr->total_size = sizeof(*hdr);
    hdr->msg_type = RDECODE_MSG_DESTROY;
    hdr->stream_handle = stream_handle;
    buf_flush(&msgbuf);
    ib_begin(&ib);
    DB_SET(RDECODE_CMDBUF_FLAGS_SESSION_CONTEXT_BUFFER, session_contex_buffer_address_lo,
           session_contex_buffer_address_hi, session.addr);
    DB_SET(RDECODE_CMDBUF_FLAGS_MSG_BUFFER, msg_buffer_address_lo,
           msg_buffer_address_hi, msgbuf.addr);
    ib_submit(&ib, "destroy");

bufs:
    if (win)
        CloseWindow(win);
    for (i = 0; i < PLAY_SLOTS; i++)
        FreeVec(shown[i].rgb);
    buf_free(&ib.buf);
    buf_free(&dt);
    buf_free(&ctx);
    buf_free(&dpb);
    buf_free(&msgbuf);
    buf_free(&session);
    buf_free(&bs);
mem:
    FreeVec(rbsp);
    FreeVec(stream);
lib:
    CloseLibrary(AmdgpuBase);
args:
    FreeArgs(rd);
    return result;
}
