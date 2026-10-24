#include <assert.h>
#include <stdio.h>
#include "../uvc_payload.h"
int main(void)
{
    unsigned char out[8], rgb[12];
    struct uvc_payload s = { .data=out, .capacity=8 };
    const unsigned char a[] = {2,0,16,128,235,128};
    const unsigned char b[] = {2,2,16,128,235,128};
    const unsigned char bad[] = {9,1,0};
    const unsigned char err[] = {2,0x43,16,128,235,128};
    const unsigned char full[] = {2,3,16,128,235,128,16,128,235,128};
    assert(!uvc_payload_feed(&s,a,sizeof(a)));
    assert(uvc_payload_feed(&s,b,sizeof(b)));
    assert(!uvc_payload_feed(&s,b,sizeof(b)));
    uvc_yuy2_rgb(out,rgb,sizeof(out));
    assert(rgb[0]==0 && rgb[1]==0 && rgb[2]==0);
    assert(rgb[3]==255 && rgb[4]==255 && rgb[5]==255);
    assert(!uvc_payload_feed(&s,bad,sizeof(bad)));
    assert(!uvc_payload_feed(&s,err,sizeof(err)));
    assert(!uvc_payload_feed(&s,full,sizeof(full)));
    assert(!uvc_payload_feed(&s,a,sizeof(a)));
    assert(uvc_payload_feed(&s,b,sizeof(b)));
    /* Missing EOF, truncated frame, overrun, FID recovery. */
    assert(!uvc_payload_feed(&s,full,6));
    assert(!uvc_payload_feed(&s,a,sizeof(a)));
    assert(!uvc_payload_feed(&s,a,sizeof(a)));
    assert(!uvc_payload_feed(&s,b,sizeof(b)));
    assert(uvc_payload_feed(&s,full,sizeof(full)));
    /* Bulk synchronization drops even header-looking initial pixels. */
    {
        unsigned char storage[16];
        struct uvc_bulk_payload bulk = { .data=storage, .capacity=16 };
        struct uvc_payload frame = { .data=out, .capacity=8 };
        assert(!uvc_bulk_feed(&bulk,&frame,full,sizeof(full),16));
        assert(frame.eofs == 0 && bulk.synchronized);
        /* EOF in the first chunk must not publish before the tail arrives. */
        assert(!uvc_bulk_feed(&bulk,&frame,full,8,8));
        assert(frame.eofs == 0);
        assert(uvc_bulk_feed(&bulk,&frame,full+8,2,8));
        assert(!memcmp(out,full+2,8));
        /* A ZLP terminates a payload consisting of full-sized transfers. */
        frame = (struct uvc_payload){ .data=out, .capacity=8 };
        assert(!uvc_bulk_feed(&bulk,&frame,full,10,10));
        assert(uvc_bulk_feed(&bulk,&frame,full,0,10));
        /* Negotiated maximum also terminates without a short transfer. */
        frame = (struct uvc_payload){ .data=out, .capacity=8 };
        bulk.capacity = 10;
        assert(uvc_bulk_feed(&bulk,&frame,full,10,10));
        /* Overlong accumulation is discarded; recovery needs a boundary. */
        assert(!uvc_bulk_feed(&bulk,&frame,full,8,8));
        assert(!uvc_bulk_feed(&bulk,&frame,full,8,8));
        assert(!bulk.synchronized);
        assert(!uvc_bulk_feed(&bulk,&frame,full,0,8));
        assert(bulk.synchronized);
    }
    puts("PASS: frame boundaries, errors, overflow, recovery, YUY2 RGB");
    return 0;
}
