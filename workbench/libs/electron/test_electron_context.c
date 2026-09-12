/*
 * First simultaneous CEF and Node context probe.
 *
 * CEF owns V8 initialization and creates the renderer isolate/context.  The
 * renderer callback then asks node.library, through electron.library, to
 * create a Node Environment in that already-current context.  No engine
 * archive is linked into this client or electron.library.
 */

#include <stdlib.h>
#include <string.h>

#include <dos/dosextens.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <exec/types.h>
#include <proto/debug.h>
#include <proto/dos.h>
#include <proto/electron.h>
#include <proto/exec.h>

#include "include/capi/cef_app_capi.h"
#include "include/capi/cef_browser_capi.h"
#include "include/capi/cef_client_capi.h"
#include "include/capi/cef_display_handler_capi.h"
#include "include/capi/cef_frame_capi.h"
#include "include/capi/cef_life_span_handler_capi.h"
#include "include/capi/cef_load_handler_capi.h"
#include "include/capi/cef_render_handler_capi.h"
#include "include/capi/cef_render_process_handler_capi.h"
#include "include/capi/cef_v8_capi.h"

#define VIEW_WIDTH 640
#define VIEW_HEIGHT 360
#define NODE_PUMP_TICKS 64
#define CLOSE_AFTER_PAINT_TICKS 100
#define CREATE_TIMEOUT_TICKS 30000
#define CLOSE_TIMEOUT_TICKS 500
#define SHUTDOWN_DRAIN_TICKS 200
#define MAIN_SCRIPT_OPTION "--main-script="
static cef_app_t app;
static cef_client_t client;
static cef_display_handler_t display_handler;
static cef_life_span_handler_t life_span_handler;
static cef_load_handler_t load_handler;
static cef_render_handler_t render_handler;
static cef_render_process_handler_t render_process_handler;
static cef_v8handler_t node_pump_handler;

static cef_browser_t *browser;
static cef_v8context_t *node_context;
static struct Task *node_context_task;
static struct MsgPort *ui_pump_port;
static struct Message ui_pump_message;
static int browser_closed;
static int close_requested;
static int exit_requested;
static LONG requested_exit_code;
static int node_attached;
static int node_detached;
static int node_task_mismatch;
static int node_marker_seen;
static int completion_queued;
static int external_url_mode;
static ULONG node_pump_ticks;
static int first_paint;
static ULONG first_paint_tick;
static ULONG pump_tick;

static cef_char_t resources_path[] = u"Developer:Chromium";
static cef_char_t locales_path[] = u"Developer:Chromium/locales";
static cef_char_t root_cache_path[] = u"T:ElectronContext";
static cef_char_t cache_path[] = u"T:ElectronContext/Default";
#ifdef ELECTRON_SHELL_WINDOWED
static cef_char_t test_url[] =
    u"data:text/html,<html><head><title>AROS Electron Shell</title>"
    u"<style>"
    u"html,body{margin:0;width:100%;height:100%;overflow:hidden;"
    u"background:rgb(20,26,36);color:rgb(239,244,255);"
    u"font-family:sans-serif}"
    u"body{display:flex;align-items:center;justify-content:center}"
    u"main{width:78%;padding:48px;border:1px solid rgb(54,78,104);"
    u"background:rgb(27,36,50);box-shadow:0 18px 60px rgb(5,8,14)}"
    u"h1{margin:0 0 12px;font-size:44px;font-weight:500;"
    u"color:rgb(64,203,255)}"
    u"p{font-size:21px;line-height:1.5;margin:8px 0}"
    u".ok{margin-top:30px;padding:14px 18px;background:rgb(34,50,68);"
    u"border-left:5px solid rgb(64,203,255)}"
    u"</style></head><body><main><h1>Electron on AROS</h1>"
    u"<p>CEF is rendering this native window.</p>"
    u"<p>Node is attached to the live Blink V8 context through "
    u"electron.library.</p>"
    u"<div class='ok'>Shared CEF + shared Node + shared V8</div>"
    u"</main></body></html>";
static cef_char_t command_url[2048];
static cef_char_t window_title[] = u"AROS Electron Shell";
static unsigned char command_node_script[4096];
#else
static cef_char_t test_url[] =
    u"data:text/html,<html><body>AROS Electron Node Context</body></html>";
#endif
static cef_char_t extension_name[] = u"aros/electron_node_pump";
static cef_char_t extension_code[] =
    u"Object.defineProperty(globalThis, 'AROSElectron', {"
    u"value: {}, writable: false, configurable: false"
    u"});"
    u"(function() {"
    u"globalThis.AROSElectron.pumpNode = function() {"
    u"native function AROSPumpNodeNative();"
    u"return AROSPumpNodeNative();"
    u"};"
    u"})();";
static cef_char_t timer_code[] =
    u"globalThis.__arosNodePumpTimer = setInterval(function() {"
    u"globalThis.AROSElectron.pumpNode();"
    u"}, 1);";
