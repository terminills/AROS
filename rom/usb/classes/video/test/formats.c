/* Host regression: cc -fsanitize=address,undefined formats.c -ljpeg */
#include <assert.h>
#include <string.h>
#include "../uvc_jpeg.h"
static void check_image(unsigned width, unsigned height)
{
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr error;
    unsigned char *compressed = NULL;
    unsigned long length = 0;
    size_t bytes = (size_t)width*height*3;
    unsigned char *rgb = malloc(bytes), *row = malloc(width*3);
    size_t i;
    assert(rgb && row);
    memset(row, 128, width*3);
    c.err = jpeg_std_error(&error);
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &compressed, &length);
    c.image_width = width; c.image_height = height;
    c.input_components = 3; c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < height) jpeg_write_scanlines(&c, &row, 1);
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    assert(uvc_jpeg_rgb(compressed, length, rgb, bytes, width, height, NULL));
    for (i = 0; i < bytes; i++) assert(rgb[i] == 128);
    {
        unsigned char *padded = calloc(1, length+4);
        assert(padded); memcpy(padded, compressed, length);
        for (i = 1; i <= 3; i++)
            assert(uvc_jpeg_rgb(padded, length+i, rgb, bytes, width, height, NULL));
        assert(!uvc_jpeg_rgb(padded, length+4, rgb, bytes, width, height, NULL));
        free(padded);
    }
    assert(!uvc_jpeg_rgb(compressed, length, rgb, bytes-1, width, height, NULL));
    assert(!uvc_jpeg_rgb(compressed, length, rgb, bytes, width-1, height, NULL));
    assert(!uvc_jpeg_rgb(compressed, length-2, rgb, bytes, width, height, NULL));
    compressed[0] = 0;
    assert(!uvc_jpeg_rgb(compressed, length, rgb, bytes, width, height, NULL));
    compressed[0] = 0xff;
    /* Exercise the decoder error path with a retained SOI/EOI envelope. */
    compressed[2] = 0;
    assert(!uvc_jpeg_rgb(compressed, length, rgb, bytes, width, height, NULL));
    free(row); free(rgb); free(compressed);
}
int main(void)
{
    check_image(64, 32);
    check_image(3840, 2160);
    puts("JPEG bounds, dimensions, corruption, truncation and native 4K: PASS");
    return 0;
}
