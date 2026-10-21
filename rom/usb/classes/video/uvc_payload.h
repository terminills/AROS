/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef UVC_PAYLOAD_H
#define UVC_PAYLOAD_H
#include <stddef.h>
#include <string.h>

/* A complete uncompressed frame must have one FID, no ERR payloads, an EOF,
 * and exactly the negotiated byte count. Never publish partial frames. */
struct uvc_payload {
    unsigned char *data;
    size_t capacity, used;
    unsigned int fid;
    int have_fid, damaged;
};
static int uvc_payload_feed(struct uvc_payload *s,
                           const unsigned char *p, size_t len)
{
    size_t n;
    unsigned int fid;
    if (len < 2 || p[0] < 2 || p[0] > len) {
        s->damaged = 1;
        return 0;
    }
    fid = p[1] & 1;
    if (!s->have_fid || fid != s->fid) {
        s->fid = fid;
        s->have_fid = 1;
        s->used = 0;
        s->damaged = 0;
    }
    if (p[1] & 0x40) s->damaged = 1;
    n = len - p[0];
    if (n > s->capacity - s->used) s->damaged = 1;
    if (!s->damaged) {
        memcpy(s->data + s->used, p + p[0], n);
        s->used += n;
    }
    if (p[1] & 2) {
        int complete = !s->damaged && s->used == s->capacity;
        /* Reject additional payloads with the same FID after EOF. */
        s->damaged = 1;
        return complete;
    }
    return 0;
}
static unsigned char uvc_clip(int v)
{
    return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}
static void uvc_yuy2_rgb(const unsigned char *in, unsigned char *out, size_t n)
{
    size_t i;
    for (i = 0; i < n; i += 4) {
        int y0 = in[i] - 16, u = in[i+1] - 128;
        int y1 = in[i+2] - 16, v = in[i+3] - 128;
        out[0] = uvc_clip((298*y0 + 409*v + 128) >> 8);
        out[1] = uvc_clip((298*y0 - 100*u - 208*v + 128) >> 8);
        out[2] = uvc_clip((298*y0 + 516*u + 128) >> 8);
        out[3] = uvc_clip((298*y1 + 409*v + 128) >> 8);
        out[4] = uvc_clip((298*y1 - 100*u - 208*v + 128) >> 8);
        out[5] = uvc_clip((298*y1 + 516*u + 128) >> 8);
        out += 6;
    }
}
#endif
