/*
 * First combined Electron lifecycle probe.
 *
 * Run the proven one-shot Node command lifecycle, then initialize and shut
 * down CEF in the same AROS process through electron.library.  This does not
 * claim simultaneous embedding; it tests whether process-global V8 teardown
 * from Node leaves the shared engine reusable by CEF.
 */

#include <string.h>

#include <exec/types.h>
#include <proto/debug.h>
#include <proto/electron.h>

#include "include/capi/cef_app_capi.h"

static cef_char_t resources_path[] = u"Developer:Chromium";
static cef_char_t locales_path[] = u"Developer:Chromium/locales";
static char node_name[] = "ElectronLifecycleTest";
static char eval_flag[] = "-e";
static char node_script[] =
    "console.log('ELECTRON_NODE_PHASE_PASS '+process.version)";

static void set_static_cef_string(cef_string_t *target, cef_char_t *value,
    size_t length)
{
    target->str = value;
    target->length = length;
    target->dtor = NULL;
}

int main(int argc, char **argv)
{
    char *cef_argv[5];
    STRPTR node_argv[] = {
        (STRPTR)node_name,
        (STRPTR)eval_flag,
        (STRPTR)node_script,
        NULL
    };
    cef_main_args_t main_args;
    cef_settings_t settings;
    LONG node_result;
    LONG execute_result;
    LONG initialize_result;

    (void)argc;
    KPrintF("[ELECTRON-LIFECYCLE] node begin\n");
    node_result = ElectronRunNode(3, node_argv);
    KPrintF("[ELECTRON-LIFECYCLE] node end rc=%ld\n", node_result);
    if (node_result != 0)
        return 20;

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
    set_static_cef_string(&settings.resources_dir_path, resources_path,
        (sizeof(resources_path) / sizeof(resources_path[0])) - 1);
    set_static_cef_string(&settings.locales_dir_path, locales_path,
        (sizeof(locales_path) / sizeof(locales_path[0])) - 1);

    KPrintF("[ELECTRON-LIFECYCLE] cef execute begin\n");
    execute_result = ElectronExecuteCEFProcess(&main_args, NULL, NULL);
    KPrintF("[ELECTRON-LIFECYCLE] cef execute end rc=%ld\n", execute_result);
    if (execute_result >= 0)
        return (int)execute_result;

    KPrintF("[ELECTRON-LIFECYCLE] cef initialize begin\n");
    initialize_result = ElectronInitializeCEF(
        &main_args, &settings, NULL, NULL);
    KPrintF("[ELECTRON-LIFECYCLE] cef initialize end rc=%ld\n",
        initialize_result);
    if (initialize_result == 0)
        return 20;

    ElectronDoCEFMessageLoopWork();
    KPrintF("[ELECTRON-LIFECYCLE] cef shutdown begin\n");
    ElectronShutdownCEF();
    KPrintF("[ELECTRON-LIFECYCLE] PASS node then cef shutdown\n");
    return 0;
}
