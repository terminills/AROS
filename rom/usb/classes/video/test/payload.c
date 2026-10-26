#include <assert.h>
#include <stdio.h>
#include "../uvc_payload.h"
int main(void)
{
    unsigned char out[8], rgb[12];
    {
        unsigned char bytes[16];
        struct uvc_payload compressed = { .data = bytes, .capacity = sizeof(bytes), .variable_length = 1 };
        const unsigned char jpeg[] = {2,2,0xff,0xd8,0xff,0xd9};
        const unsigned char empty[] = {2,3};
        const unsigned char damaged[] = {2,0x42,0xff,0xd8,0xff,0xd9};
        assert(uvc_payload_feed(&compressed, jpeg, sizeof(jpeg)));
        assert(compressed.used == 4);
        assert(!uvc_payload_feed(&compressed, jpeg, sizeof(jpeg)));
        assert(!uvc_payload_feed(&compressed, empty, sizeof(empty)));
        assert(!uvc_payload_feed(&compressed, damaged, sizeof(damaged)));
    }
    {
        const unsigned char gray[] = {0,128,255};
        const unsigned char gray16[] = {255,0,0,128,0,255};
        uvc_gray_rgb(gray, rgb, 3, 0);
        assert(rgb[0] == 0 && rgb[3] == 128 && rgb[8] == 255);
        uvc_gray_rgb(gray16, rgb, 3, 1);
        assert(rgb[0] == 0 && rgb[4] == 128 && rgb[8] == 255);
    }
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
