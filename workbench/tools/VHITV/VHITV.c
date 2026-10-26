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
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <graphics/rastport.h>
#include <cybergraphx/cybergraphics.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include <proto/cybergraphics.h>

#include <vhi/vhi.h>
#include <devices/uvc_capture.h>

#include <resources/log.h>
#define __LOG_LIBBASE LogResBase
#include <proto/log.h>
#include <workbench/startup.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char version[] __attribute__((used)) =
    "$VER: VHITV 1.2 (26.10.2026)";

long __stack = 131072;
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
struct Library *CyberGfxBase = NULL;
struct Library *VHIBase = NULL;
struct Library *GadToolsBase = NULL;
struct Library *AslBase = NULL;
struct Library *LogResBase = NULL;

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

/* Only one request is in flight. The worker owns its driver and VHI handle;
 * the UI owns returned frames after ReplyMsg. LoadSeg gives the worker its
 * own executable lifetime, including when the UI exits on an SMP machine. */
struct CaptureRequest {
    struct Message message;
    BOOL quit;
    const char *driver;
    ULONG input, width, height, interval, error;
    LONG mode;
    struct vhi_image *image;
};

static int capture_worker(void)
{
    struct Process *self = (struct Process *)FindTask(NULL);
    struct CaptureRequest *request;
    APTR handle = NULL;
    LONG current_mode = -1;
    ULONG current_interval = 0;
    ULONG error = 0;
    for (;;) {
        WaitPort(&self->pr_MsgPort);
        request = (struct CaptureRequest *)GetMsg(&self->pr_MsgPort);
        if (!request) continue;
        if (request->quit) {
            if (handle) call_vhi(VHI_METHOD_CLOSE, 0, NULL, &error, handle);
            if (VHIBase) CloseLibrary(VHIBase);
            ReplyMsg(&request->message);
            return RETURN_OK;
        }
        request->error = 0;
        request->image = NULL;
        if (!VHIBase) VHIBase = OpenLibrary(request->driver, 0);
        if (VHIBase && !handle)
            handle = (APTR)call_vhi(VHI_METHOD_OPEN, 0, NULL, &error, NULL);
        if (!handle) request->error = VHI_ERR_INTERNAL_ERROR;
        else {
            struct vhi_setoptions option = {0};
            struct vhi_size maximum = {0};
            struct vhi_digitize digitize = {0};
            error = 0;
            option.option = VHI_OPT_INPUT; option.value = request->input;
            call_vhi(VHI_METHOD_SET, VHI_OPTIONS, &option, &error, handle);
            if (!error && request->mode != current_mode) {
                call_vhi(VHI_METHOD_SET, VHI_UVC_MODE, (APTR)(IPTR)request->mode, &error, handle);
                if (!error) current_mode = request->mode;
            }
            if (!error)
                call_vhi(VHI_METHOD_GET, VHI_MAXIMUM_SIZE, &maximum, &error, handle);
            digitize.dim.x2 = digitize.dim.dst_width =
                request->width ? request->width : maximum.max_width;
            digitize.dim.y2 = digitize.dim.dst_height =
                request->height ? request->height : maximum.max_height;
            if (!error)
                call_vhi(VHI_METHOD_PERFORM, VHI_CHECK_DIGITIZE_SIZE,
                         &digitize.dim, &error, handle);
            if (!error && request->interval != current_interval) {
                call_vhi(VHI_METHOD_SET, VHI_UVC_INTERVAL,
                         (APTR)(IPTR)request->interval, &error, handle);
                if (!error) current_interval = request->interval;
            }
            if (!error)
                request->image = (APTR)call_vhi(VHI_METHOD_PERFORM,
                    VHI_DIGITIZE_PICTURE, &digitize, &error, handle);
            request->error = error;
        }
        Delay(2); /* bound synthetic preview rate without blocking the UI */
        ReplyMsg(&request->message);
    }
}

