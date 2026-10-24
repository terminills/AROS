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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char version[] __attribute__((used)) =
    "$VER: VHITV 1.1 (24.10.2026)";

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
struct Library *CyberGfxBase = NULL;
struct Library *VHIBase = NULL;
struct Library *GadToolsBase = NULL;
struct Library *AslBase = NULL;

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
    struct Menu *menus;
    APTR visual_info;
    BOOL paused;
    struct vhi_image *last_frame;
    APTR vhi_handle;
    ULONG input_count;
    ULONG input;
    STRPTR input_name;
    UBYTE *display;
    ULONG display_size;
    ULONG frame_width, frame_height;
    char status[160];
};

enum { MENU_QUIT = 1, MENU_NEXT, MENU_PREVIOUS, MENU_PAUSE, MENU_SNAPSHOT };
static struct NewMenu menu_spec[] = {
    { NM_TITLE, (STRPTR)"Project", NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Save snapshot...", (STRPTR)"S", 0, 0, (APTR)MENU_SNAPSHOT },
    { NM_ITEM, (STRPTR)"Quit", (STRPTR)"Q", 0, 0, (APTR)MENU_QUIT },
    { NM_TITLE, (STRPTR)"Input", NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Next input", (STRPTR)"N", 0, 0, (APTR)MENU_NEXT },
    { NM_ITEM, (STRPTR)"Previous input", (STRPTR)"P", 0, 0, (APTR)MENU_PREVIOUS },
    { NM_TITLE, (STRPTR)"Capture", NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Pause", NULL, CHECKIT | MENUTOGGLE, 0, (APTR)MENU_PAUSE },
    { NM_END, NULL, NULL, 0, 0, NULL }
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
                if (action == MENU_QUIT) running = FALSE;
                else if (action == MENU_SNAPSHOT) save_snapshot(app);
                else if (action == MENU_NEXT) select_input(app, app->input + 1);
                else if (action == MENU_PREVIOUS && app->input_count)
                    select_input(app, app->input ? app->input - 1 : app->input_count - 1);
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
        WA_SimpleRefresh, TRUE,
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
    select_input(&app, initial_input);
    while (running)
    {
        struct vhi_image *image;
        running = process_events(&app);
        if (!running) break;
        if (app.paused)
        {
            WaitPort(app.window->UserPort);
            continue;
        }
        /* Input changes may select a different native capture geometry. */
        error = 0;
        memset(&maximum, 0, sizeof(maximum));
        call_vhi(VHI_METHOD_GET, VHI_MAXIMUM_SIZE, &maximum, &error,
                 app.vhi_handle);
        if (error || !maximum.max_width || !maximum.max_height)
        {
            set_status(&app, error ? vhi_error(error) : "capture size unavailable");
            running = process_events(&app);
            Delay(2);
            continue;
        }
        memset(&digitize, 0, sizeof(digitize));
        digitize.dim.x2 = maximum.max_width;
        digitize.dim.y2 = maximum.max_height;
        digitize.dim.dst_width = maximum.max_width;
        digitize.dim.dst_height = maximum.max_height;
        image = (struct vhi_image *)call_vhi(VHI_METHOD_PERFORM,
                                             VHI_DIGITIZE_PICTURE,
                                             &digitize, &error,
                                             app.vhi_handle);
        if (image && !error)
        {
            render_frame(&app, image);
            free_frame(app.last_frame);
            app.last_frame = image;
            image = NULL;
        }
        else
            set_status(&app, vhi_error(error));
        free_frame(image);
        running = process_events(&app);
        Delay(2);
    }
    result = RETURN_OK;

cleanup:
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
