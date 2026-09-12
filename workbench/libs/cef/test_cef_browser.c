/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Focused CEF windowless browser and native 2D presentation probe
*/

#include <string.h>

#include <cybergraphx/cybergraphics.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <proto/cef.h>
#include <proto/cybergraphics.h>
#include <proto/debug.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>

#include "include/capi/cef_app_capi.h"
#include "include/capi/cef_browser_capi.h"
#include "include/capi/cef_client_capi.h"
#include "include/capi/cef_life_span_handler_capi.h"
#include "include/capi/cef_render_handler_capi.h"

#define VIEW_WIDTH 960
#define VIEW_HEIGHT 540
#define CLOSE_AFTER_PAINT_TICKS 100
#define CREATE_TIMEOUT_TICKS 1500
#define CLOSE_TIMEOUT_TICKS 500

struct Library *CEFBase = NULL;

static struct Window *browser_window;
static cef_browser_t *browser;
static int browser_closed;
static int close_requested;
static int first_paint;
static int initial_frame_logged;
static ULONG first_paint_tick;
static ULONG pump_tick;
static const char *snapshot_path;

static cef_client_t client;
static cef_render_handler_t render_handler;
static cef_life_span_handler_t life_span_handler;

/*
 * Developer:CEF, not Developer:Chromium: grit numbers IDR_* per build, so the
 * paks the standalone browser binary was built with resolve the wrong entry
 * for a cef-blink.library lookup - returning an empty resource rather than
 * failing, which surfaces as
 *   FATAL:pdf_extension_util.cc(205) Check failed:
 *       manifest_contents.find(kNameTag) != std::string::npos
 * Staged from out/aros-cef-bootstrap.  See test_electron_lifecycle.c.
 */
static cef_char_t resources_path[] = u"Developer:CEF";
static cef_char_t locales_path[] = u"Developer:CEF/locales";
static cef_char_t root_cache_path[] = u"T:CEFSharedLibrary";
static cef_char_t cache_path[] = u"T:CEFSharedLibrary/Default";
static cef_char_t test_url[] =
    u"data:text/html,<html><body style='margin:0;background:%23151a22;"
    u"display:flex;align-items:center;justify-content:center;height:100vh'>"
    u"<div style='width:480px;height:180px;background:%23f5f7fa;"
    u"border:24px solid %2300c7f2'></div></body></html>";