static cef_char_t timer_script_url[] = u"aros://electron/node-pump";
static const unsigned char node_script[] =
    "const {app,native,BrowserWindow,ipcMain,nativeTheme,powerMonitor,screen,"
    "protocol}=require('electron');"
    "if(app.isReady())throw new Error('Electron app became ready synchronously');"
    "const primaryDisplay=screen.getPrimaryDisplay();"
    "if(!primaryDisplay||primaryDisplay.bounds.width<1||"
    "primaryDisplay.bounds.height<1)"
    "throw new Error('Electron screen snapshot failed');"
    "if(typeof powerMonitor.on!=='function')"
    "throw new Error('Electron powerMonitor event contract failed');"
    "protocol.registerFileProtocol('aros-test',(_request,callback)=>"
    "callback({path:'T:ElectronProtocolTest'}));"
    "protocol.registerBufferProtocol('aros-buffer-test',(_request,callback)=>"
    "callback({data:Buffer.from('AROS')}));"
    "protocol.registerHttpProtocol('aros-http-test',(_request,callback)=>"
    "callback({url:'https://aros.org/'}));"
    "if(nativeTheme.themeSource!=='system')"
    "throw new Error('Electron nativeTheme default source mismatch');"
    "let themeUpdates=0;"
    "nativeTheme.on('updated',()=>{themeUpdates++;});"
    "nativeTheme.themeSource='dark';"
    "if(!nativeTheme.shouldUseDarkColors||themeUpdates!==1)"
    "throw new Error('Electron nativeTheme update failed');"
    "nativeTheme.themeSource='system';"
    "let ipcMessage='';"
    "ipcMain.on('aros-test-message',(_event,value)=>{ipcMessage=value;});"
    "ipcMain._arosDispatchMessage('aros-test-message',{},'message-pass');"
    "if(ipcMessage!=='message-pass')"
    "throw new Error('Electron ipcMain message dispatch failed');"
    "ipcMain.handle('aros-test-invoke',(_event,value)=>value+'-pass');"
    "ipcMain._arosDispatchInvoke('aros-test-invoke',{},'invoke').then(value=>{"
    "if(value!=='invoke-pass')"
    "throw new Error('Electron ipcMain invoke dispatch failed');"
    "process._rawDebug('[ELECTRON-MODULE] ipcMain dispatch pass');"
    "});"
    "const nativeResult=native.ping();"
    "if(nativeResult!=='AROS Electron native binding')"
    "throw new Error('Electron native binding mismatch: '+nativeResult);"
    "process._rawDebug('[ELECTRON-MODULE] require electron app='+"
    "app.getName()+' version='+app.getVersion()+' native='+nativeResult);"
#ifdef ELECTRON_SHELL_WINDOWED
    "app.whenReady().then(()=>{"
    "if(!app.isReady())throw new Error('Electron ready promise resolved early');"
    "process._rawDebug('[ELECTRON-MODULE] app ready');"
    "const mainWindow=new BrowserWindow({width:640,height:360});"
    "if(typeof mainWindow.webContents.on!=='function')"
    "throw new Error('Electron webContents event contract failed');"
    "if(BrowserWindow.getFocusedWindow()!==mainWindow)"
    "throw new Error('Electron focused window contract failed');"
    "mainWindow.setTitle('AROS Electron Context');"
    "if(mainWindow.getTitle()!=='AROS Electron Context'||mainWindow.isDestroyed())"
    "throw new Error('Electron window title/lifecycle contract failed');"
    "if(!mainWindow.isVisible()||mainWindow.isMinimized())"
    "throw new Error('Electron window visibility default failed');"
    "mainWindow.hide();"
    "if(mainWindow.isVisible())"
    "throw new Error('Electron window hide state failed');"
    "mainWindow.show();"
    "if(!mainWindow.isVisible())"
    "throw new Error('Electron window show state failed');"
    "mainWindow.minimize();"
    "if(!mainWindow.isMinimized()||mainWindow.isVisible())"
    "throw new Error('Electron window minimize state failed');"
    "mainWindow.restore();"
    "if(mainWindow.isMinimized()||!mainWindow.isVisible())"
    "throw new Error('Electron window restore state failed');"
    "mainWindow.focus();"
    "if(mainWindow.isMaximized()||mainWindow.webContents.isDestroyed())"
    "throw new Error('Electron native lifecycle default failed');"
    "mainWindow.maximize();"
    "if(!mainWindow.isMaximized())"
    "throw new Error('Electron maximize state failed');"
    "if(mainWindow.isFullScreen())"
    "throw new Error('Electron fullscreen default failed');"
    "mainWindow.setFullScreen(true);"
    "if(!mainWindow.isFullScreen())"
    "throw new Error('Electron fullscreen state failed');"
    "if(mainWindow.isSimpleFullScreen())"
    "throw new Error('Electron simple fullscreen default failed');"
    "mainWindow.setSimpleFullScreen(true);"
    "if(!mainWindow.isSimpleFullScreen())"
    "throw new Error('Electron simple fullscreen state failed');"
    "mainWindow.setMenuBarVisibility(false);"
    "if(mainWindow.isMenuBarVisible())"
    "throw new Error('Electron menu visibility state failed');"
    "mainWindow.loadURL('aros-electron:startup');"
    "});"
#endif
    "const fs=require('fs');"
    "let arosTicks=0;"
    "const arosStep=()=>{"
    "if(++arosTicks===3){"
    "fs.writeFileSync('T:ElectronNodePersistent.pass',"
    "'ELECTRON_NODE_PERSISTENT_PASS '+process.version+' '+"
    "app.getName()+' '+app.getVersion()+'\\n');"
    "}else{setImmediate(arosStep);}"
    "};"
    "setImmediate(arosStep);"
    "globalThis.AROSNodeVersion=process.version;";

static void set_static_cef_string(cef_string_t *target, cef_char_t *value,
    size_t length)
{
    target->str = value;
    target->length = length;
    target->dtor = NULL;
}

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

