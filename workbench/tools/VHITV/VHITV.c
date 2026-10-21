/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    VHITV - a small live-view client for Video Hardware Interface drivers.

    The application deliberately knows nothing about USB or UVC transport.
    It consumes the public VHI contract, so the built-in uvc.library test
    pattern works now and cameras/HDMI capture inputs start working as soon as
    their VHI backend returns frames.

    Usage: VHITV [driver.library] [input]

    Keys:  N = next input, P = previous input, Q = quit
*/

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <aros/libcall.h>
#include <aros/debug.h>
#include <intuition/intuition.h>
#include <graphics/rastport.h>
#include <cybergraphx/cybergraphics.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>

#include <vhi/vhi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
struct Library *CyberGfxBase = NULL;
struct Library *VHIBase = NULL;

/* VHI drivers are selected at runtime and all implement the same first LVO.
 * Do not include the generated proto/vhi.h inline here: its AROS_LIBREQ marks
 * a fixed VHIBase for startup auto-opening before main(), while this client
 * deliberately opens uvc.library (or another named driver) itself. */
static IPTR call_vhi(ULONG method, ULONG submethod, APTR attr,
                     ULONG *error, APTR handle)
{
    return AROS_LC5(IPTR, vhi_api,
                    AROS_LCA(ULONG, method, D2),
                    AROS_LCA(ULONG, submethod, D3),
                    AROS_LCA(APTR, attr, D4),
                    AROS_LCA(ULONG *, error, D5),
                    AROS_LCA(APTR, handle, D6),
                    struct Library *, VHIBase, 5, Vhi);
}

#define STATUS_HEIGHT 18
#define DEFAULT_DRIVER "uvc.library"

struct App
{
    struct Window *window;
    APTR vhi_handle;
    ULONG input_count;
    ULONG input;
    STRPTR input_name;
    UBYTE *display;
    ULONG display_size;
    char status[160];
};

static const char *vhi_error(ULONG error)
{
    switch (error)
    {
        case 0:                      return "ready";
        case VHI_ERR_NO_INPUT_DATA:  return "waiting for video";
        case VHI_ERR_NO_HARDWARE:    return "hardware unavailable";
        case VHI_ERR_OUT_OF_MEMORY:  return "out of memory";
        case VHI_ERR_ERR_WHILE_DIG:  return "capture error";
        case VHI_ERR_IO_READ:        return "input read error";
        default:                     return "VHI error";
    }
}

static void free_frame(struct vhi_image *image)
{
    if (!image)
        return;
    if (image->chunky) FreeVec(image->chunky);
    if (image->y)      FreeVec(image->y);
    if (image->u)      FreeVec(image->u);
    if (image->v)      FreeVec(image->v);
    FreeVec(image);
}

static UBYTE clamp_component(LONG value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (UBYTE)value;
}

static BOOL source_rgb(const struct vhi_image *image, ULONG x, ULONG y,
                       UBYTE *red, UBYTE *green, UBYTE *blue)
{
    ULONG offset;

    if (!image || x >= image->width || y >= image->height)
        return FALSE;

    switch (image->type)
    {
        case VHI_RGB_24:
        {
            const UBYTE *pixel;
            if (!image->chunky) return FALSE;
            pixel = (const UBYTE *)image->chunky +
                    (y * image->width + x) * 3;
            *red = pixel[0]; *green = pixel[1]; *blue = pixel[2];
            return TRUE;
        }

        case VHI_GRAYSCALE_8:
            if (!image->chunky) return FALSE;
            *red = *green = *blue =
                ((const UBYTE *)image->chunky)[y * image->width + x];
            return TRUE;

        case VHI_YUV_411:
        case VHI_YUV_422:
        case VHI_YUV_444:
        {
            LONG yy, uu, vv, chroma_x;
            ULONG divisor = image->type == VHI_YUV_411 ? 4 :
                            image->type == VHI_YUV_422 ? 2 : 1;
            if (!image->y || !image->u || !image->v) return FALSE;
            offset = y * image->width + x;
            chroma_x = x / divisor;
            yy = ((const UBYTE *)image->y)[offset];
            uu = ((const UBYTE *)image->u)
                 [y * ((image->width + divisor - 1) / divisor) + chroma_x] - 128;
            vv = ((const UBYTE *)image->v)
                 [y * ((image->width + divisor - 1) / divisor) + chroma_x] - 128;
            *red   = clamp_component(yy + ((359 * vv) >> 8));
            *green = clamp_component(yy - ((88 * uu + 183 * vv) >> 8));
            *blue  = clamp_component(yy + ((454 * uu) >> 8));
            return TRUE;
        }

        default:
            return FALSE;
    }
}