struct App
{
    struct Window *window;
    struct Window *log_window;
    struct Menu *menus;
    APTR visual_info;
    BOOL paused, menus_dirty;
    ULONG mode_count;
    struct UVCModeInfo modes[32];
    char mode_labels[32][64];
    struct NewMenu menu_entries[128];
    ULONG rate_count, rates[30], requested_interval;
    LONG requested_mode;
    char rate_labels[30][48];
    struct vhi_image *last_frame;
    APTR vhi_handle;
    ULONG input_count;
    ULONG input;
    STRPTR input_name;
    UBYTE *display;
    ULONG display_size;
    ULONG frame_width, frame_height;
    ULONG requested_width, requested_height;
    char status[160];
};

enum { MENU_QUIT = 1, MENU_NEXT, MENU_PREVIOUS, MENU_PAUSE, MENU_SNAPSHOT,
       MENU_SIZE_DEFAULT, MENU_SIZE_VGA, MENU_SIZE_SVGA, MENU_SIZE_HD, MENU_SIZE_FHD, MENU_INPUT_INFO, MENU_DEVICE_LOG };
static struct NewMenu menu_spec[] = {
    { NM_TITLE, (STRPTR)"Project", NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Save snapshot...", (STRPTR)"S", 0, 0, (APTR)MENU_SNAPSHOT },
    { NM_ITEM, (STRPTR)"Quit", (STRPTR)"Q", 0, 0, (APTR)MENU_QUIT },
    { NM_TITLE, (STRPTR)"Input", NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Input information...", (STRPTR)"I", 0, 0, (APTR)MENU_INPUT_INFO },
    { NM_ITEM, (STRPTR)"Next input", (STRPTR)"N", 0, 0, (APTR)MENU_NEXT },
    { NM_ITEM, (STRPTR)"Previous input", (STRPTR)"P", 0, 0, (APTR)MENU_PREVIOUS },
    { NM_ITEM, (STRPTR)"Device log", (STRPTR)"L", 0, 0, (APTR)MENU_DEVICE_LOG },
    { NM_TITLE, (STRPTR)"Capture", NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Pause", NULL, CHECKIT | MENUTOGGLE, 0, (APTR)MENU_PAUSE },
    { NM_TITLE, (STRPTR)"Requested size", NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Input default", NULL, 0, 0, (APTR)MENU_SIZE_DEFAULT },
    { NM_ITEM, (STRPTR)"640 x 480 (nearest native)", NULL, 0, 0, (APTR)MENU_SIZE_VGA },
    { NM_ITEM, (STRPTR)"800 x 600 (nearest native)", NULL, 0, 0, (APTR)MENU_SIZE_SVGA },
    { NM_ITEM, (STRPTR)"1280 x 720 (nearest native)", NULL, 0, 0, (APTR)MENU_SIZE_HD },
    { NM_ITEM, (STRPTR)"1920 x 1080 (nearest native)", NULL, 0, 0, (APTR)MENU_SIZE_FHD },
    { NM_END, NULL, NULL, 0, 0, NULL }
};

/* Rebuild between event batches: never free an ItemAddress result while
 * processing its NextSelect chain. Labels live as long as the menu strip. */
#define MENU_NATIVE_BASE 100
#define MENU_RATE_DEFAULT 199
#define MENU_RATE_BASE 200
static BOOL rebuild_menus(struct App *app)
{
    ULONG error = 0, count, i, n = 0;
    struct Menu *menus;
    if (app->menus) { ClearMenuStrip(app->window); FreeMenus(app->menus); }
    app->menus = NULL;
    app->mode_count = 0;
    while (menu_spec[n].nm_Type != NM_END) {
        app->menu_entries[n] = menu_spec[n];
        if ((IPTR)menu_spec[n].nm_UserData == MENU_PAUSE && app->paused)
            app->menu_entries[n].nm_Flags |= CHECKED;
        n++;
    }
    count = call_vhi(VHI_METHOD_GET, VHI_UVC_MODECOUNT, NULL,
                     &error, app->vhi_handle);
    if (!error && count && count <= 32) {
        app->menu_entries[n++] = (struct NewMenu){ NM_TITLE,
            (STRPTR)"Camera modes", NULL, 0, 0, NULL };
        for (i = 0; i < count && app->mode_count < 31; i++) {
            struct VHIUVCModeQuery q = { .size = sizeof(q), .index = i };
            ULONG index = app->mode_count;
            error = 0;
            if (!call_vhi(VHI_METHOD_GET, VHI_UVC_MODEINFO, &q,
                          &error, app->vhi_handle) || error) continue;
            app->modes[index] = q.mode;
            snprintf(app->mode_labels[index], sizeof(app->mode_labels[index]),
                     "%lux%lu %s", (unsigned long)q.mode.width,
                     (unsigned long)q.mode.height,
                     q.mode.pixel_format == UVC_MODE_MJPEG ? "MJPEG" :
                     q.mode.pixel_format == UVC_MODE_GRAY8 ? "Gray8" :
                     q.mode.pixel_format == UVC_MODE_GRAY16 ? "Gray16" : "YUY2");
            app->menu_entries[n++] = (struct NewMenu){ NM_ITEM,
                app->mode_labels[index], NULL, 0, 0,
                (APTR)(IPTR)(MENU_NATIVE_BASE + index) };
            app->mode_count++;
        }
    }
    app->rate_count = 0;
    if (app->mode_count) {
        struct vhi_size maximum = {0};
        ULONG w = app->requested_width, h = app->requested_height, best = 0;
        unsigned long long distance = ~0ULL;
        if (!w || !h) {
            error = 0;
            call_vhi(VHI_METHOD_GET, VHI_MAXIMUM_SIZE, &maximum, &error, app->vhi_handle);
            w = maximum.max_width; h = maximum.max_height;
        }
        for (i = 0; i < app->mode_count; i++) {
            struct UVCModeInfo *mode = &app->modes[i];
            unsigned long long d = (unsigned long long)(w > mode->width ? w-mode->width : mode->width-w) +
                (h > mode->height ? h-mode->height : mode->height-h);
            if (d < distance) { best = i; distance = d; }
        }
        if (app->requested_mode >= 0 && app->requested_mode < app->mode_count) best = app->requested_mode;
        {
            struct VHIUVCRateQuery query = { .size = sizeof(query), .index = best };
            error = 0;
            if (call_vhi(VHI_METHOD_GET, VHI_UVC_RATES, &query, &error, app->vhi_handle)) {
                app->menu_entries[n++] = (struct NewMenu){ NM_TITLE, (STRPTR)"Frame rate", NULL, 0, 0, NULL };
                app->menu_entries[n++] = (struct NewMenu){ NM_ITEM, (STRPTR)"Input default", NULL, 0, 0, (APTR)MENU_RATE_DEFAULT };
                if (!query.rates.count && query.rates.minimum && query.rates.step &&
                    query.rates.maximum >= query.rates.minimum) {
                    /* Offer exact endpoints of a continuous descriptor. */
                    query.rates.intervals[query.rates.count++] = query.rates.minimum;
                    if (query.rates.maximum != query.rates.minimum)
                        query.rates.intervals[query.rates.count++] = query.rates.minimum +
                            ((query.rates.maximum-query.rates.minimum)/query.rates.step)*query.rates.step;
                }
                for (i = 0; i < query.rates.count && i < 30; i++) {
                    ULONG interval = query.rates.intervals[i], fps100;
                    if (!interval) continue;
                    fps100 = 1000000000UL / interval;
                    snprintf(app->rate_labels[app->rate_count], 48, "%lu.%02lu fps (%lu x 100 ns)",
                        (unsigned long)(fps100/100), (unsigned long)(fps100%100), (unsigned long)interval);
                    app->rates[app->rate_count] = interval;
                    app->menu_entries[n++] = (struct NewMenu){ NM_ITEM, app->rate_labels[app->rate_count],
                        NULL, 0, 0, (APTR)(IPTR)(MENU_RATE_BASE+app->rate_count) };
                    app->rate_count++;
                }
            }
        }
    }
    app->menu_entries[n] = (struct NewMenu){ NM_END, NULL, NULL, 0, 0, NULL };
    menus = CreateMenus(app->menu_entries, TAG_DONE);
    if (!menus) return FALSE;
    if (!LayoutMenus(menus, app->visual_info, TAG_DONE) ||
        !SetMenuStrip(app->window, menus)) { FreeMenus(menus); return FALSE; }
    app->menus = menus;
    app->menus_dirty = FALSE;
    return TRUE;
}

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
    char mode[40] = "";
    if (app->frame_width && app->frame_height)
        snprintf(mode, sizeof(mode), " %lux%lu",
                 (unsigned long)app->frame_width,
                 (unsigned long)app->frame_height);
    snprintf(app->status, sizeof(app->status), "%lu/%lu %s%s - %s",
             (unsigned long)(app->input + 1),
             (unsigned long)app->input_count, name, mode, detail);
    SetWindowTitles(app->window, (CONST_STRPTR)app->status,
                    (CONST_STRPTR)-1);
    draw_status(app);
}

static void select_input(struct App *app, ULONG input)
{
    struct vhi_setoptions option;
    ULONG error = 0;
    STRPTR name;

    app->input_count = (ULONG)call_vhi(VHI_METHOD_GET, VHI_NUMBER_OF_INPUTS,
                                      NULL, &error, app->vhi_handle);
    if (error || !app->input_count)
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
    free_frame(app->last_frame);
    app->last_frame = NULL;
    app->input = input;
    app->menus_dirty = TRUE;
    app->requested_width = app->requested_height = 0;
    app->requested_interval = 0;
    app->requested_mode = -1;
    app->frame_width = app->frame_height = 0;
    SetAPen(app->window->RPort, 0);
    RectFill(app->window->RPort, app->window->BorderLeft,
             app->window->BorderTop + STATUS_HEIGHT,
             app->window->Width - app->window->BorderRight - 1,
             app->window->Height - app->window->BorderBottom - 1);
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
    app->frame_width = image->width;
    app->frame_height = image->height;
    set_status(app, "live - N/P changes input, Q quits");
}

/* Export the captured image, not the scaled window preview. */
static void save_snapshot(struct App *app)
{
    struct FileRequester *request;
    struct vhi_image *image = app->last_frame;
    UBYTE *row = NULL;
    BPTR file = 0, existing;
    ULONG x, y;
    char path[1024], header[80];
    int header_size;
    BOOL ok = FALSE;
    if (!image || !image->width || !image->height)
    {
        set_status(app, "no captured frame to save");
        return;
    }
    request = AllocAslRequestTags(ASL_FileRequest,
        ASLFR_Window, (IPTR)app->window,
        ASLFR_TitleText, (IPTR)"Save snapshot as PPM",
        ASLFR_DoSaveMode, TRUE,
        ASLFR_InitialFile, (IPTR)"snapshot.ppm", TAG_DONE);
    if (!request) { set_status(app, "file requester unavailable"); return; }
    if (!AslRequest(request, NULL)) goto done;
    if (strlen(request->fr_Drawer) >= sizeof(path)) goto failed;
    strcpy(path, request->fr_Drawer);
    if (!request->fr_File[0] || !AddPart(path, request->fr_File, sizeof(path)))
        goto failed;
    existing = Lock(path, ACCESS_READ);
    if (existing)
    {
        struct EasyStruct confirm = { sizeof(struct EasyStruct), 0,
            (STRPTR)"Replace snapshot?", (STRPTR)"The selected file exists.",
            (STRPTR)"Replace|Cancel" };
        UnLock(existing);
        if (!EasyRequestArgs(app->window, &confirm, NULL, NULL)) goto done;
    }
    if (image->width > 0x7fffffffUL / 3) goto failed;
    row = AllocVec(image->width * 3, MEMF_PUBLIC);
    if (!row) goto failed;
    /* Reject unsupported formats before opening or replacing the file. */
    if (!source_rgb(image, 0, 0, row, row + 1, row + 2)) goto failed;
    file = Open(path, MODE_NEWFILE);
    if (!file) goto failed;
    header_size = snprintf(header, sizeof(header), "P6\n%lu %lu\n255\n",
        (unsigned long)image->width, (unsigned long)image->height);
    if (header_size <= 0 || header_size >= sizeof(header) ||
        Write(file, header, header_size) != header_size) goto failed;
    for (y = 0; y < image->height; y++)
    {
        for (x = 0; x < image->width; x++)
            if (!source_rgb(image, x, y, row + x*3, row + x*3+1,
                            row + x*3+2)) goto failed;
        if (Write(file, row, image->width * 3) != image->width * 3)
            goto failed;
    }
    ok = Close(file); file = 0;
failed:
    set_status(app, ok ? "snapshot saved (PPM)" : "snapshot save failed");
done:
    if (file) Close(file);
    if (row) FreeVec(row);
    FreeAslRequest(request);
}

static void show_input_info(struct App *app)
{
    char text[2048];
    ULONG i, used, error = 0;
    struct UVCDeviceInfo device = {0};
    struct EasyStruct info = { sizeof(info), 0, (STRPTR)"Capture input",
        (STRPTR)"%s", (STRPTR)"Close" };
    IPTR args[1] = { (IPTR)text };
    used = snprintf(text, sizeof(text),
        "Input %lu/%lu: %.128s\nLast captured frame: %lu x %lu\n"
        "Requested size: %lu x %lu (0 = input default)\n"
        "Enumerated native modes: %lu\n",
        (unsigned long)(app->input + 1), (unsigned long)app->input_count,
        app->input_name ? (const char *)app->input_name : "unknown",
        (unsigned long)app->frame_width, (unsigned long)app->frame_height,
        (unsigned long)app->requested_width, (unsigned long)app->requested_height,
        (unsigned long)app->mode_count);
    device.size = sizeof(device);
    if (call_vhi(VHI_METHOD_GET, VHI_UVC_DEVICEINFO, &device,
                 &error, app->vhi_handle)) {
        const char *speed = device.speed == UVC_SPEED_SUPER ? "SuperSpeed" :
            device.speed == UVC_SPEED_HIGH ? "High speed" :
            device.speed == UVC_SPEED_LOW ? "Low speed" : "Full speed";
        used += snprintf(text + used, sizeof(text) - used,
            "USB %04lx:%04lx - %s, %s\nVC interface %lu / VS interface %lu\n",
            (unsigned long)device.vendor_id, (unsigned long)device.product_id,
            speed, device.bulk ? "bulk" : "isochronous",
            (unsigned long)device.control_interface,
            (unsigned long)device.streaming_interface);
        if (device.negotiated)
            used += snprintf(text + used, sizeof(text) - used,
                "Last committed: %lux%lu, format %lu / frame %lu\n"
                "Interval %lu x 100 ns, payload %lu, frame %lu bytes, alt %lu\n"
                "Commit history does not imply an active stream.\n",
                (unsigned long)device.width, (unsigned long)device.height,
                (unsigned long)device.format_index, (unsigned long)device.frame_index,
                (unsigned long)device.interval, (unsigned long)device.payload_bytes,
                (unsigned long)device.frame_bytes, (unsigned long)device.alternate);
        else
            used += snprintf(text + used, sizeof(text) - used,
                "No successful streaming commit recorded.\n");
    } else
        used += snprintf(text + used, sizeof(text) - used,
            "USB metadata unavailable (synthetic input or older driver).\n");
    for (i = 0; i < app->mode_count; i++) {
        const struct UVCModeInfo *mode = &app->modes[i];
        if (mode->width == app->frame_width && mode->height == app->frame_height &&
            (app->requested_mode < 0 || i == app->requested_mode)) {
            snprintf(text + used, sizeof(text) - used,
                "Matching descriptor: format %lu / frame %lu\n"
                "Descriptor default interval: %lu x 100 ns\n"
                "Proposed capture interval: %lu x 100 ns\n"
                "Descriptor frame buffer: %lu bytes\n"
                "Intervals are descriptor values, not measured preview FPS.",
                (unsigned long)mode->format_index, (unsigned long)mode->frame_index,
                (unsigned long)mode->default_interval,
                (unsigned long)mode->capture_interval,
                (unsigned long)mode->max_frame_bytes);
            break;
        }
    }
    EasyRequestArgs(app->window, &info, NULL, (RAWARG)args);
}

/* Copy under log.resource's read lock, then draw without holding it.
 * Match the generation, not a product name shared by two identical cameras. */
static void draw_device_log(struct App *app)
{
    char lines[12][192] = {{0}}, marker[40];
    struct UVCDeviceInfo device = { .size = sizeof(device) };
    ULONG error = 0, count = 0, i, start, rows, columns;
    struct Window *window = app->log_window;
    struct RastPort *rp;
    APTR entry = NULL;
    if (!window) return;
    if (!LogResBase)
        strcpy(lines[count++], "log.resource unavailable");
    else if (!call_vhi(VHI_METHOD_GET, VHI_UVC_DEVICEINFO, &device,
                      &error, app->vhi_handle))
        strcpy(lines[count++], "Select a USB camera to view its capture log.");
    else {
        snprintf(marker, sizeof(marker), "[uvc:%lu]", (unsigned long)device.binding_id);
        logLockEntries(LLF_READ);
        while ((entry = logNextEntry(&entry)) && entry != (APTR)-1) {
            STRPTR origin = NULL, message = NULL;
            struct TagItem attrs[] = {
                { LOGMA_Component, (IPTR)&origin },
                { LOGMA_Entry, (IPTR)&message }, { TAG_DONE, 0 }
            };
            logGetEntryAttrs(entry, attrs);
            if (origin && strstr((char *)origin, "uvc") && message &&
                strstr((char *)message, marker)) {
                snprintf(lines[count % 12], sizeof(lines[0]), "%s", message);
                count++;
            }
        }
        logUnlockEntries(LLF_READ);
        if (!count) strcpy(lines[count++], "Waiting for this camera's capture records...");
    }
    rp = window->RPort;
    SetAPen(rp, 0);
    RectFill(rp, window->BorderLeft, window->BorderTop,
        window->Width-window->BorderRight-1, window->Height-window->BorderBottom-1);
    SetAPen(rp, 1); SetBPen(rp, 0);
    rows = (window->Height-window->BorderTop-window->BorderBottom-8) / rp->TxHeight;
    if (rows > 12) rows = 12;
    if (rows > count) rows = count;
    columns = (window->Width-window->BorderLeft-window->BorderRight-12) / rp->TxWidth;
    start = count - rows;
    for (i = 0; i < rows; i++) {
        char *line = lines[(start+i) % 12];
        ULONG length = strlen(line);
        if (length > columns) length = columns;
        Move(rp, window->BorderLeft+6, window->BorderTop+4+rp->TxBaseline+i*rp->TxHeight);
        Text(rp, line, length);
    }
}

static void open_device_log(struct App *app)
{
    if (!app->log_window)
        app->log_window = OpenWindowTags(NULL,
            WA_Title, (IPTR)"VHITV - selected device log (log.resource)",
            WA_Left, 30, WA_Top, 450, WA_InnerWidth, 760, WA_InnerHeight, 180,
            WA_MinWidth, 300, WA_MinHeight, 100, WA_MaxWidth, ~0UL, WA_MaxHeight, ~0UL,
            WA_CloseGadget, TRUE, WA_DragBar, TRUE, WA_DepthGadget, TRUE,
            WA_SizeGadget, TRUE, WA_SmartRefresh, TRUE,
            WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_NEWSIZE,
            TAG_DONE);
    draw_device_log(app);
}

static void process_log_events(struct App *app)
{
    struct IntuiMessage *message;
    if (!app->log_window) return;
    while ((message = (APTR)GetMsg(app->log_window->UserPort))) {
        ULONG class = message->Class;
        ReplyMsg((struct Message *)message);
        if (class == IDCMP_CLOSEWINDOW) {
            CloseWindow(app->log_window); app->log_window = NULL;
            return;
        }
        if (class == IDCMP_REFRESHWINDOW) {
            BeginRefresh(app->log_window);
            draw_device_log(app);
            EndRefresh(app->log_window, TRUE);
        } else draw_device_log(app);
    }
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
        else if (class == IDCMP_MENUPICK)
        {
            while (code != MENUNULL)
            {
                struct MenuItem *item = ItemAddress(app->menus, code);
                ULONG action;
                if (!item) break;
                code = item->NextSelect;
                action = (ULONG)(IPTR)GTMENUITEM_USERDATA(item);
                if (action == MENU_RATE_DEFAULT || (action >= MENU_RATE_BASE && action < MENU_RATE_BASE+app->rate_count)) {
                    app->requested_interval = action == MENU_RATE_DEFAULT ? 0 : app->rates[action-MENU_RATE_BASE];
                    set_status(app, "frame interval selected (takes effect on next capture)");
                }
                else if (action == MENU_DEVICE_LOG) open_device_log(app);
                else if (action == MENU_QUIT) running = FALSE;
                else if (action == MENU_SNAPSHOT) save_snapshot(app);
                else if (action == MENU_INPUT_INFO) show_input_info(app);
                else if (action == MENU_NEXT) select_input(app, app->input + 1);
                else if (action == MENU_PREVIOUS && app->input_count)
                    select_input(app, app->input ? app->input - 1 : app->input_count - 1);
                else if (action >= MENU_SIZE_DEFAULT && action <= MENU_SIZE_FHD)
                {
                    static const ULONG sizes[][2] = {
                        {0,0}, {640,480}, {800,600}, {1280,720}, {1920,1080}
                    };
                    ULONG index = action - MENU_SIZE_DEFAULT;
                    app->requested_mode = -1;
                    app->requested_interval = 0; app->menus_dirty = TRUE;
                    app->requested_width = sizes[index][0];
                    app->requested_height = sizes[index][1];
                    free_frame(app->last_frame);
                    app->last_frame = NULL;
                    app->frame_width = app->frame_height = 0;
                    set_status(app, "capture size changed; awaiting frame");
                }
                else if (!app->menus_dirty && action >= MENU_NATIVE_BASE &&
                         action < MENU_NATIVE_BASE + app->mode_count)
                {
                    ULONG index = action - MENU_NATIVE_BASE;
                    app->requested_mode = index;
                    app->requested_interval = 0; app->menus_dirty = TRUE;
                    app->requested_width = app->modes[index].width;
                    app->requested_height = app->modes[index].height;
                    free_frame(app->last_frame); app->last_frame = NULL;
                    app->frame_width = app->frame_height = 0;
                    set_status(app, "camera mode selected; awaiting frame");
                }
                else if (action == MENU_PAUSE)
                {
                    app->paused = (item->Flags & CHECKED) != 0;
                    set_status(app, app->paused ? "paused" : "resuming");
                }
            }
        }
        else if (class == IDCMP_VANILLAKEY)
        {
            if (code == 'q' || code == 'Q')
                running = FALSE;
            else if (code == 'l' || code == 'L')
                open_device_log(app);
            else if (code == 'i' || code == 'I')
                show_input_info(app);
            else if (code == 's' || code == 'S')
                save_snapshot(app);
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
    struct CaptureRequest request = {0};
    struct MsgPort *reply_port = NULL;
    struct Process *worker = NULL;
    BOOL pending = FALSE;
    BPTR worker_segment = 0;
    char worker_path[1024];
    ULONG error = 0;
    BOOL running = TRUE;
    int result = RETURN_FAIL;

    if (argc > 1 && !strcmp(argv[1], "--capture-worker"))
        return capture_worker();
    memset(&app, 0, sizeof(app));
    bug("VHITV: entered main, driver=%s input=%lu\n", driver,
        (unsigned long)initial_input);
    IntuitionBase = (struct IntuitionBase *)
                    OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)
              OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 0);
    VHIBase = OpenLibrary((CONST_STRPTR)driver, 0);
    GadToolsBase = OpenLibrary("gadtools.library", 39);
    AslBase = OpenLibrary("asl.library", 39);
    if (!IntuitionBase || !GfxBase || !CyberGfxBase || !VHIBase || !GadToolsBase || !AslBase)
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
    if (error || initial_input >= app.input_count)
    {
        printf("VHITV: requested input %lu is unavailable (%lu inputs)\n",
               (unsigned long)initial_input, (unsigned long)app.input_count);
        goto cleanup;
    }

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
        WA_SmartRefresh, TRUE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY |
                  IDCMP_REFRESHWINDOW | IDCMP_NEWSIZE | IDCMP_MENUPICK,
        TAG_DONE);
    if (!app.window)
    {
        printf("VHITV: could not open display window\n");
        goto cleanup;
    }

