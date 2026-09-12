/*
 * Exact-build CEF/Blink implementation bridge.
 *
 * All Chromium C++ implementation and data remain in this module.  The public
 * cef.library calls only these typed C ABI vectors.
 */

#include <aros/libcall.h>
#include <exec/libraries.h>

#include "cef-blink_libdefs.h"

extern int cef_engine_version_info(int entry) __asm__("cef_version_info");
extern const char *cef_engine_api_hash(int entry) __asm__("cef_api_hash");
extern int cef_engine_execute_process(const void *args, void *application,
    void *sandbox_info) __asm__("cef_execute_process");
extern int cef_engine_initialize(const void *args, const void *settings,
    void *application, void *sandbox_info) __asm__("cef_initialize");
extern void cef_engine_shutdown(void) __asm__("cef_shutdown");
extern void cef_engine_do_message_loop_work(void)
    __asm__("cef_do_message_loop_work");
extern void cef_engine_run_message_loop(void) __asm__("cef_run_message_loop");
extern void cef_engine_quit_message_loop(void) __asm__("cef_quit_message_loop");
extern int cef_engine_create_browser(const void *window_info, void *client,
    const void *url, const void *settings, void *extra_info,
    void *request_context) __asm__("cef_browser_host_create_browser");
extern int cef_engine_post_task(int thread_id, void *task)
    __asm__("cef_post_task");
extern int cef_engine_post_delayed_task(int thread_id, void *task,
    long long delay_ms) __asm__("cef_post_delayed_task");
extern int cef_engine_register_extension(const void *name,
    const void *javascript_code, void *handler)
    __asm__("cef_register_extension");
extern void *cef_engine_v8_platform(void)
    __asm__("_ZN3gin10V8Platform3GetEv");
extern int cef_engine_v8_worker_threads(void *platform)
    __asm__("_ZN3gin10V8Platform21NumberOfWorkerThreadsEv");
extern int *__stdc_geterrnoptr(void);

int *__errno_location(void)
{
    return __stdc_geterrnoptr();
}

AROS_LH1(LONG, CEFBlinkVersionInfo,
    AROS_LHA(LONG, entry, D0),
    LIBBASETYPEPTR, LIBBASE, 5, CEFBlink)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return (LONG)cef_engine_version_info((int)entry);
    AROS_LIBFUNC_EXIT
}

AROS_LH1(CONST_STRPTR, CEFBlinkApiHash,
    AROS_LHA(LONG, entry, D0),
    LIBBASETYPEPTR, LIBBASE, 6, CEFBlink)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return (CONST_STRPTR)cef_engine_api_hash((int)entry);
    AROS_LIBFUNC_EXIT
}

AROS_LH3(LONG, CEFBlinkExecuteProcess,
    AROS_LHA(APTR, args, A0),
    AROS_LHA(APTR, application, A1),
    AROS_LHA(APTR, sandbox_info, A2),
    LIBBASETYPEPTR, LIBBASE, 7, CEFBlink)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return (LONG)cef_engine_execute_process(args, application, sandbox_info);
    AROS_LIBFUNC_EXIT
}

AROS_LH4(LONG, CEFBlinkInitialize,
    AROS_LHA(APTR, args, A0),
    AROS_LHA(APTR, settings, A1),
    AROS_LHA(APTR, application, A2),
    AROS_LHA(APTR, sandbox_info, A3),
    LIBBASETYPEPTR, LIBBASE, 8, CEFBlink)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return (LONG)cef_engine_initialize(args, settings, application,
        sandbox_info);
    AROS_LIBFUNC_EXIT
}

AROS_LH0(void, CEFBlinkShutdown,
    LIBBASETYPEPTR, LIBBASE, 9, CEFBlink)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    cef_engine_shutdown();
    AROS_LIBFUNC_EXIT
}

AROS_LH0(void, CEFBlinkDoMessageLoopWork,
    LIBBASETYPEPTR, LIBBASE, 10, CEFBlink)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    cef_engine_do_message_loop_work();
    AROS_LIBFUNC_EXIT
}

AROS_LH0(void, CEFBlinkRunMessageLoop,
    LIBBASETYPEPTR, LIBBASE, 11, CEFBlink)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    cef_engine_run_message_loop();
    AROS_LIBFUNC_EXIT
}

AROS_LH0(void, CEFBlinkQuitMessageLoop,
    LIBBASETYPEPTR, LIBBASE, 12, CEFBlink)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    cef_engine_quit_message_loop();
    AROS_LIBFUNC_EXIT
}

AROS_LH6(LONG, CEFBlinkCreateBrowser,
    AROS_LHA(APTR, window_info, A0),
    AROS_LHA(APTR, client, A1),
    AROS_LHA(APTR, url, A2),
    AROS_LHA(APTR, settings, A3),
    AROS_LHA(APTR, extra_info, D0),
    AROS_LHA(APTR, request_context, D1),
    LIBBASETYPEPTR, LIBBASE, 13, CEFBlink)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return (LONG)cef_engine_create_browser(window_info, client, url, settings,
        extra_info, request_context);
    AROS_LIBFUNC_EXIT
}

AROS_LH2(LONG, CEFBlinkPostTask,
    AROS_LHA(LONG, thread_id, D0),
    AROS_LHA(APTR, task, A0),
    LIBBASETYPEPTR, LIBBASE, 14, CEFBlink)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return (LONG)cef_engine_post_task((int)thread_id, task);
    AROS_LIBFUNC_EXIT
}

AROS_LH3(LONG, CEFBlinkPostDelayedTask,
    AROS_LHA(LONG, thread_id, D0),
    AROS_LHA(APTR, task, A0),
    AROS_LHA(LONG, delay_ms, D1),
    LIBBASETYPEPTR, LIBBASE, 15, CEFBlink)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return (LONG)cef_engine_post_delayed_task((int)thread_id, task,
        (long long)delay_ms);
    AROS_LIBFUNC_EXIT
}

AROS_LH3(LONG, CEFBlinkRegisterV8Extension,
    AROS_LHA(APTR, name, A0),
    AROS_LHA(APTR, javascript_code, A1),
    AROS_LHA(APTR, handler, A2),
    LIBBASETYPEPTR, LIBBASE, 16, CEFBlink)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return (LONG)cef_engine_register_extension(name, javascript_code,
        handler);
    AROS_LIBFUNC_EXIT
}

AROS_LH0(LONG, CEFBlinkDefaultV8WorkerThreadCount,
    LIBBASETYPEPTR, LIBBASE, 17, CEFBlink)
{
    void *platform;

    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    platform = cef_engine_v8_platform();
    if (platform == NULL)
        return 0;
    return (LONG)cef_engine_v8_worker_threads(platform);
    AROS_LIBFUNC_EXIT
}