static BOOL ensure_display(struct App *app, ULONG bytes)
{
    UBYTE *replacement;

    if (bytes <= app->display_size)
        return TRUE;
    replacement = AllocVec(bytes, MEMF_PUBLIC);
    if (!replacement)
        return FALSE;
    if (app->display)
        FreeVec(app->display);
    app->display = replacement;
    app->display_size = bytes;
    return TRUE;
}

static void draw_status(struct App *app)
{
    struct RastPort *rp = app->window->RPort;
    LONG left = app->window->BorderLeft;
    LONG top = app->window->BorderTop;
    LONG right = app->window->Width - app->window->BorderRight - 1;

    SetAPen(rp, 0);
    RectFill(rp, left, top, right, top + STATUS_HEIGHT - 1);
    SetAPen(rp, 1);
    Move(rp, left + 4, top + 12);
    Text(rp, (CONST_STRPTR)app->status, strlen(app->status));
}

static void set_status(struct App *app, const char *detail)
{
    const char *name = app->input_name ? (const char *)app->input_name :
                                         "unknown input";
    snprintf(app->status, sizeof(app->status), "%lu/%lu %s - %s",
             (unsigned long)(app->input + 1),
             (unsigned long)app->input_count, name, detail);
    SetWindowTitles(app->window, (CONST_STRPTR)app->status,
                    (CONST_STRPTR)-1);
    draw_status(app);
}

static void select_input(struct App *app, ULONG input)
{
    struct vhi_setoptions option;
    ULONG error = 0;
    STRPTR name;

    if (!app->input_count)
        return;
    input %= app->input_count;
    option.option = VHI_OPT_INPUT;
    option.value = input;
    call_vhi(VHI_METHOD_SET, VHI_OPTIONS, &option, &error, app->vhi_handle);
    if (error)
    {
        set_status(app, vhi_error(error));
        return;
    }

    name = (STRPTR)call_vhi(VHI_METHOD_GET, VHI_NAME_OF_INPUT,
                           (APTR)(IPTR)input, &error, app->vhi_handle);
    if (app->input_name)
        FreeVec(app->input_name);
    app->input_name = (!error) ? name : NULL;
    app->input = input;
    set_status(app, "selected");
}

static void render_frame(struct App *app, const struct vhi_image *image)
{
    struct RastPort *rp = app->window->RPort;
    LONG inner_width = app->window->Width - app->window->BorderLeft -
                       app->window->BorderRight;
    LONG inner_height = app->window->Height - app->window->BorderTop -
                        app->window->BorderBottom - STATUS_HEIGHT;
    ULONG width, height, x, y;
    LONG dest_x, dest_y;

    if (!image || !image->width || !image->height ||
        inner_width <= 0 || inner_height <= 0)
        return;

    width = (ULONG)inner_width;
    height = (ULONG)(((unsigned long long)width * image->height) / image->width);
    if (height > (ULONG)inner_height)
    {
        height = (ULONG)inner_height;
        width = (ULONG)(((unsigned long long)height * image->width) /
                        image->height);
    }
    if (!width || !height || !ensure_display(app, width * height * 3))
    {
        set_status(app, "display buffer unavailable");
        return;
    }

    for (y = 0; y < height; y++)
    {
        ULONG source_y = (ULONG)(((unsigned long long)y * image->height) /
                                 height);
        for (x = 0; x < width; x++)
        {
            ULONG source_x = (ULONG)(((unsigned long long)x * image->width) /
                                     width);
            UBYTE *pixel = app->display + (y * width + x) * 3;
            if (!source_rgb(image, source_x, source_y,
                            &pixel[0], &pixel[1], &pixel[2]))
            {
                set_status(app, image->type == VHI_JPEG ||
                                   image->type == VHI_EXIF_JPEG
                                   ? "JPEG decoder needed" :
                                     "unsupported pixel format");
                return;
            }
        }
    }

    dest_x = app->window->BorderLeft + (inner_width - (LONG)width) / 2;
    dest_y = app->window->BorderTop + STATUS_HEIGHT +
             (inner_height - (LONG)height) / 2;
    SetAPen(rp, 0);
    RectFill(rp, app->window->BorderLeft,
             app->window->BorderTop + STATUS_HEIGHT,
             app->window->Width - app->window->BorderRight - 1,
             app->window->Height - app->window->BorderBottom - 1);
    WritePixelArray(app->display, 0, 0, width * 3, rp,
                    dest_x, dest_y, width, height, RECTFMT_RGB);
    set_status(app, "live - N/P changes input, Q quits");
}