static size_t append_decimal(char *output, ULONG value)
{
    char reversed[16];
    size_t digits = 0;
    size_t index;

    do
    {
        reversed[digits++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0);

    for (index = 0; index < digits; ++index)
        output[index] = reversed[digits - index - 1];

    return digits;
}

static int write_snapshot_ppm(const void *buffer, int width, int height)
{
    const unsigned char *source = (const unsigned char *)buffer;
    unsigned char *row;
    char header[64];
    size_t header_length = 0;
    BPTR output;
    LONG y;
    LONG x;
    int result = 0;

    if (snapshot_path == NULL || width <= 0 || height <= 0)
        return 0;

    output = Open(snapshot_path, MODE_NEWFILE);
    if (output == BNULL)
        return 0;

    header[header_length++] = 'P';
    header[header_length++] = '6';
    header[header_length++] = '\n';
    header_length += append_decimal(header + header_length, (ULONG)width);
    header[header_length++] = ' ';
    header_length += append_decimal(header + header_length, (ULONG)height);
    header[header_length++] = '\n';
    header[header_length++] = '2';
    header[header_length++] = '5';
    header[header_length++] = '5';
    header[header_length++] = '\n';

    row = AllocVec((IPTR)width * 3, MEMF_ANY);
    if (row != NULL &&
        Write(output, header, (LONG)header_length) == (LONG)header_length)
    {
        result = 1;
        for (y = 0; y < height && result; ++y)
        {
            const unsigned char *source_row =
                source + ((IPTR)y * (IPTR)width * 4);

            for (x = 0; x < width; ++x)
            {
                row[(IPTR)x * 3] = source_row[(IPTR)x * 4 + 2];
                row[(IPTR)x * 3 + 1] = source_row[(IPTR)x * 4 + 1];
                row[(IPTR)x * 3 + 2] = source_row[(IPTR)x * 4];
            }
            result = Write(output, row, width * 3) == width * 3;
        }
    }

    if (row != NULL)
        FreeVec(row);
    Close(output);
    return result;
}

static int frame_has_test_content(const void *buffer, int width, int height)
{
    const unsigned char *pixels = (const unsigned char *)buffer;
    IPTR pixel_count = (IPTR)width * (IPTR)height;
    IPTR pixel;
    ULONG bright_pixels = 0;

    /*
     * The test document has a dark #151a22 background and light text.  Do not
     * accept the compositor's initial background-only frame as page content.
     */
    for (pixel = 0; pixel < pixel_count; ++pixel)
    {
        const unsigned char *bgra = pixels + (pixel * 4);

        if (bgra[0] > 180 && bgra[1] > 180 && bgra[2] > 180 &&
            ++bright_pixels >= 64)
            return 1;
    }

    return 0;
}

static void set_static_cef_string(cef_string_t *target, cef_char_t *value,
    size_t length)
{
    target->str = value;
    target->length = length;
    target->dtor = NULL;
}

/*
 * These handler objects have static process lifetime. CEF may retain and
 * release them, but there is no allocation to free and shutdown is the only
 * lifetime boundary for this focused client.
 */
static void CEF_CALLBACK static_add_ref(cef_base_ref_counted_t *self)
{
    (void)self;
}

static int CEF_CALLBACK static_release(cef_base_ref_counted_t *self)
{
    (void)self;
    return 0;
}

static int CEF_CALLBACK static_has_one_ref(cef_base_ref_counted_t *self)
{
    (void)self;
    return 1;
}

static int CEF_CALLBACK static_has_at_least_one_ref(
    cef_base_ref_counted_t *self)
{
    (void)self;
    return 1;
}

static void initialize_base(cef_base_ref_counted_t *base, size_t size)
{
    base->size = size;
    base->add_ref = static_add_ref;
    base->release = static_release;
    base->has_one_ref = static_has_one_ref;
    base->has_at_least_one_ref = static_has_at_least_one_ref;
}

static void release_browser_reference(cef_browser_t *cef_browser)
{
    if (cef_browser != NULL && cef_browser->base.release != NULL)
        cef_browser->base.release(&cef_browser->base);
}

static cef_life_span_handler_t *CEF_CALLBACK get_life_span_handler(
    cef_client_t *self)
{
    (void)self;
    return &life_span_handler;
}

static cef_render_handler_t *CEF_CALLBACK get_render_handler(
    cef_client_t *self)
{
    (void)self;
    return &render_handler;
}

static void CEF_CALLBACK get_view_rect(cef_render_handler_t *self,
    cef_browser_t *cef_browser, cef_rect_t *rect)
{
    (void)self;

    rect->x = 0;
    rect->y = 0;
    if (browser_window != NULL)
    {
        rect->width = browser_window->GZZWidth;
        rect->height = browser_window->GZZHeight;
    }
    else
    {
        rect->width = VIEW_WIDTH;
        rect->height = VIEW_HEIGHT;
    }

    release_browser_reference(cef_browser);
}

static void CEF_CALLBACK on_paint(cef_render_handler_t *self,
    cef_browser_t *cef_browser, cef_paint_element_type_t type,
    size_t dirty_rect_count, const cef_rect_t *dirty_rects,
    const void *buffer, int width, int height)
{
    int draw_width;
    int draw_height;

    (void)self;
    (void)dirty_rect_count;
    (void)dirty_rects;

    if (type != PET_VIEW || browser_window == NULL || buffer == NULL)
        goto release_argument;

    draw_width = width;
    draw_height = height;
    if (draw_width > browser_window->GZZWidth)
        draw_width = browser_window->GZZWidth;
    if (draw_height > browser_window->GZZHeight)
        draw_height = browser_window->GZZHeight;

    WritePixelArray((APTR)buffer, 0, 0, width * 4,
        browser_window->RPort, 0, 0, draw_width, draw_height, RECTFMT_BGRA32);

    if (!first_paint && frame_has_test_content(buffer, width, height))
    {
        if (snapshot_path != NULL)
        {
            if (write_snapshot_ppm(buffer, width, height))
                KPrintF("[CEF-BROWSER] snapshot wrote %s\n", snapshot_path);
            else
                KPrintF("[CEF-BROWSER] snapshot write failed %s\n",
                    snapshot_path);
        }
        first_paint_tick = pump_tick;
        first_paint = 1;
        KPrintF("[CEF-BROWSER] first paint %ldx%ld dirty=%lu\n",
            (LONG)width, (LONG)height, (ULONG)dirty_rect_count);
    }
    else if (!first_paint && !initial_frame_logged)
    {
        initial_frame_logged = 1;
        KPrintF("[CEF-BROWSER] initial frame has no test content; waiting\n");
    }

release_argument:
    release_browser_reference(cef_browser);
}

static void CEF_CALLBACK on_after_created(cef_life_span_handler_t *self,
    cef_browser_t *created_browser)
{
    (void)self;
    browser = created_browser;
    KPrintF("[CEF-BROWSER] browser created=%p\n", browser);
}

static void CEF_CALLBACK on_before_close(cef_life_span_handler_t *self,
    cef_browser_t *closing_browser)
{
    (void)self;

    release_browser_reference(closing_browser);
    release_browser_reference(browser);
    browser = NULL;
    browser_closed = 1;
    KPrintF("[CEF-BROWSER] browser closed\n");
}

static cef_browser_host_t *get_browser_host(void)
{
    if (browser == NULL || browser->get_host == NULL)
        return NULL;
    return browser->get_host(browser);
}

static void release_browser_host(cef_browser_host_t *host)
{
    if (host != NULL && host->base.release != NULL)
        host->base.release(&host->base);
}

static void notify_browser_resized(void)
{
    cef_browser_host_t *host = get_browser_host();
    if (host != NULL && host->was_resized != NULL)
        host->was_resized(host);
    release_browser_host(host);
}

static void invalidate_browser(void)
{
    cef_browser_host_t *host = get_browser_host();
    if (host != NULL && host->invalidate != NULL)
        host->invalidate(host, PET_VIEW);
    release_browser_host(host);
}

static void request_browser_close(void)
{
    cef_browser_host_t *host;

    if (close_requested)
        return;
    close_requested = 1;
    host = get_browser_host();
    if (host != NULL && host->close_browser != NULL)
        host->close_browser(host, 1);
    release_browser_host(host);
}

static void handle_window_messages(void)
{
    struct IntuiMessage *message;
    ULONG message_class;

    while ((message = (struct IntuiMessage *)
        GetMsg(browser_window->UserPort)) != NULL)
    {
        message_class = message->Class;
        ReplyMsg((struct Message *)message);

        switch (message_class)
        {
            case IDCMP_CLOSEWINDOW:
                request_browser_close();
                break;
            case IDCMP_NEWSIZE:
                notify_browser_resized();
                break;
            case IDCMP_REFRESHWINDOW:
                BeginRefresh(browser_window);
                EndRefresh(browser_window, TRUE);
                invalidate_browser();
                break;
        }
    }
}

static void initialize_handlers(void)
{
    memset(&client, 0, sizeof(client));
    memset(&render_handler, 0, sizeof(render_handler));
    memset(&life_span_handler, 0, sizeof(life_span_handler));

    initialize_base(&client.base, sizeof(client));
    client.get_life_span_handler = get_life_span_handler;
    client.get_render_handler = get_render_handler;

    initialize_base(&render_handler.base, sizeof(render_handler));
    render_handler.get_view_rect = get_view_rect;
    render_handler.on_paint = on_paint;

    initialize_base(&life_span_handler.base, sizeof(life_span_handler));
    life_span_handler.on_after_created = on_after_created;
    life_span_handler.on_before_close = on_before_close;
}

int main(int argc, char **argv)
{
    char *cef_argv[5];
    cef_main_args_t main_args;
    cef_settings_t settings;
    cef_window_info_t window_info;
    cef_browser_settings_t browser_settings;
    cef_string_t url;
    LONG execute_result;
    LONG initialize_result;
    LONG create_result;
    ULONG ticks;
    ULONG close_ticks;
    int result = RETURN_FAIL;

    if (argc > 1)
        snapshot_path = argv[1];
    CEFBase = OpenLibrary(CEFNAME, CEFVERSION);
    if (CEFBase == NULL)
    {
        KPrintF("[CEF-BROWSER] cef.library open failed\n");
        return RETURN_FAIL;
    }

    browser_window = OpenWindowTags(NULL,
        WA_Title, (IPTR)"CEF Shared-Library Browser",
        WA_InnerWidth, VIEW_WIDTH,
        WA_InnerHeight, VIEW_HEIGHT,
        WA_AutoAdjust, TRUE,
        WA_GimmeZeroZero, TRUE,
        WA_Activate, TRUE,
        WA_DragBar, TRUE,
        WA_DepthGadget, TRUE,
        WA_CloseGadget, TRUE,
        WA_SizeGadget, TRUE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_NEWSIZE | IDCMP_REFRESHWINDOW,
        TAG_DONE);
    if (browser_window == NULL)
    {
        KPrintF("[CEF-BROWSER] Intuition window open failed\n");
        goto close_library;
    }

    initialize_handlers();

    cef_argv[0] = argv[0];
    cef_argv[1] = (char *)"--single-process";
    cef_argv[2] = (char *)"--disable-gpu";
    cef_argv[3] = (char *)"--disable-gpu-compositing";
    cef_argv[4] = NULL;
    main_args.argc = 4;
    main_args.argv = cef_argv;

    memset(&settings, 0, sizeof(settings));
    settings.size = sizeof(settings);
    settings.no_sandbox = 1;
    settings.windowless_rendering_enabled = 1;
    set_static_cef_string(&settings.root_cache_path, root_cache_path,
        (sizeof(root_cache_path) / sizeof(root_cache_path[0])) - 1);
    set_static_cef_string(&settings.cache_path, cache_path,
        (sizeof(cache_path) / sizeof(cache_path[0])) - 1);
    set_static_cef_string(&settings.resources_dir_path, resources_path,
        (sizeof(resources_path) / sizeof(resources_path[0])) - 1);
    set_static_cef_string(&settings.locales_dir_path, locales_path,
        (sizeof(locales_path) / sizeof(locales_path[0])) - 1);

    execute_result = CEFExecuteProcess(&main_args, NULL, NULL);
    KPrintF("[CEF-BROWSER] execute rc=%ld\n", execute_result);
    if (execute_result >= 0)
    {
        result = (int)execute_result;
        goto close_window;
    }

    initialize_result = CEFInitialize(&main_args, &settings, NULL, NULL);
    KPrintF("[CEF-BROWSER] initialize rc=%ld\n", initialize_result);
    if (initialize_result == 0)
        goto close_window;

    memset(&window_info, 0, sizeof(window_info));
    window_info.bounds.width = browser_window->GZZWidth;
    window_info.bounds.height = browser_window->GZZHeight;
    window_info.windowless_rendering_enabled = 1;

    memset(&browser_settings, 0, sizeof(browser_settings));
    browser_settings.size = sizeof(browser_settings);

    memset(&url, 0, sizeof(url));
    set_static_cef_string(&url, test_url,
        (sizeof(test_url) / sizeof(test_url[0])) - 1);

    create_result = CEFCreateBrowser(&window_info, &client, &url,
        &browser_settings, NULL, NULL);
    KPrintF("[CEF-BROWSER] create rc=%ld\n", create_result);
    if (create_result == 0)
        goto shutdown;

    for (ticks = 0; ticks < CREATE_TIMEOUT_TICKS && !browser_closed; ++ticks)
    {
        pump_tick = ticks;
        handle_window_messages();
        CEFDoMessageLoopWork();

        if (first_paint &&
            ticks > first_paint_tick + CLOSE_AFTER_PAINT_TICKS)
            request_browser_close();
        Delay(1);
    }

    if (!browser_closed)
        request_browser_close();

    for (close_ticks = 0;
        close_ticks < CLOSE_TIMEOUT_TICKS && !browser_closed; ++close_ticks)
    {
        handle_window_messages();
        CEFDoMessageLoopWork();
        Delay(1);
    }

    if (first_paint && browser_closed)
    {
        KPrintF("[CEF-BROWSER] PASS painted and closed cleanly\n");
        result = RETURN_OK;
    }
    else
    {
        KPrintF("[CEF-BROWSER] FAIL painted=%ld closed=%ld\n",
            (LONG)first_paint, (LONG)browser_closed);
    }

shutdown:
    CEFShutdown();

close_window:
    CloseWindow(browser_window);
    browser_window = NULL;

close_library:
    CloseLibrary(CEFBase);
    CEFBase = NULL;
    return result;
}