static void log_cef_string(const char *label, const cef_string_t *value)
{
    char text[512];
    size_t i;
    size_t length = value != NULL ? value->length : 0;

    if (length >= sizeof(text))
        length = sizeof(text) - 1;
    for (i = 0; i < length; ++i)
    {
        cef_char_t character = value->str[i];
        text[i] = character <= 0x7f ? (char)character : '?';
    }
    text[length] = '\0';
    KPrintF("[ELECTRON-CONTEXT] %s '%s'\n", label, text);
}

static int marker_exists(void)
{
    BPTR marker = Lock("T:ElectronNodePersistent.pass", ACCESS_READ);

    if (marker == BNULL)
        return 0;
    UnLock(marker);
    return 1;
}

static void release_node_context(void)
{
    if (node_context != NULL && node_context->base.release != NULL)
        node_context->base.release(&node_context->base);
    node_context = NULL;
}

static void finish_node_on_renderer(void)
{
    ElectronDetachNodeEmbedder();
    node_detached = 1;
    KPrintF("[ELECTRON-CONTEXT] persistent Node detached after %lu pumps\n",
        node_pump_ticks);
}

static void release_browser_reference(cef_browser_t *cef_browser)
{
    if (cef_browser != NULL && cef_browser->base.release != NULL)
        cef_browser->base.release(&cef_browser->base);
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

static void service_renderer_pump_port(void);

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

static void drain_cef_before_shutdown(void)
{
    ULONG drain_ticks;

    for (drain_ticks = 0; drain_ticks < SHUTDOWN_DRAIN_TICKS; ++drain_ticks)
    {
        ElectronDoCEFMessageLoopWork();
        service_renderer_pump_port();
        Delay(1);
    }
    KPrintF("[ELECTRON-CONTEXT] CEF shutdown drain ticks=%lu\n",
        (unsigned long)SHUTDOWN_DRAIN_TICKS);
}

static void pump_node_on_context_task(void)
{
    struct Task *current_task = FindTask(NULL);
    LONG pump_rc;

    if (!node_attached || node_detached)
        return;
    if (current_task != node_context_task)
    {
        node_task_mismatch = 1;
        KPrintF("[ELECTRON-CONTEXT] Node pump task mismatch current=%p "
            "context=%p\n", current_task, node_context_task);
        return;
    }

    pump_rc = ElectronPumpNodeEmbedder();
    ++node_pump_ticks;
    if ((node_pump_ticks & 7UL) == 0)
        KPrintF("[ELECTRON-CONTEXT] direct Node pump=%lu rc=%ld\n",
            node_pump_ticks, pump_rc);

    if (node_pump_ticks >= NODE_PUMP_TICKS)
    {
        node_marker_seen = marker_exists();
        finish_node_on_renderer();
        release_node_context();
    }
}

static void service_renderer_pump_port(void)
{
    struct Message *message;

    if (ui_pump_port == NULL)
        return;
    message = GetMsg(ui_pump_port);
    if (message == NULL)
        return;
    KPrintF("[ELECTRON-CONTEXT] UI dequeued renderer handoff pump=%lu\n",
        node_pump_ticks);

    if (node_detached && !external_url_mode)
    {
        KPrintF("[ELECTRON-CONTEXT] renderer completion reached UI task\n");
        request_browser_close();
    }
}

#ifdef ELECTRON_SHELL_WINDOWED
static void service_native_window_requests(cef_window_info_t *window_info,
    cef_browser_settings_t *browser_settings,
    cef_char_t *browser_url, size_t browser_url_length)
{
    char method[64];
    char payload[2048];
    int create_requested = 0;
    LONG rc;

    while ((rc = ElectronPollNativeRequest(method, sizeof(method),
        payload, sizeof(payload))) > 0)
    {
        KPrintF("[ELECTRON-CONTEXT] native request method='%s' "
            "payload='%s'\n", method, payload);
        if (strcmp(method, "browser-window.create") == 0)
            create_requested = 1;
        else if (strcmp(method, "browser-window.load-url") == 0)
        {
            const char *requested_url = strchr(payload, '\t');
            size_t i;
            size_t output_offset = 0;

            create_requested = 1;
            if (requested_url != NULL && requested_url[1] != '\0')
            {
                ++requested_url;
                /*
                 * BrowserWindow accepts native paths as well as URLs.  AROS
                 * volume syntax looks like a URL scheme to CEF, so convert it
                 * to a file URL before crossing the Electron/CEF seam.
                 */
                if (strstr(requested_url, "://") == NULL &&
                    strchr(requested_url, ':') != NULL)
                {
                    static const char file_prefix[] = "file:///";

                    for (i = 0; file_prefix[i] != '\0'; ++i)
                        command_url[i] = (unsigned char)file_prefix[i];
                    output_offset = i;
                }
                for (i = 0; requested_url[i] != '\0' &&
                    output_offset + i + 1 < (sizeof(command_url) /
                        sizeof(command_url[0])); ++i)
                    command_url[output_offset + i] =
                        (unsigned char)requested_url[i];
                command_url[output_offset + i] = 0;
                browser_url = command_url;
                browser_url_length = output_offset + i;
                KPrintF("[ELECTRON-CONTEXT] BrowserWindow requested URL "
                    "'%s' routed through CEF file URL length=%lu\n",
                    requested_url, (unsigned long)browser_url_length);
            }
        }
        else if (strcmp(method, "app.exit") == 0)
        {
            requested_exit_code = strtol(payload, NULL, 10);
            exit_requested = 1;
            KPrintF("[ELECTRON-CONTEXT] app.exit requested code=%ld\n",
                requested_exit_code);
            if (browser != NULL)
                request_browser_close();
            else
                browser_closed = 1;
        }
    }

    if (create_requested && browser == NULL)
    {
        cef_string_t url;

        memset(&url, 0, sizeof(url));
        set_static_cef_string(&url, browser_url, browser_url_length);
        rc = ElectronCreateCEFBrowser(window_info, &client, &url,
            browser_settings, NULL, NULL);
        KPrintF("[ELECTRON-CONTEXT] BrowserWindow create rc=%ld\n", rc);
    }
}

static void pump_node_on_main_task(void)
{
    LONG rc;

    if (!node_attached || node_detached)
        return;
    rc = ElectronPumpNodeEmbedder();
    ++node_pump_ticks;
    if ((node_pump_ticks & 7UL) == 0)
        KPrintF("[ELECTRON-CONTEXT] main Node pump=%lu rc=%ld\n",
            node_pump_ticks, rc);
    if (!node_marker_seen && marker_exists())
        node_marker_seen = 1;
}
#endif

static int CEF_CALLBACK execute_node_pump(cef_v8handler_t *self,
    const cef_string_t *name, cef_v8value_t *object, size_t arguments_count,
    cef_v8value_t *const *arguments, cef_v8value_t **retval,
    cef_string_t *exception)
{
    (void)self;
    (void)name;
    (void)object;
    (void)arguments_count;
    (void)arguments;
    (void)retval;
    (void)exception;

    pump_node_on_context_task();
    if (node_detached && !completion_queued && ui_pump_port != NULL)
    {
        completion_queued = 1;
        PutMsg(ui_pump_port, &ui_pump_message);
        KPrintF("[ELECTRON-CONTEXT] renderer queued UI completion\n");
    }
    return 1;
}

static void CEF_CALLBACK on_web_kit_initialized(
    cef_render_process_handler_t *self)
{
    cef_string_t name;
    cef_string_t code;
    LONG rc;

    (void)self;
    memset(&name, 0, sizeof(name));
    memset(&code, 0, sizeof(code));
    set_static_cef_string(&name, extension_name,
        (sizeof(extension_name) / sizeof(extension_name[0])) - 1);
    set_static_cef_string(&code, extension_code,
        (sizeof(extension_code) / sizeof(extension_code[0])) - 1);
    rc = ElectronRegisterCEFExtension(&name, &code, &node_pump_handler);
    KPrintF("[ELECTRON-CONTEXT] register renderer extension rc=%ld task=%p\n",
        rc, FindTask(NULL));
}

static cef_render_process_handler_t *CEF_CALLBACK
get_render_process_handler(cef_app_t *self)
{
    (void)self;
    return &render_process_handler;
}

static int CEF_CALLBACK on_console_message(
    cef_display_handler_t *self,
    cef_browser_t *cef_browser,
    cef_log_severity_t level,
    const cef_string_t *message,
    const cef_string_t *source,
    int line)
{
    (void)self;
    (void)cef_browser;
    KPrintF("[ELECTRON-RENDERER] console level=%ld line=%ld\n",
        (LONG)level, (LONG)line);
    log_cef_string("renderer message", message);
    log_cef_string("renderer source", source);
    return 0;
}

static cef_display_handler_t *CEF_CALLBACK get_display_handler(
    cef_client_t *self)
{
    (void)self;
    return &display_handler;
}

static cef_life_span_handler_t *CEF_CALLBACK get_life_span_handler(
    cef_client_t *self)
{
    (void)self;
    return &life_span_handler;
}

static cef_load_handler_t *CEF_CALLBACK get_load_handler(cef_client_t *self)
{
    (void)self;
    return &load_handler;
}

static cef_render_handler_t *CEF_CALLBACK get_render_handler(
    cef_client_t *self)
{
    (void)self;
    return &render_handler;
}

static void CEF_CALLBACK on_loading_state_change(
    cef_load_handler_t *self, cef_browser_t *cef_browser, int is_loading,
    int can_go_back, int can_go_forward)
{
    (void)self;
    (void)cef_browser;
    KPrintF("[ELECTRON-CONTEXT] loading state loading=%ld back=%ld "
        "forward=%ld\n", (LONG)is_loading, (LONG)can_go_back,
        (LONG)can_go_forward);
}

static void CEF_CALLBACK on_load_start(cef_load_handler_t *self,
    cef_browser_t *cef_browser, cef_frame_t *frame,
    cef_transition_type_t transition_type)
{
    int is_main = frame != NULL && frame->is_main != NULL
        ? frame->is_main(frame) : 0;

    (void)self;
    (void)cef_browser;
    KPrintF("[ELECTRON-CONTEXT] load start main=%ld transition=%lu\n",
        (LONG)is_main, (ULONG)transition_type);
}

static void CEF_CALLBACK on_load_end(cef_load_handler_t *self,
    cef_browser_t *cef_browser, cef_frame_t *frame, int http_status_code)
{
    int is_main = frame != NULL && frame->is_main != NULL
        ? frame->is_main(frame) : 0;

    (void)self;
    (void)cef_browser;
    KPrintF("[ELECTRON-CONTEXT] load end main=%ld HTTP=%ld\n",
        (LONG)is_main, (LONG)http_status_code);
}

static void CEF_CALLBACK on_load_error(cef_load_handler_t *self,
    cef_browser_t *cef_browser, cef_frame_t *frame,
    cef_errorcode_t error_code, const cef_string_t *error_text,
    const cef_string_t *failed_url)
{
    int is_main = frame != NULL && frame->is_main != NULL
        ? frame->is_main(frame) : 0;

    (void)self;
    (void)cef_browser;
    KPrintF("[ELECTRON-CONTEXT] load error main=%ld code=%ld\n",
        (LONG)is_main, (LONG)error_code);
    log_cef_string("load error text", error_text);
    log_cef_string("load error URL", failed_url);
}

static void CEF_CALLBACK get_view_rect(cef_render_handler_t *self,
    cef_browser_t *cef_browser, cef_rect_t *rect)
{
    (void)self;
    rect->x = 0;
    rect->y = 0;
    rect->width = VIEW_WIDTH;
    rect->height = VIEW_HEIGHT;
    release_browser_reference(cef_browser);
}

static void CEF_CALLBACK on_paint(cef_render_handler_t *self,
    cef_browser_t *cef_browser, cef_paint_element_type_t type,
    size_t dirty_rect_count, const cef_rect_t *dirty_rects,
    const void *buffer, int width, int height)
{
    (void)self;
    (void)type;
    (void)dirty_rect_count;
    (void)dirty_rects;
    (void)buffer;
    if (!first_paint && width > 0 && height > 0)
    {
        first_paint = 1;
        first_paint_tick = pump_tick;
        KPrintF("[ELECTRON-CONTEXT] first paint=%ldx%ld tick=%lu\n",
            (LONG)width, (LONG)height, first_paint_tick);
    }
    release_browser_reference(cef_browser);
}

static void CEF_CALLBACK on_after_created(cef_life_span_handler_t *self,
    cef_browser_t *created_browser)
{
    (void)self;
    browser = created_browser;
    KPrintF("[ELECTRON-CONTEXT] browser created=%p\n", browser);
#ifdef ELECTRON_SHELL_WINDOWED
    first_paint = 1;
    first_paint_tick = pump_tick;
    KPrintF("[ELECTRON-CONTEXT] visible browser ready tick=%lu\n",
        first_paint_tick);
#endif
}

static void CEF_CALLBACK on_before_close(cef_life_span_handler_t *self,
    cef_browser_t *closing_browser)
{
    (void)self;
    release_browser_reference(closing_browser);
    release_browser_reference(browser);
    browser = NULL;
    browser_closed = 1;
    KPrintF("[ELECTRON-CONTEXT] browser closed\n");
}

static void CEF_CALLBACK on_context_created(
    cef_render_process_handler_t *self,
    cef_browser_t *cef_browser,
    cef_frame_t *frame,
    cef_v8context_t *context)
{
    LONG rc;

#ifdef ELECTRON_SHELL_WINDOWED
    (void)self;
    (void)cef_browser;
    (void)frame;
    (void)context;
    return;
#else
    (void)self;
    (void)cef_browser;
    (void)frame;
    (void)context;

    if (node_attached)
        return;
    KPrintF("[ELECTRON-CONTEXT] attach current V8 context begin\n");
    rc = ElectronAttachNodeIsolatedV8ContextWithModule(node_script);
    KPrintF("[ELECTRON-CONTEXT] attach current V8 context end rc=%ld\n", rc);
    if (rc == 0)
    {
        cef_string_t code;
        cef_string_t script_url;
        cef_v8value_t *retval = NULL;
        cef_v8exception_t *exception = NULL;
        int eval_rc = 0;

        node_attached = 1;
        node_context_task = FindTask(NULL);
        node_context = context;
        if (node_context->base.add_ref != NULL)
            node_context->base.add_ref(&node_context->base);
        KPrintF("[ELECTRON-CONTEXT] context task=%p\n", node_context_task);
        memset(&code, 0, sizeof(code));
        memset(&script_url, 0, sizeof(script_url));
        set_static_cef_string(&code, timer_code,
            (sizeof(timer_code) / sizeof(timer_code[0])) - 1);
        set_static_cef_string(&script_url, timer_script_url,
            (sizeof(timer_script_url) / sizeof(timer_script_url[0])) - 1);
        if (node_context->eval != NULL)
            eval_rc = node_context->eval(node_context, &code, &script_url, 1,
                &retval, &exception);
        KPrintF("[ELECTRON-CONTEXT] start renderer timer rc=%ld "
            "exception=%p\n", (LONG)eval_rc, exception);
        if (retval != NULL && retval->base.release != NULL)
            retval->base.release(&retval->base);
        if (exception != NULL && exception->base.release != NULL)
            exception->base.release(&exception->base);
    }
#endif
}

static void CEF_CALLBACK on_context_released(
    cef_render_process_handler_t *self,
    cef_browser_t *cef_browser,
    cef_frame_t *frame,
    cef_v8context_t *context)
{
    (void)self;
    (void)cef_browser;
    (void)frame;
    (void)context;

#ifdef ELECTRON_SHELL_WINDOWED
    /*
     * BrowserWindow mode owns Node on the browser/UI task. Renderer and
     * subframe V8 contexts may come and go for the lifetime of that main
     * environment, so their release must not tear it down from a renderer
     * callback. The browser task detaches Node after the window closes.
     */
    return;
#endif

    if (!node_attached || node_detached)
        return;
    KPrintF("[ELECTRON-CONTEXT] detach current V8 context begin\n");
    ElectronDetachNodeEmbedder();
    node_detached = 1;
    release_node_context();
    KPrintF("[ELECTRON-CONTEXT] detach current V8 context end\n");
}

static void initialize_handlers(void)
{
    memset(&app, 0, sizeof(app));
    memset(&client, 0, sizeof(client));
    memset(&display_handler, 0, sizeof(display_handler));
    memset(&life_span_handler, 0, sizeof(life_span_handler));
    memset(&load_handler, 0, sizeof(load_handler));
    memset(&render_handler, 0, sizeof(render_handler));
    memset(&render_process_handler, 0, sizeof(render_process_handler));
    memset(&node_pump_handler, 0, sizeof(node_pump_handler));

    initialize_base(&app.base, sizeof(app));
    app.get_render_process_handler = get_render_process_handler;

    initialize_base(&client.base, sizeof(client));
    client.get_display_handler = get_display_handler;
    client.get_life_span_handler = get_life_span_handler;
    client.get_load_handler = get_load_handler;
#ifndef ELECTRON_SHELL_WINDOWED
    client.get_render_handler = get_render_handler;
#endif

    initialize_base(&display_handler.base, sizeof(display_handler));
    display_handler.on_console_message = on_console_message;

    initialize_base(&life_span_handler.base, sizeof(life_span_handler));
    life_span_handler.on_after_created = on_after_created;
    life_span_handler.on_before_close = on_before_close;

    initialize_base(&load_handler.base, sizeof(load_handler));
    load_handler.on_loading_state_change = on_loading_state_change;
    load_handler.on_load_start = on_load_start;
    load_handler.on_load_end = on_load_end;
    load_handler.on_load_error = on_load_error;

    initialize_base(&render_handler.base, sizeof(render_handler));
    render_handler.get_view_rect = get_view_rect;
    render_handler.on_paint = on_paint;

    initialize_base(&render_process_handler.base,
        sizeof(render_process_handler));
    render_process_handler.on_web_kit_initialized =
        on_web_kit_initialized;
    render_process_handler.on_context_created = on_context_created;
    render_process_handler.on_context_released = on_context_released;

    initialize_base(&node_pump_handler.base, sizeof(node_pump_handler));
    node_pump_handler.execute = execute_node_pump;
}

static int ensure_profile_directory(CONST_STRPTR path)
{
    BPTR lock = Lock(path, ACCESS_READ);

    if (lock == BNULL)
        lock = CreateDir(path);
    if (lock == BNULL)
    {
        KPrintF("[ELECTRON-CONTEXT] unable to create profile directory "
            "'%s' IoErr=%ld\n", path, IoErr());
        return 0;
    }

    UnLock(lock);
    KPrintF("[ELECTRON-CONTEXT] profile directory ready '%s'\n", path);
    return 1;
}

#ifdef ELECTRON_SHELL_WINDOWED
static int make_node_main_script(const char *path)
{
    static const char prefix[] =
        "process.argv=['AROS-Electron','--no-sandbox',"
        "'--disable-gpu','--disable-gpu-compositing'];"
        "process.env.VSCODE_DEV='1';"
        "process._rawDebug('[ELECTRON-MODULE] loading main entry "
        MAIN_SCRIPT_OPTION "');"
        "const __arosElectronMain='";
    static const char suffix[] =
        "';const __arosRequire=require('module').createRequire("
        "__arosElectronMain);"
        "globalThis.__arosElectronMainModuleRequest=__arosElectronMain;"
        "__arosRequire(__arosElectronMain);";
    const size_t prefix_length = sizeof(prefix) - 1;
    const size_t suffix_length = sizeof(suffix) - 1;
    const size_t path_length = strlen(path);

    if (path_length == 0 ||
        strchr(path, '\'') != NULL ||
        prefix_length + path_length + suffix_length + 1 >
            sizeof(command_node_script))
        return 0;

    memcpy(command_node_script, prefix, prefix_length);
    memcpy(command_node_script + prefix_length, path, path_length);
    memcpy(command_node_script + prefix_length + path_length,
        suffix, suffix_length + 1);
    return 1;
}
#endif

int main(int argc, char **argv)
{
    /*
     * Preserve every native child-process switch and leave room for the two
     * AROS defaults plus the terminating NULL.  Chromium appends transport
     * switches (including Mojo) after its ordinary process switches, so a
     * fixed bring-up array silently converted a valid multi-process launch
     * into an invalid child command line.
     */
    char *cef_argv[argc + 3];
    cef_main_args_t main_args;
    cef_settings_t settings;
    cef_window_info_t window_info;
    cef_browser_settings_t browser_settings;
    cef_string_t url;
    cef_char_t *selected_url = test_url;
    size_t selected_url_length =
        (sizeof(test_url) / sizeof(test_url[0])) - 1;
    int external_url = 0;
    int cef_child_process = 0;
    int cef_argc;
    int arg_index;
    LONG rc;
    ULONG ticks;
    ULONG close_ticks;
    int result = 20;
    struct Process *process = (struct Process *)FindTask(NULL);
    APTR saved_window_ptr = NULL;
    int requesters_suppressed = 0;
    const unsigned char *selected_node_script = node_script;

#ifdef ELECTRON_SHELL_WINDOWED
    for (arg_index = 1; arg_index < argc; ++arg_index)
    {
        const size_t main_script_option_length =
            sizeof(MAIN_SCRIPT_OPTION) - 1;
        size_t i;

        if (argv[arg_index] == NULL || argv[arg_index][0] == '\0')
            continue;
        if (strncmp(argv[arg_index], "--type=", 7) == 0)
            cef_child_process = 1;
        if (strncmp(argv[arg_index], MAIN_SCRIPT_OPTION,
            main_script_option_length) == 0)
        {
            const char *main_script_path =
                argv[arg_index] + main_script_option_length;
            if (!make_node_main_script(main_script_path))
            {
                KPrintF("[ELECTRON-CONTEXT] invalid main script path '%s'\n",
                    main_script_path);
                return 20;
            }
            selected_node_script = command_node_script;
            KPrintF("[ELECTRON-CONTEXT] loading Node main script '%s'\n",
                main_script_path);
            continue;
        }
        if (argv[arg_index][0] == '-' && argv[arg_index][1] == '-')
            continue;
        if (external_url)
            continue;

        for (i = 0; argv[arg_index][i] != '\0' &&
            i + 1 < (sizeof(command_url) / sizeof(command_url[0])); ++i)
            command_url[i] = (unsigned char)argv[arg_index][i];
        command_url[i] = 0;
        selected_url = command_url;
        selected_url_length = i;
        external_url = 1;
        external_url_mode = 1;
        KPrintF("[ELECTRON-CONTEXT] loading command URL '%s'\n",
            argv[arg_index]);
    }
#else
    (void)argc;
#endif
    initialize_handlers();
    ui_pump_port = CreateMsgPort();
    if (ui_pump_port == NULL)
    {
        KPrintF("[ELECTRON-CONTEXT] unable to create UI pump port\n");
        return 20;
    }
    memset(&ui_pump_message, 0, sizeof(ui_pump_message));
    ui_pump_message.mn_Node.ln_Type = NT_MESSAGE;
    ui_pump_message.mn_Length = sizeof(ui_pump_message);

    if (!ensure_profile_directory("T:ElectronContext") ||
        !ensure_profile_directory("T:ElectronContext/Default"))
    {
        DeleteMsgPort(ui_pump_port);
        return 20;
    }

    if (external_url && process != NULL)
    {
        saved_window_ptr = process->pr_WindowPtr;
        process->pr_WindowPtr = (APTR)(SIPTR)-1;
        requesters_suppressed = 1;
        KPrintF("[ELECTRON-CONTEXT] DOS requesters suppressed window=%p\n",
            saved_window_ptr);
    }

    cef_argv[0] = argv[0];
    /*
     * Native AROS child launch and Mojo transport are now available.  Keep
     * --single-process as an explicitly forwarded compatibility option, not
     * the default Electron process model.
     */
    cef_argv[1] = (char *)"--disable-gpu";
    cef_argv[2] = (char *)"--disable-gpu-compositing";
    cef_argc = 3;
#ifdef ELECTRON_SHELL_WINDOWED
    for (arg_index = 1; arg_index < argc &&
        cef_argc + 1 < (int)(sizeof(cef_argv) / sizeof(cef_argv[0]));
        ++arg_index)
    {
        if (argv[arg_index] != NULL && argv[arg_index][0] == '-' &&
            argv[arg_index][1] == '-' &&
            strncmp(argv[arg_index], MAIN_SCRIPT_OPTION,
                sizeof(MAIN_SCRIPT_OPTION) - 1) != 0)
        {
            cef_argv[cef_argc++] = argv[arg_index];
            KPrintF("[ELECTRON-CONTEXT] forwarding CEF switch '%s'\n",
                argv[arg_index]);
        }
    }
#else
    (void)arg_index;
#endif
    cef_argv[cef_argc] = NULL;
    main_args.argc = cef_argc;
    main_args.argv = cef_argv;

    memset(&settings, 0, sizeof(settings));
    settings.size = sizeof(settings);
    settings.no_sandbox = 1;
#ifndef ELECTRON_SHELL_WINDOWED
    settings.windowless_rendering_enabled = 1;
#endif
    set_static_cef_string(&settings.root_cache_path, root_cache_path,
        (sizeof(root_cache_path) / sizeof(root_cache_path[0])) - 1);
    set_static_cef_string(&settings.cache_path, cache_path,
        (sizeof(cache_path) / sizeof(cache_path[0])) - 1);
    set_static_cef_string(&settings.resources_dir_path, resources_path,
        (sizeof(resources_path) / sizeof(resources_path[0])) - 1);
    set_static_cef_string(&settings.locales_dir_path, locales_path,
        (sizeof(locales_path) / sizeof(locales_path[0])) - 1);

    if (cef_child_process)
    {
        rc = ElectronExecuteCEFProcess(&main_args, &app, NULL);
        KPrintF("[ELECTRON-CONTEXT] child CEF execute rc=%ld\n", rc);
        if (rc >= 0)
        {
            DeleteMsgPort(ui_pump_port);
            return (int)rc;
        }
    }

    /*
     * Renderer children share electron.library with the browser process, but
     * they are Chromium roles rather than Node embedders.  Dispatch them
     * before touching the process-global Node engine; otherwise they observe
     * the browser's prepared Node state, return rc=20, and exit before Blink
     * can execute the document's first script.  Other child roles retain the
     * established bring-up behavior until their service lifecycle is proven
     * independently.
     */
    rc = ElectronPrepareNodeEmbedder();
    KPrintF("[ELECTRON-CONTEXT] prepare Node rc=%ld\n", rc);
    if (rc != 0)
    {
        DeleteMsgPort(ui_pump_port);
        return (int)rc;
    }

    if (!cef_child_process)
    {
        rc = ElectronExecuteCEFProcess(&main_args, &app, NULL);
        KPrintF("[ELECTRON-CONTEXT] CEF execute rc=%ld\n", rc);
        if (rc >= 0)
        {
            if (requesters_suppressed)
            {
                process->pr_WindowPtr = saved_window_ptr;
                KPrintF("[ELECTRON-CONTEXT] DOS requester window restored=%p\n",
                    saved_window_ptr);
            }
            DeleteMsgPort(ui_pump_port);
            return (int)rc;
        }
    }

    rc = ElectronInitializeCEF(&main_args, &settings, &app, NULL);
    KPrintF("[ELECTRON-CONTEXT] CEF initialize rc=%ld\n", rc);
    if (rc == 0)
        goto detach_node;

    memset(&window_info, 0, sizeof(window_info));
    window_info.bounds.width = VIEW_WIDTH;
    window_info.bounds.height = VIEW_HEIGHT;
#ifdef ELECTRON_SHELL_WINDOWED
    set_static_cef_string(&window_info.window_name, window_title,
        (sizeof(window_title) / sizeof(window_title[0])) - 1);
#else
    window_info.windowless_rendering_enabled = 1;
#endif
    memset(&browser_settings, 0, sizeof(browser_settings));
    browser_settings.size = sizeof(browser_settings);
    memset(&url, 0, sizeof(url));
    set_static_cef_string(&url, selected_url, selected_url_length);

#ifdef ELECTRON_SHELL_WINDOWED
    /*
     * Let Chromium install its browser-task V8 platform before Node creates
     * the owned main-process isolate in the same resident engine.
     */
    KPrintF("[ELECTRON-CONTEXT] priming CEF UI loop before Node attach\n");
    ElectronDoCEFMessageLoopWork();
    KPrintF("[ELECTRON-CONTEXT] primed CEF UI loop before Node attach\n");
    KPrintF("[ELECTRON-CONTEXT] Node main wrapper '%s'\n",
        selected_node_script);
    rc = ElectronAttachNodeOwnedV8ContextWithModule(selected_node_script);
    KPrintF("[ELECTRON-CONTEXT] attach owned main V8 context rc=%ld\n", rc);
    if (rc != 0)
        goto shutdown_cef;
    node_attached = 1;
    /* Preserve the UI task captured before CEF/Node initialize their workers. */
    node_context_task = (struct Task *)process;
    KPrintF("[ELECTRON-CONTEXT] entering initial native request service\n");
    service_native_window_requests(&window_info, &browser_settings,
        selected_url, selected_url_length);
    KPrintF("[ELECTRON-CONTEXT] completed initial native request service\n");
#else
    rc = ElectronCreateCEFBrowser(&window_info, &client, &url,
        &browser_settings, NULL, NULL);
    KPrintF("[ELECTRON-CONTEXT] create browser rc=%ld\n", rc);
    if (rc == 0)
        goto shutdown_cef;
#endif

    for (ticks = 0;
        ticks < CREATE_TIMEOUT_TICKS && !browser_closed &&
            !exit_requested; ++ticks)
    {
        pump_tick = ticks;
        if (ticks == 0)
            KPrintF("[ELECTRON-CONTEXT] entering first CEF message-loop pump\n");
        ElectronDoCEFMessageLoopWork();
        if (ticks == 0)
            KPrintF("[ELECTRON-CONTEXT] completed first CEF message-loop pump\n");
        service_renderer_pump_port();
#ifdef ELECTRON_SHELL_WINDOWED
        service_native_window_requests(&window_info, &browser_settings,
            selected_url, selected_url_length);
        pump_node_on_main_task();
#endif
        if (!node_marker_seen && marker_exists())
            node_marker_seen = 1;
#ifdef ELECTRON_SHELL_WINDOWED
        if (!external_url &&
            node_attached && node_marker_seen && !node_task_mismatch &&
            first_paint && ticks > first_paint_tick + CLOSE_AFTER_PAINT_TICKS)
            request_browser_close();
#else
        if (!external_url &&
            node_attached && node_detached && node_marker_seen &&
            !node_task_mismatch && first_paint &&
            ticks > first_paint_tick + CLOSE_AFTER_PAINT_TICKS)
            request_browser_close();
#endif
        Delay(1);
    }

    if (!browser_closed)
        request_browser_close();
    for (close_ticks = 0;
        close_ticks < CLOSE_TIMEOUT_TICKS &&
            (!browser_closed || !node_detached);
        ++close_ticks)
    {
        ElectronDoCEFMessageLoopWork();
        service_renderer_pump_port();
#ifdef ELECTRON_SHELL_WINDOWED
        pump_node_on_main_task();
#endif
        Delay(1);
    }

#ifdef ELECTRON_SHELL_WINDOWED
    if (node_attached && !node_detached)
    {
        ElectronDetachNodeEmbedder();
        node_detached = 1;
    }
#endif
    if (node_attached && node_detached && node_marker_seen &&
        !node_task_mismatch && node_pump_ticks >= NODE_PUMP_TICKS &&
        first_paint && browser_closed)
    {
        KPrintF("[ELECTRON-CONTEXT] PASS persistent Node main context drove "
            "BrowserWindow requests pumps=%lu\n", node_pump_ticks);
        result = 0;
    }
    else
    {
        KPrintF("[ELECTRON-CONTEXT] FAIL attached=%ld detached=%ld "
            "marker=%ld task_mismatch=%ld pumps=%lu painted=%ld closed=%ld\n",
            (LONG)node_attached, (LONG)node_detached, (LONG)node_marker_seen,
            (LONG)node_task_mismatch, node_pump_ticks, (LONG)first_paint,
            (LONG)browser_closed);
    }

    drain_cef_before_shutdown();

shutdown_cef:
    ElectronShutdownCEF();
detach_node:
    if (!node_detached)
        ElectronDetachNodeEmbedder();
    release_node_context();
    if (requesters_suppressed)
    {
        process->pr_WindowPtr = saved_window_ptr;
        KPrintF("[ELECTRON-CONTEXT] DOS requester window restored=%p\n",
            saved_window_ptr);
    }
    DeleteMsgPort(ui_pump_port);
    return result;
}