static BOOL process_events(struct App *app)
{
    struct IntuiMessage *message;
    BOOL running = TRUE;

    while ((message = (struct IntuiMessage *)GetMsg(app->window->UserPort)))
    {
        ULONG class = message->Class;
        UWORD code = message->Code;
        ReplyMsg((struct Message *)message);

        if (class == IDCMP_CLOSEWINDOW)
            running = FALSE;
        else if (class == IDCMP_VANILLAKEY)
        {
            if (code == 'q' || code == 'Q')
                running = FALSE;
            else if (code == 'n' || code == 'N')
                select_input(app, app->input + 1);
            else if ((code == 'p' || code == 'P') && app->input_count)
                select_input(app, app->input ? app->input - 1 :
                                           app->input_count - 1);
        }
        else if (class == IDCMP_REFRESHWINDOW)
        {
            BeginRefresh(app->window);
            draw_status(app);
            EndRefresh(app->window, TRUE);
        }
        else if (class == IDCMP_NEWSIZE)
            draw_status(app);
    }
    return running;
}

int main(int argc, char **argv)
{
    const char *driver = argc > 1 ? argv[1] : DEFAULT_DRIVER;
    ULONG initial_input = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
    struct App app;
    struct vhi_size maximum;
    struct vhi_digitize digitize;
    ULONG error = 0;
    BOOL running = TRUE;
    int result = RETURN_FAIL;

    memset(&app, 0, sizeof(app));
    bug("VHITV: entered main, driver=%s input=%lu\n", driver,
        (unsigned long)initial_input);
    IntuitionBase = (struct IntuitionBase *)
                    OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)
              OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 0);
    VHIBase = OpenLibrary((CONST_STRPTR)driver, 0);
    if (!IntuitionBase || !GfxBase || !CyberGfxBase || !VHIBase)
    {
        printf("VHITV: could not open required libraries or %s\n", driver);
        goto cleanup;
    }

    app.vhi_handle = (APTR)call_vhi(VHI_METHOD_OPEN, 0, NULL, &error, NULL);
    if (!app.vhi_handle || error)
    {
        printf("VHITV: VHI open failed: %s (%lu)\n", vhi_error(error),
               (unsigned long)error);
        goto cleanup;
    }

    memset(&maximum, 0, sizeof(maximum));
    call_vhi(VHI_METHOD_GET, VHI_MAXIMUM_SIZE, &maximum, &error,
             app.vhi_handle);
    app.input_count = (ULONG)call_vhi(VHI_METHOD_GET, VHI_NUMBER_OF_INPUTS,
                                     NULL, &error, app.vhi_handle);
    if (!app.input_count)
        app.input_count = 1;

    app.window = OpenWindowTags(NULL,
        WA_Title,        (IPTR)"VHITV",
        WA_InnerWidth,   640,
        WA_InnerHeight,  400,
        WA_MinWidth,     240,
        WA_MinHeight,    160,
        WA_MaxWidth,     ~0UL,
        WA_MaxHeight,    ~0UL,
        WA_CloseGadget,  TRUE,
        WA_DragBar,      TRUE,
        WA_DepthGadget,  TRUE,
        WA_SizeGadget,   TRUE,
        WA_Activate,     TRUE,
        WA_SimpleRefresh, TRUE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY |
                  IDCMP_REFRESHWINDOW | IDCMP_NEWSIZE,
        TAG_DONE);
    if (!app.window)
    {
        printf("VHITV: could not open display window\n");
        goto cleanup;
    }

    select_input(&app, initial_input);
    memset(&digitize, 0, sizeof(digitize));
    digitize.dim.x2 = maximum.max_width;
    digitize.dim.y2 = maximum.max_height;
    digitize.dim.dst_width = maximum.max_width;
    digitize.dim.dst_height = maximum.max_height;

    while (running)
    {
        struct vhi_image *image;
        error = 0;
        image = (struct vhi_image *)call_vhi(VHI_METHOD_PERFORM,
                                             VHI_DIGITIZE_PICTURE,
                                             &digitize, &error,
                                             app.vhi_handle);
        if (image && !error)
            render_frame(&app, image);
        else
            set_status(&app, vhi_error(error));
        free_frame(image);
        running = process_events(&app);
        Delay(2);
    }
    result = RETURN_OK;

cleanup:
    if (app.input_name) FreeVec(app.input_name);
    if (app.display) FreeVec(app.display);
    if (app.window) CloseWindow(app.window);
    if (app.vhi_handle)
    {
        error = 0;
        call_vhi(VHI_METHOD_CLOSE, 0, NULL, &error, app.vhi_handle);
    }
    if (VHIBase) CloseLibrary(VHIBase);
    if (CyberGfxBase) CloseLibrary(CyberGfxBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return result;
}