    app.visual_info = GetVisualInfo(app.window->WScreen, TAG_DONE);
    app.menus = CreateMenus(menu_spec, TAG_DONE);
    if (!app.visual_info || !app.menus ||
        !LayoutMenus(app.menus, app.visual_info, TAG_DONE) ||
        !SetMenuStrip(app.window, app.menus))
    {
        printf("VHITV: could not create menus\n");
        goto cleanup;
    }
    reply_port = CreateMsgPort();
    {
        const char *program = argc > 0 ? (const char *)FilePart(argv[0]) :
            (const char *)((struct WBStartup *)argv)->sm_ArgList[0].wa_Name;
        if (snprintf(worker_path, sizeof(worker_path), "PROGDIR:%s", program) >= sizeof(worker_path))
            goto cleanup;
        worker_segment = LoadSeg(worker_path);
    }
    if (!reply_port || !worker_segment) {
        printf("VHITV: cannot load capture worker\n");
        goto cleanup;
    }
    worker = CreateNewProcTags(NP_Seglist, worker_segment,
        NP_FreeSeglist, TRUE, NP_Cli, TRUE,
        NP_Name, (IPTR)"VHITV capture", NP_StackSize, 131072,
        NP_Arguments, (IPTR)"--capture-worker\n", TAG_DONE);
    if (!worker) goto cleanup;
    worker_segment = 0; /* ownership transferred to DOS */
    request.message.mn_ReplyPort = reply_port;
    request.message.mn_Length = sizeof(request);
    request.driver = driver;
    LogResBase = OpenResource("log.resource");
    select_input(&app, initial_input);
    while (running) {
        process_log_events(&app);
        running = process_events(&app);
        if (!running) break;
        if (app.menus_dirty && !rebuild_menus(&app)) break;
        if (pending && GetMsg(reply_port)) {
            pending = FALSE;
            /* Discard a frame from an input or mode changed while capturing. */
            if (!app.paused && request.input == app.input &&
                request.width == app.requested_width &&
                request.height == app.requested_height &&
                request.interval == app.requested_interval && request.mode == app.requested_mode) {
                if (request.image && !request.error) {
                    render_frame(&app, request.image);
                    free_frame(app.last_frame);
                    app.last_frame = request.image;
                    request.image = NULL;
                } else set_status(&app, vhi_error(request.error));
            }
            free_frame(request.image); request.image = NULL;
            draw_device_log(&app);
        }
        if (!pending && !app.paused) {
            request.input = app.input;
            request.width = app.requested_width;
            request.height = app.requested_height;
            request.interval = app.requested_interval;
            request.mode = app.requested_mode;
            PutMsg(&worker->pr_MsgPort, &request.message);
            pending = TRUE;
        }
        Wait((1UL << app.window->UserPort->mp_SigBit) |
             (1UL << reply_port->mp_SigBit) |
             (app.log_window ? (1UL << app.log_window->UserPort->mp_SigBit) : 0));
    }
    result = RETURN_OK;

cleanup:
    if (app.log_window) CloseWindow(app.log_window);
    /* Close the visible UI immediately; safely drain the bounded camera
     * request before destroying its reply port or releasing shared data. */
    if (app.window && app.menus) ClearMenuStrip(app.window);
    if (app.window) { CloseWindow(app.window); app.window = NULL; }
    if (worker) {
        if (pending) {
            WaitPort(reply_port); GetMsg(reply_port);
            free_frame(request.image); request.image = NULL;
        }
        request.quit = TRUE;
        PutMsg(&worker->pr_MsgPort, &request.message);
        WaitPort(reply_port); GetMsg(reply_port);
    }
    if (worker_segment) UnLoadSeg(worker_segment);
    if (reply_port) DeleteMsgPort(reply_port);
    free_frame(app.last_frame);
    if (app.input_name) FreeVec(app.input_name);
    if (app.display) FreeVec(app.display);
    if (app.window && app.menus) ClearMenuStrip(app.window);
    if (app.menus) FreeMenus(app.menus);
    if (app.visual_info) FreeVisualInfo(app.visual_info);
    if (app.window) CloseWindow(app.window);
    if (app.vhi_handle)
    {
        error = 0;
        call_vhi(VHI_METHOD_CLOSE, 0, NULL, &error, app.vhi_handle);
    }
    if (VHIBase) CloseLibrary(VHIBase);
    if (GadToolsBase) CloseLibrary(GadToolsBase);
    if (AslBase) CloseLibrary(AslBase);
    if (CyberGfxBase) CloseLibrary(CyberGfxBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return result;
}
