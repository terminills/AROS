/*
 * Native AROS Electron convergence adapter.
 *
 * Keep Node and CEF external.  This library is the compatibility seam where
 * Electron-specific assumptions can be reconciled without contaminating the
 * independently proven node.library, cef.library, or v8.library contracts.
 */

#include <aros/libcall.h>
#include <aros/debug.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <intuition/intuition.h>
#include <libraries/electron.h>
#include <proto/cef.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/node.h>
#include <proto/v8.h>

#include <signal.h>
#include <string.h>

#include "electron_libdefs.h"

#define AROS_ELECTRON_VERSION "0.1.0-aros"
#define ELECTRON_REQUEST_CAPACITY 8
#define ELECTRON_REQUEST_METHOD_SIZE 64
#define ELECTRON_REQUEST_PAYLOAD_SIZE 2048

struct ElectronNativeRequest
{
    char method[ELECTRON_REQUEST_METHOD_SIZE];
    char payload[ELECTRON_REQUEST_PAYLOAD_SIZE];
};

static struct ElectronNativeRequest
    electron_requests[ELECTRON_REQUEST_CAPACITY];
static ULONG electron_request_head;
static ULONG electron_request_tail;
static char electron_sync_result[32];
static char electron_screen_result[96];

static char *electron_append_long(char *output, LONG value)
{
    char digits[16];
    ULONG magnitude;
    ULONG count = 0;

    if (value < 0)
    {
        *output++ = '-';
        magnitude = (ULONG)(-(value + 1)) + 1;
    }
    else
        magnitude = (ULONG)value;

    do
    {
        digits[count++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);

    while (count != 0)
        *output++ = digits[--count];
    return output;
}

static CONST_STRPTR electron_screen_snapshot(void)
{
    struct Screen *screen = LockPubScreen(NULL);
    char *output = electron_screen_result;
    LONG values[5];
    ULONG index;

    if (screen == NULL)
        return "640,480,0,0,0";

    values[0] = screen->Width;
    values[1] = screen->Height;
    values[2] = screen->BarHeight + 1;
    values[3] = screen->MouseX;
    values[4] = screen->MouseY;
    for (index = 0; index < 5; ++index)
    {
        if (index != 0)
            *output++ = ',';
        output = electron_append_long(output, values[index]);
    }
    *output = '\0';
    UnlockPubScreen(NULL, screen);
    return electron_screen_result;
}

static void electron_decode_component(char *text)
{
    char *read = text;
    char *write = text;

    while (*read != '\0')
    {
        if (*read == '%' &&
            ((read[1] >= '0' && read[1] <= '9') ||
             (read[1] >= 'A' && read[1] <= 'F') ||
             (read[1] >= 'a' && read[1] <= 'f')) &&
            ((read[2] >= '0' && read[2] <= '9') ||
             (read[2] >= 'A' && read[2] <= 'F') ||
             (read[2] >= 'a' && read[2] <= 'f')))
        {
            unsigned int high =
                read[1] <= '9' ? read[1] - '0' :
                (read[1] & ~0x20) - 'A' + 10;
            unsigned int low =
                read[2] <= '9' ? read[2] - '0' :
                (read[2] & ~0x20) - 'A' + 10;
            *write++ = (char)((high << 4) | low);
            read += 3;
        }
        else
            *write++ = *read++;
    }
    *write = '\0';
}

static CONST_STRPTR electron_show_message_box(CONST_STRPTR payload)
{
    char *copy;
    char *fields[4] = { NULL, NULL, NULL, NULL };
    char *buttons[16];
    char *cursor;
    char *gadgets;
    char *message;
    ULONG field_count = 0;
    ULONG button_count = 0;
    ULONG gadget_length = 1;
    ULONG message_length;
    LONG result;
    ULONG index;
    struct EasyStruct easy;
    IPTR arguments[1];

    if (payload == NULL)
        payload = (CONST_STRPTR)"";
    copy = AllocVec(strlen((const char *)payload) + 1, MEMF_ANY);
    if (copy == NULL)
        return "-1";
    strcpy(copy, (const char *)payload);

    cursor = copy;
    fields[field_count++] = cursor;
    while (*cursor != '\0' && field_count < 4)
    {
        if (*cursor == '\t')
        {
            *cursor = '\0';
            fields[field_count++] = cursor + 1;
        }
        ++cursor;
    }
    while (field_count < 4)
        fields[field_count++] = (char *)"";
    for (index = 0; index < 3; ++index)
        electron_decode_component(fields[index]);

    cursor = fields[3];
    while (*cursor != '\0' && button_count < 16)
    {
        buttons[button_count++] = cursor;
        while (*cursor != '\0' && *cursor != '\x1f')
            ++cursor;
        if (*cursor == '\x1f')
            *cursor++ = '\0';
    }
    if (button_count == 0)
    {
        buttons[button_count++] = (char *)"OK";
    }
    for (index = 0; index < button_count; ++index)
    {
        const char *label;

        electron_decode_component(buttons[index]);
        gadget_length += strlen(buttons[index]) + 1;
        for (label = buttons[index]; *label != '\0'; ++label)
            if (*label == '%')
                ++gadget_length;
    }
    gadgets = AllocVec(gadget_length, MEMF_ANY);
    if (gadgets == NULL)
    {
        FreeVec(copy);
        return "-1";
    }
    gadgets[0] = '\0';
    for (index = 0; index < button_count; ++index)
    {
        const char *label;
        char *end;

        if (index != 0)
            strcat(gadgets, "|");
        end = gadgets + strlen(gadgets);
        for (label = buttons[index]; *label != '\0'; ++label)
        {
            *end++ = *label;
            if (*label == '%')
                *end++ = '%';
        }
        *end = '\0';
    }

    message_length = strlen(fields[1]) + strlen(fields[2]) + 3;
    message = AllocVec(message_length, MEMF_ANY);
    if (message == NULL)
    {
        FreeVec(gadgets);
        FreeVec(copy);
        return "-1";
    }
    strcpy(message, fields[1]);
    if (fields[2][0] != '\0')
    {
        strcat(message, "\n\n");
        strcat(message, fields[2]);
    }

    easy.es_StructSize = sizeof(easy);
    easy.es_Flags = 0;
    easy.es_Title = (STRPTR)(fields[0][0] != '\0' ?
        fields[0] : "AROS Electron");
    easy.es_TextFormat = (STRPTR)"%s";
    easy.es_GadgetFormat = gadgets;
    arguments[0] = (IPTR)message;
    result = EasyRequestArgs(NULL, &easy, NULL, (RAWARG)arguments);

    index = result == 0 ? button_count - 1 : (ULONG)result - 1;
    if (index >= 10)
    {
        electron_sync_result[0] = (char)('0' + index / 10);
        electron_sync_result[1] = (char)('0' + index % 10);
        electron_sync_result[2] = '\0';
    }
    else
    {
        electron_sync_result[0] = (char)('0' + index);
        electron_sync_result[1] = '\0';
    }
    FreeVec(message);
    FreeVec(gadgets);
    FreeVec(copy);
    return (CONST_STRPTR)electron_sync_result;
}

/*
 * Keep Electron's Node-facing compatibility policy in this adapter rather
 * than patching the independently usable node.library.  This first append-only
 * module seam deliberately exposes only the app lifecycle surface that has a
 * proven implementation.  BrowserWindow and IPC will be added when their
 * native contracts exist instead of advertising inert stubs.
 */
static const char electron_module_bootstrap[] =
    "(()=>{"
    "const binding=process._linkedBinding('aros_electron');"
    "const describeError=value=>{"
    "if(value&&typeof value.stack==='string')return value.stack;"
    "try{return typeof value==='string'?value:JSON.stringify(value);}"
    "catch{return String(value);}"
    "};"
    "const nativeConsoleError=console.error.bind(console);"
    "console.error=(...values)=>{"
    "binding.invoke('console-error',values.map(describeError).join(' '));"
    "return nativeConsoleError(...values);"
    "};"
    "process.on('uncaughtException',error=>"
    "binding.invoke('uncaught-exception',describeError(error)));"
    "process.on('unhandledRejection',reason=>"
    "binding.invoke('unhandled-rejection',describeError(reason)));"
    "if(!process.versions.electron)process.versions.electron='0.1.0-aros';"
    "if(!process.type)process.type='browser';"
    "if(!process.resourcesPath)process.resourcesPath=process.cwd();"
    "process.env.VSCODE_HANDLES_SIGPIPE='1';"
    "if(typeof globalThis.performance==='undefined')"
    "globalThis.performance=require('node:perf_hooks').performance;"
    "const native=Object.freeze({"
    "ping:()=>binding.invoke('ping','')"
    "});"
    "const {EventEmitter}=require('node:events');"
    "const electronPath=require('node:path');"
    "const electronOs=require('node:os');"
    "const app=new EventEmitter();"
    "let appName='AROS Electron';"
    "const homePath=electronOs.homedir();"
    "const appDataPath=process.env.APPDATA||homePath;"
    "const appPaths=new Map(["
    "['home',homePath],"
    "['appData',appDataPath],"
    "['userData',electronPath.join(appDataPath,appName)],"
    "['sessionData',electronPath.join(appDataPath,appName)],"
    "['temp',electronOs.tmpdir()],"
    "['exe',process.execPath],"
    "['module',process.execPath],"
    "['desktop',electronPath.join(homePath,'Desktop')],"
    "['documents',electronPath.join(homePath,'Documents')],"
    "['downloads',electronPath.join(homePath,'Downloads')],"
    "['music',electronPath.join(homePath,'Music')],"
    "['pictures',electronPath.join(homePath,'Pictures')],"
    "['videos',electronPath.join(homePath,'Videos')],"
    "['logs',electronPath.join(appDataPath,appName,'logs')],"
    "['crashDumps',electronPath.join(appDataPath,appName,'Crashpad')]"
    "]);"
    "let appReady=false;"
    "let resolveAppReady;"
    "const appReadyPromise=new Promise(resolve=>{resolveAppReady=resolve;});"
    "const commandLineSwitches=new Map();"
    "const commandLineArguments=[];"
    "for(const argument of process.argv.slice(1)){"
    "if(argument.startsWith('--')){"
    "const separator=argument.indexOf('=');"
    "const name=(separator<0?argument.slice(2):"
    "argument.slice(2,separator)).toLowerCase();"
    "const value=separator<0?'':argument.slice(separator+1);"
    "commandLineSwitches.set(name,value);"
    "}else commandLineArguments.push(argument);"
    "}"
    "const commandLine=Object.freeze({"
    "hasSwitch:name=>commandLineSwitches.has(String(name).toLowerCase()),"
    "getSwitchValue:name=>"
    "commandLineSwitches.get(String(name).toLowerCase())||'',"
    "appendSwitch:(name,value)=>{"
    "commandLineSwitches.set(String(name).toLowerCase(),"
    "value===undefined?'':String(value));"
    "},"
    "removeSwitch:name=>commandLineSwitches.delete("
    "String(name).toLowerCase()),"
    "appendArgument:value=>commandLineArguments.push(String(value))"
    "});"
    "app.isReady=()=>appReady;"
    "app.whenReady=()=>appReadyPromise;"
    "app.getName=()=>appName;"
    "app.setName=value=>{appName=String(value);};"
    "app.getVersion=()=>'0.1.0-aros';"
    "app.commandLine=commandLine;"
    "app.getAppPath=()=>process.cwd();"
    "app.getPath=name=>{"
    "const key=String(name);"
    "if(!appPaths.has(key))throw new Error('Unknown Electron path: '+key);"
    "return appPaths.get(key);"
    "};"
    "app.setPath=(name,value)=>{"
    "const key=String(name);"
    "const path=String(value);"
    "if(!path)throw new TypeError('Electron path must not be empty');"
    "appPaths.set(key,path);"
    "};"
    "app.setAppLogsPath=value=>{"
    "appPaths.set('logs',value===undefined?"
    "electronPath.join(appPaths.get('userData'),'logs'):String(value));"
    "};"
    "const preferredSystemLanguages=(()=>{"
    "const values=[...(process.env.LANGUAGE||'').split(':'),"
    "process.env.LC_ALL,process.env.LC_MESSAGES,process.env.LANG,"
    "Intl.DateTimeFormat().resolvedOptions().locale];"
    "const languages=[];"
    "for(let value of values){"
    "if(!value)continue;"
    "value=String(value).split('.')[0].replace(/_/g,'-');"
    "if(value==='C'||value==='POSIX')value='en';"
    "if(!languages.includes(value))languages.push(value);"
    "}"
    "return languages.length?languages:['en'];"
    "})();"
    "app.getPreferredSystemLanguages=()=>preferredSystemLanguages.slice();"
    "app.getLocale=()=>preferredSystemLanguages[0];"
    "app.getSystemLocale=()=>preferredSystemLanguages[0];"
    "app.exit=code=>binding.invoke('app.exit',String(code===undefined?0:code));"
    "app.quit=()=>{"
    "app.emit('before-quit');"
    "app.emit('will-quit');"
    "return binding.invoke('app.exit','0');"
    "};"
    "let applicationMenu=null;"
    "class Menu{"
    "constructor(){this.items=[];}"
    "append(item){this.items.push(item);}"
    "insert(position,item){this.items.splice(Number(position),0,item);}"
    "getMenuItemById(id){"
    "return this.items.find(item=>item&&item.id===id)||null;"
    "}"
    "static setApplicationMenu(menu){"
    "if(menu!==null&&!(menu instanceof Menu))"
    "throw new TypeError('application menu must be a Menu or null');"
    "applicationMenu=menu;"
    "}"
    "static getApplicationMenu(){return applicationMenu;}"
    "static buildFromTemplate(template){"
    "if(!Array.isArray(template))throw new TypeError('menu template must be an array');"
    "const menu=new Menu();"
    "for(const item of template)menu.append(item);"
    "return menu;"
    "}"
    "}"
    "const privilegedSchemes=new Map();"
    "const globalFileProtocols=new Map();"
    "const globalInterceptedFileProtocols=new Map();"
    "const registerGlobalFileProtocol=(kind,scheme,handler)=>{"
    "const key=String(scheme);"
    "if(!key||typeof handler!=='function')"
    "throw new TypeError(kind+' requires a scheme and handler');"
    "const registry=kind.startsWith('protocol.intercept-')?"
    "globalInterceptedFileProtocols:globalFileProtocols;"
    "registry.set(key,handler);"
    "binding.invoke(kind,key);"
    "};"
    "const protocol=Object.freeze({"
    "registerSchemesAsPrivileged:schemes=>{"
    "if(appReady)throw new Error("
    "'protocol schemes must be registered before app ready');"
    "if(!Array.isArray(schemes))"
    "throw new TypeError('protocol schemes must be an array');"
    "for(const entry of schemes){"
    "if(!entry||typeof entry.scheme!=='string'||!entry.scheme)"
    "throw new TypeError('protocol scheme must be a non-empty string');"
    "privilegedSchemes.set(entry.scheme,"
    "Object.freeze({...entry.privileges}));"
    "}"
    "},"
    "isProtocolHandled:scheme=>"
    "Promise.resolve(privilegedSchemes.has(String(scheme))||"
    "globalFileProtocols.has(String(scheme))||"
    "globalInterceptedFileProtocols.has(String(scheme))),"
    "registerFileProtocol:(scheme,handler)=>"
    "registerGlobalFileProtocol('protocol.register-file-protocol',"
    "scheme,handler),"
    "registerBufferProtocol:(scheme,handler)=>"
    "registerGlobalFileProtocol('protocol.register-buffer-protocol',"
    "scheme,handler),"
    "registerHttpProtocol:(scheme,handler)=>"
    "registerGlobalFileProtocol('protocol.register-http-protocol',"
    "scheme,handler),"
    "interceptFileProtocol:(scheme,handler)=>"
    "registerGlobalFileProtocol('protocol.intercept-file-protocol',"
    "scheme,handler),"
    "unregisterProtocol:scheme=>{"
    "const key=String(scheme);"
    "if(globalFileProtocols.delete(key))"
    "binding.invoke('protocol.unregister-protocol',key);"
    "return Promise.resolve();"
    "},"
    "uninterceptProtocol:scheme=>{"
    "const key=String(scheme);"
    "if(globalInterceptedFileProtocols.delete(key))"
    "binding.invoke('protocol.unintercept-protocol',key);"
    "return Promise.resolve();"
    "}"
    "});"
    "let permissionRequestHandler=null;"
    "let permissionCheckHandler=null;"
    "let codeCachePath='';"
    "const requestHandlers={"
    "beforeRequest:[],headersReceived:[],beforeSendHeaders:[]"
    "};"
    "const registerRequestHandler=(kind,filterOrListener,listener)=>{"
    "const handler=typeof listener==='function'?listener:filterOrListener;"
    "if(typeof handler!=='function')"
    "throw new TypeError('webRequest listener must be a function');"
    "requestHandlers[kind].push(handler);"
    "};"
    "const webRequest=Object.freeze({"
    "onBeforeRequest:(filterOrListener,listener)=>"
    "registerRequestHandler('beforeRequest',filterOrListener,listener),"
    "onHeadersReceived:(filterOrListener,listener)=>"
    "registerRequestHandler('headersReceived',filterOrListener,listener),"
    "onBeforeSendHeaders:(filterOrListener,listener)=>"
    "registerRequestHandler('beforeSendHeaders',filterOrListener,listener)"
    "});"
    "const sessionFileProtocols=new Map();"
    "const sessionInterceptedFileProtocols=new Map();"
    "const sessionProtocol=Object.freeze({"
    "registerFileProtocol:(scheme,handler)=>{"
    "const key=String(scheme);"
    "if(!key||typeof handler!=='function')"
    "throw new TypeError('registerFileProtocol requires a scheme and handler');"
    "sessionFileProtocols.set(key,handler);"
    "binding.invoke('session.register-file-protocol',key);"
    "},"
    "interceptFileProtocol:(scheme,handler)=>{"
    "const key=String(scheme);"
    "if(!key||typeof handler!=='function')"
    "throw new TypeError('interceptFileProtocol requires a scheme and handler');"
    "sessionInterceptedFileProtocols.set(key,handler);"
    "binding.invoke('session.intercept-file-protocol',key);"
    "},"
    "unregisterProtocol:scheme=>{"
    "const key=String(scheme);"
    "const removed=sessionFileProtocols.delete(key);"
    "if(removed)binding.invoke('session.unregister-protocol',key);"
    "return Promise.resolve();"
    "},"
    "uninterceptProtocol:scheme=>{"
    "const key=String(scheme);"
    "const removed=sessionInterceptedFileProtocols.delete(key);"
    "if(removed)binding.invoke('session.unintercept-protocol',key);"
    "return Promise.resolve();"
    "}"
    "});"
    "const defaultSession=Object.freeze({"
    "setPermissionRequestHandler:handler=>{"
    "if(handler!==null&&typeof handler!=='function')"
    "throw new TypeError('permission request handler must be a function or null');"
    "permissionRequestHandler=handler;"
    "},"
    "setPermissionCheckHandler:handler=>{"
    "if(handler!==null&&typeof handler!=='function')"
    "throw new TypeError('permission check handler must be a function or null');"
    "permissionCheckHandler=handler;"
    "},"
    "setCodeCachePath:path=>{codeCachePath=String(path);},"
    "webRequest,"
    "protocol:sessionProtocol"
    "});"
    "const session=Object.freeze({defaultSession});"
    "const ipcMain=new EventEmitter();"
    "const ipcMainHandlers=new Map();"
    "ipcMain.handle=(channel,listener)=>{"
    "const key=String(channel);"
    "if(typeof listener!=='function')"
    "throw new TypeError('ipcMain handler must be a function');"
    "if(ipcMainHandlers.has(key))"
    "throw new Error('Attempted to register a second handler for '+key);"
    "ipcMainHandlers.set(key,listener);"
    "};"
    "ipcMain.handleOnce=(channel,listener)=>{"
    "const key=String(channel);"
    "if(typeof listener!=='function')"
    "throw new TypeError('ipcMain handler must be a function');"
    "ipcMain.handle(key,async(...args)=>{"
    "ipcMainHandlers.delete(key);"
    "return listener(...args);"
    "});"
    "};"
    "ipcMain.removeHandler=channel=>{"
    "ipcMainHandlers.delete(String(channel));"
    "};"
    "Object.defineProperty(ipcMain,'_arosDispatchInvoke',{"
    "value:(channel,event,...args)=>{"
    "const key=String(channel);"
    "const handler=ipcMainHandlers.get(key);"
    "if(!handler)"
    "return Promise.reject(new Error('No handler registered for '+key));"
    "return Promise.resolve().then(()=>handler(event,...args));"
    "},enumerable:false"
    "});"
    "Object.defineProperty(ipcMain,'_arosDispatchMessage',{"
    "value:(channel,event,...args)=>"
    "ipcMain.emit(String(channel),event,...args),enumerable:false"
    "});"
    "const nativeTheme=new EventEmitter();"
    "const systemDarkTheme=/^(1|true|dark)$/i.test("
    "String(process.env.AROS_DARK_MODE||process.env.AROS_THEME||''));"
    "let nativeThemeSource='system';"
    "Object.defineProperties(nativeTheme,{"
    "themeSource:{"
    "get:()=>nativeThemeSource,"
    "set:value=>{"
    "const source=String(value);"
    "if(source!=='system'&&source!=='light'&&source!=='dark')"
    "throw new TypeError('themeSource must be system, light, or dark');"
    "if(source===nativeThemeSource)return;"
    "nativeThemeSource=source;"
    "nativeTheme.emit('updated');"
    "},enumerable:true"
    "},"
    "shouldUseDarkColors:{"
    "get:()=>nativeThemeSource==='dark'||"
    "(nativeThemeSource==='system'&&systemDarkTheme),enumerable:true"
    "},"
    "shouldUseHighContrastColors:{get:()=>false,enumerable:true},"
    "shouldUseInvertedColorScheme:{get:()=>false,enumerable:true}"
    "});"
    "const powerMonitor=new EventEmitter();"
    "powerMonitor.getSystemIdleState=threshold=>"
    "(Number(threshold)>=0?'active':'unknown');"
    "powerMonitor.getSystemIdleTime=()=>0;"
    "powerMonitor.isOnBatteryPower=()=>false;"
    "const crashReporterParameters=new Map();"
    "let crashReporterStarted=false;"
    "const crashReporter=Object.freeze({"
    "start:options=>{"
    "if(options!==undefined&&(options===null||typeof options!=='object'))"
    "throw new TypeError('crashReporter options must be an object');"
    "crashReporterStarted=true;"
    "const extra=options&&options.extra;"
    "if(extra&&typeof extra==='object')"
    "for(const [key,value] of Object.entries(extra))"
    "crashReporterParameters.set(String(key),String(value));"
    "binding.invoke('crash-reporter.start',JSON.stringify({"
    "productName:String(options&&options.productName||appName),"
    "companyName:String(options&&options.companyName||''),"
    "uploadToServer:false}));"
    "},"
    "isCrashReporterEnabled:()=>crashReporterStarted,"
    "getLastCrashReport:()=>null,"
    "getUploadedReports:()=>[],"
    "getUploadToServer:()=>false,"
    "setUploadToServer:value=>{"
    "if(value)binding.invoke('crash-reporter.upload-unsupported','');"
    "},"
    "addExtraParameter:(key,value)=>"
    "crashReporterParameters.set(String(key),String(value)),"
    "removeExtraParameter:key=>crashReporterParameters.delete(String(key)),"
    "getParameters:()=>Object.fromEntries(crashReporterParameters)"
    "});"
    "const screen=new EventEmitter();"
    "const readScreen=()=>{"
    "const values=String(binding.invoke('screen.snapshot','')).split(',')"
    ".map(value=>Number(value));"
    "const width=Number.isFinite(values[0])?values[0]:640;"
    "const height=Number.isFinite(values[1])?values[1]:480;"
    "const top=Math.max(0,Number.isFinite(values[2])?values[2]:0);"
    "const cursorX=Number.isFinite(values[3])?values[3]:0;"
    "const cursorY=Number.isFinite(values[4])?values[4]:0;"
    "const bounds={x:0,y:0,width,height};"
    "const workArea={x:0,y:top,width,height:Math.max(1,height-top)};"
    "return {display:Object.freeze({id:1,bounds,workArea,"
    "size:{width,height},workAreaSize:{width,height:workArea.height},"
    "scaleFactor:1,rotation:0,internal:false,touchSupport:'unknown',"
    "monochrome:false,accelerometerSupport:'unknown',colorSpace:'srgb',"
    "colorDepth:24,depthPerComponent:8,displayFrequency:60}),"
    "cursor:{x:cursorX,y:cursorY}};"
    "};"
    "screen.getAllDisplays=()=>[readScreen().display];"
    "screen.getPrimaryDisplay=()=>readScreen().display;"
    "screen.getDisplayNearestPoint=()=>readScreen().display;"
    "screen.getDisplayMatching=()=>readScreen().display;"
    "screen.getCursorScreenPoint=()=>readScreen().cursor;"
    "let nextWindowId=1;"
    "const browserWindows=[];"
    "class BrowserWindow extends EventEmitter{"
    "constructor(options={}){"
    "super();this.id=nextWindowId++;this.options=options;"
    "this._fullScreen=!!options.fullscreen;"
    "this._simpleFullScreen=!!options.simpleFullscreen;"
    "this._menuBarVisible=options.autoHideMenuBar!==true;"
    "this._title=String(options.title||'');"
    "this._destroyed=false;"
    "this._visible=options.show!==false;"
    "this._minimized=false;"
    "this._maximized=false;"
    "this.webContents=new EventEmitter();"
    "this.webContents.loadURL=(url)=>this.loadURL(url);"
    "this.webContents.session=defaultSession;"
    "this.webContents.isDestroyed=()=>this._destroyed;"
    "browserWindows.push(this);"
    "const webPreferences=options.webPreferences||{};"
    "const preload=String(webPreferences.preload||'');"
    "const additionalArguments=Array.isArray("
    "webPreferences.additionalArguments)"
    "?webPreferences.additionalArguments.map(String):[];"
    "binding.invoke('browser-window.create',"
    "[String(this.id),preload,...additionalArguments].join('\\t'));"
    "}"
    "loadURL(url){"
    "binding.invoke('browser-window.load-url',"
    "String(this.id)+'\\t'+String(url));"
    "return Promise.resolve();"
    "}"
    "isFullScreen(){return this._fullScreen;}"
    "setFullScreen(value){"
    "this._fullScreen=!!value;"
    "binding.invoke('browser-window.set-full-screen',"
    "String(this.id)+'\\t'+String(this._fullScreen));"
    "}"
    "isSimpleFullScreen(){return this._simpleFullScreen;}"
    "setSimpleFullScreen(value){"
    "this._simpleFullScreen=!!value;"
    "binding.invoke('browser-window.set-simple-full-screen',"
    "String(this.id)+'\\t'+String(this._simpleFullScreen));"
    "}"
    "setMenuBarVisibility(value){"
    "this._menuBarVisible=!!value;"
    "binding.invoke('browser-window.set-menu-bar-visibility',"
    "String(this.id)+'\\t'+String(this._menuBarVisible));"
    "}"
    "isMenuBarVisible(){return this._menuBarVisible;}"
    "setTitle(value){"
    "this._title=String(value);"
    "binding.invoke('browser-window.set-title',"
    "String(this.id)+'\\t'+this._title);"
    "}"
    "getTitle(){return this._title;}"
    "isDestroyed(){return this._destroyed;}"
    "isVisible(){return !this._destroyed&&this._visible;}"
    "show(){"
    "this._visible=true;this._minimized=false;"
    "binding.invoke('browser-window.show',String(this.id));"
    "}"
    "hide(){"
    "this._visible=false;"
    "binding.invoke('browser-window.hide',String(this.id));"
    "}"
    "isMinimized(){return this._minimized;}"
    "minimize(){"
    "this._minimized=true;this._visible=false;"
    "binding.invoke('browser-window.minimize',String(this.id));"
    "}"
    "restore(){"
    "this._minimized=false;this._visible=true;"
    "binding.invoke('browser-window.restore',String(this.id));"
    "}"
    "focus(){binding.invoke('browser-window.focus',String(this.id));}"
    "isMaximized(){return this._maximized;}"
    "maximize(){"
    "this._maximized=true;"
    "binding.invoke('browser-window.maximize',String(this.id));"
    "}"
    "unmaximize(){"
    "this._maximized=false;"
    "binding.invoke('browser-window.unmaximize',String(this.id));"
    "}"
    "close(){"
    "const index=browserWindows.indexOf(this);"
    "if(index>=0)browserWindows.splice(index,1);"
    "this._destroyed=true;"
    "this._visible=false;"
    "binding.invoke('browser-window.close',String(this.id));"
    "}"
    "static getAllWindows(){return browserWindows.slice();}"
    "static getFocusedWindow(){"
    "return browserWindows.length?browserWindows[browserWindows.length-1]:null;"
    "}"
    "}"
    "const normalizeDialogOptions=(windowOrOptions,maybeOptions)=>"
    "(windowOrOptions instanceof BrowserWindow?maybeOptions:windowOrOptions)||{};"
    "const serializeDialogOptions=options=>{"
    "const encode=value=>encodeURIComponent(value===undefined?'':String(value));"
    "const buttons=Array.isArray(options.buttons)&&options.buttons.length?"
    "options.buttons:['OK'];"
    "return [encode(options.title||appName),encode(options.message||''),"
    "encode(options.detail||''),buttons.map(encode).join('\\x1f')].join('\\t');"
    "};"
    "const dialog=Object.freeze({"
    "showMessageBoxSync:(windowOrOptions,maybeOptions)=>{"
    "const options=normalizeDialogOptions(windowOrOptions,maybeOptions);"
    "const response=Number(binding.invoke('dialog.show-message-box-sync',"
    "serializeDialogOptions(options)));"
    "return Number.isInteger(response)&&response>=0?response:"
    "(Number.isInteger(options.cancelId)?options.cancelId:0);"
    "},"
    "showMessageBox:(windowOrOptions,maybeOptions)=>"
    "Promise.resolve({response:dialog.showMessageBoxSync("
    "windowOrOptions,maybeOptions),checkboxChecked:false})"
    "});"
    "const api=Object.freeze({"
    "app,native,BrowserWindow,Menu,protocol,session,dialog,ipcMain,nativeTheme,"
    "powerMonitor,crashReporter,screen"
    "});"
    "const load=require;"
    "const Module=load('node:module');"
    "const moduleLoad=Module._load;"
    "const signalAppReady=()=>{"
    "if(appReady)return;"
    "appReady=true;"
    "app.emit('ready');"
    "resolveAppReady();"
    "};"
    "Module._load=function(request,parent,isMain){"
    "binding.invoke('module-load',String(request));"
    "if(request==='electron')return api;"
    "if(request==='original-fs')"
    "return moduleLoad.call(this,'fs',parent,isMain);"
    "const result=moduleLoad.apply(this,arguments);"
    "binding.invoke('module-return',String(request));"
    "if(globalThis.__arosElectronMainModuleRequest===String(request)){"
    "binding.invoke('main-module-ready',String(request));"
    "delete globalThis.__arosElectronMainModuleRequest;"
    "signalAppReady();"
    "}"
    "return result;"
    "};"
    "require=function(request){"
    "if(request==='electron')return api;"
    "if(request==='original-fs')return load('fs');"
    "return load(request);"
    "};"
    "setImmediate(signalAppReady);"
    "})();\n";

static LONG electron_queue_request(CONST_STRPTR method, CONST_STRPTR payload)
{
    ULONG next_tail;
    struct ElectronNativeRequest *request;
    const char *method_text =
        method != NULL ? (const char *)method : "";
    const char *payload_text =
        payload != NULL ? (const char *)payload : "";

    Forbid();
    next_tail = (electron_request_tail + 1) % ELECTRON_REQUEST_CAPACITY;
    if (next_tail == electron_request_head)
    {
        Permit();
        return 20;
    }
    request = &electron_requests[electron_request_tail];
    strncpy(request->method, method_text,
        sizeof(request->method) - 1);
    request->method[sizeof(request->method) - 1] = '\0';
    strncpy(request->payload, payload_text,
        sizeof(request->payload) - 1);
    request->payload[sizeof(request->payload) - 1] = '\0';
    electron_request_tail = next_tail;
    Permit();
    return 0;
}

static CONST_STRPTR electron_node_invoke(CONST_STRPTR method,
    CONST_STRPTR payload, APTR private_data)
{
    (void)payload;
    (void)private_data;
    if (method != NULL && strcmp(method, "ping") == 0)
        return "AROS Electron native binding";
    if (method != NULL &&
        (strcmp(method, "console-error") == 0 ||
        strcmp(method, "uncaught-exception") == 0 ||
        strcmp(method, "unhandled-rejection") == 0))
    {
        bug("[ELECTRON-JS] %s: %s\n", method,
            payload != NULL ? (const char *)payload : "");
        return "logged";
    }
    if (method != NULL && strcmp(method, "screen.snapshot") == 0)
        return electron_screen_snapshot();
    if (method != NULL &&
        strcmp(method, "dialog.show-message-box-sync") == 0)
        return electron_show_message_box(payload);
    if (method != NULL &&
        (strcmp(method, "module-load") == 0 ||
        strcmp(method, "module-return") == 0 ||
        strcmp(method, "main-module-ready") == 0))
    {
        bug("[ELECTRON-MODULE] %s '%s'\n",
            strcmp(method, "module-load") == 0 ? "require" :
            strcmp(method, "module-return") == 0 ? "returned" :
            "signalling ready after",
            payload != NULL ? (const char *)payload : "");
        return "traced";
    }
    if (method != NULL && strncmp(method, "crash-reporter.", 15) == 0)
    {
        bug("[ELECTRON-CRASH] %s '%s'\n", method,
            payload != NULL ? (const char *)payload : "");
        return strcmp(method, "crash-reporter.start") == 0 ?
            "diagnostics-active" : "upload-disabled";
    }
    if (method != NULL &&
        (strncmp(method, "browser-window.", 15) == 0 ||
        strcmp(method, "app.exit") == 0 ||
        strncmp(method, "session.", 8) == 0 ||
        strncmp(method, "protocol.", 9) == 0))
        return electron_queue_request(method, payload) == 0 ?
            "queued" : "queue-full";
    return "";
}

AROS_LH0(CONST_STRPTR, ElectronVersion,
    LIBBASETYPEPTR, LIBBASE, 5, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return AROS_ELECTRON_VERSION;
    AROS_LIBFUNC_EXIT
}

AROS_LH0(ULONG, ElectronCapabilities,
    LIBBASETYPEPTR, LIBBASE, 6, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return ELECTRON_CAP_CEF | ELECTRON_CAP_NODE |
        ELECTRON_CAP_MODULE_SHIM | ELECTRON_CAP_BROWSER_WINDOW_REQUESTS;
    AROS_LIBFUNC_EXIT
}

AROS_LH0(CONST_STRPTR, ElectronNodeVersion,
    LIBBASETYPEPTR, LIBBASE, 7, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return NodeVersion();
    AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, ElectronCEFVersionInfo,
    AROS_LHA(LONG, entry, D0),
    LIBBASETYPEPTR, LIBBASE, 8, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return cef_version_info(entry);
    AROS_LIBFUNC_EXIT
}

/*
 * Give the process ONE heap, before anything allocates.
 *
 * cef-blink.library allocates through PartitionAlloc; v8.library and
 * node.library fall through libstdc++ to stdc.library's malloc. Two heaps in
 * one process means an object allocated by Blink and destroyed by V8 - Blink's
 * background script streamer hands V8 exactly such a vector - is freed to the
 * wrong allocator and corrupts stdc's TLSF free list. PartitionAlloc is the
 * side to standardise on, because unlike stdc's malloc it needs no AROS
 * library base and so works on a Chromium-created thread.
 *
 * WHY HERE AND NOT LATER. Electron runs node first and initialises CEF
 * afterwards, and node uses V8. Installing the shared heap when CEF starts
 * would leave everything V8 allocated during node's phase owned by stdc and
 * freed by PartitionAlloc - the same bug, inverted. So this runs before
 * NodeStart(), and v8.library refuses a late install rather than honouring it.
 *
 * Best-effort by design: if cef-blink cannot supply an allocator, the process
 * keeps the old split-heap behaviour rather than failing to start. That is a
 * known bug, not a new one, and a node-only embedder never loads cef-blink at
 * all.
 */
static void ElectronEnsureSharedHeap(void)
{
    static BOOL attempted = FALSE;
    APTR allocFn = NULL;
    APTR freeFn = NULL;

    if (attempted)
        return;
    attempted = TRUE;

    CEFGetAllocator(&allocFn, &freeFn);
    if (allocFn == NULL || freeFn == NULL)
        return;

    /* A FALSE return means v8.library had already served an allocation from
       the default heap, so switching would free those objects to the wrong
       allocator; it refused rather than corrupt the heap. Not logged here on
       purpose: electron.library is a DISK library and kprintf/bug need
       _arosdebuglock from the kernel ROM, which makes any disk-loaded library
       that calls them fail to LINK on a pc target. An embedder that needs to
       know can call V8AllocatorInstall() itself and read the result. */
    (void)V8AllocatorInstall(allocFn, freeFn);
}

AROS_LH2(LONG, ElectronRunNode,
    AROS_LHA(LONG, argc, D0),
    AROS_LHA(STRPTR *, argv, A0),
    LIBBASETYPEPTR, LIBBASE, 9, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    ElectronEnsureSharedHeap();
    return NodeStart(argc, argv);
    AROS_LIBFUNC_EXIT
}

AROS_LH3(LONG, ElectronExecuteCEFProcess,
    AROS_LHA(APTR, args, A0),
    AROS_LHA(APTR, application, A1),
    AROS_LHA(APTR, sandbox_info, A2),
    LIBBASETYPEPTR, LIBBASE, 10, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    ElectronEnsureSharedHeap();
    return CEFExecuteProcess(args, application, sandbox_info);
    AROS_LIBFUNC_EXIT
}

AROS_LH4(LONG, ElectronInitializeCEF,
    AROS_LHA(APTR, args, A0),
    AROS_LHA(APTR, settings, A1),
    AROS_LHA(APTR, application, A2),
    AROS_LHA(APTR, sandbox_info, A3),
    LIBBASETYPEPTR, LIBBASE, 11, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    ElectronEnsureSharedHeap();
    return CEFInitialize(args, settings, application, sandbox_info);
    AROS_LIBFUNC_EXIT
}

AROS_LH0(void, ElectronDoCEFMessageLoopWork,
    LIBBASETYPEPTR, LIBBASE, 12, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    CEFDoMessageLoopWork();
    AROS_LIBFUNC_EXIT
}

AROS_LH0(void, ElectronShutdownCEF,
    LIBBASETYPEPTR, LIBBASE, 13, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    CEFShutdown();
    AROS_LIBFUNC_EXIT
}

AROS_LH0(LONG, ElectronPrepareNodeEmbedder,
    LIBBASETYPEPTR, LIBBASE, 14, Electron)
{
    LONG result;

    AROS_LIBFUNC_INIT
    (void)LIBBASE;

    result = NodeRegisterEmbedderLinkedBinding(
        "aros_electron", (APTR)electron_node_invoke, NULL);
    if (result != 0)
        return result;
    return NodePrepareEmbedder();
    AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, ElectronAttachNodeCurrentV8Context,
    AROS_LHA(CONST_STRPTR, script, A0),
    LIBBASETYPEPTR, LIBBASE, 15, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return NodeAttachCurrentV8Context(script);
    AROS_LIBFUNC_EXIT
}

AROS_LH0(LONG, ElectronPumpNodeEmbedder,
    LIBBASETYPEPTR, LIBBASE, 16, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return NodePumpEmbedder();
    AROS_LIBFUNC_EXIT
}

AROS_LH0(void, ElectronDetachNodeEmbedder,
    LIBBASETYPEPTR, LIBBASE, 17, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    NodeDetachEmbedder();
    AROS_LIBFUNC_EXIT
}

AROS_LH6(LONG, ElectronCreateCEFBrowser,
    AROS_LHA(APTR, window_info, A0),
    AROS_LHA(APTR, client, A1),
    AROS_LHA(APTR, url, A2),
    AROS_LHA(APTR, settings, A3),
    AROS_LHA(APTR, extra_info, D0),
    AROS_LHA(APTR, request_context, D1),
    LIBBASETYPEPTR, LIBBASE, 18, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return CEFCreateBrowser(window_info, client, url, settings, extra_info,
        request_context);
    AROS_LIBFUNC_EXIT
}

AROS_LH2(LONG, ElectronPostCEFTask,
    AROS_LHA(LONG, thread_id, D0),
    AROS_LHA(APTR, task, A0),
    LIBBASETYPEPTR, LIBBASE, 19, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return CEFPostTask(thread_id, task);
    AROS_LIBFUNC_EXIT
}

AROS_LH3(LONG, ElectronPostDelayedCEFTask,
    AROS_LHA(LONG, thread_id, D0),
    AROS_LHA(APTR, task, A0),
    AROS_LHA(LONG, delay_ms, D1),
    LIBBASETYPEPTR, LIBBASE, 20, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return CEFPostDelayedTask(thread_id, task, delay_ms);
    AROS_LIBFUNC_EXIT
}

AROS_LH3(LONG, ElectronRegisterCEFExtension,
    AROS_LHA(APTR, name, A0),
    AROS_LHA(APTR, javascript_code, A1),
    AROS_LHA(APTR, handler, A2),
    LIBBASETYPEPTR, LIBBASE, 21, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return CEFRegisterV8Extension(name, javascript_code, handler);
    AROS_LIBFUNC_EXIT
}

AROS_LH0(void, ElectronRunCEFMessageLoop,
    LIBBASETYPEPTR, LIBBASE, 22, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    CEFRunMessageLoop();
    AROS_LIBFUNC_EXIT
}

AROS_LH0(void, ElectronQuitCEFMessageLoop,
    LIBBASETYPEPTR, LIBBASE, 23, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    CEFQuitMessageLoop();
    AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, ElectronAttachNodeIsolatedV8Context,
    AROS_LHA(CONST_STRPTR, script, A0),
    LIBBASETYPEPTR, LIBBASE, 24, Electron)
{
    AROS_LIBFUNC_INIT
    (void)LIBBASE;
    return NodeAttachIsolatedV8Context(script);
    AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, ElectronAttachNodeIsolatedV8ContextWithModule,
    AROS_LHA(CONST_STRPTR, script, A0),
    LIBBASETYPEPTR, LIBBASE, 25, Electron)
{
    AROS_LIBFUNC_INIT
    const size_t bootstrap_length = sizeof(electron_module_bootstrap) - 1;
    const size_t script_length =
        script != NULL ? strlen((const char *)script) : 0;
    char *combined;
    LONG result;

    (void)LIBBASE;
    combined = AllocVec(bootstrap_length + script_length + 1, MEMF_ANY);
    if (combined == NULL)
        return 20;

    CopyMem(electron_module_bootstrap, combined, bootstrap_length);
    if (script_length != 0)
        CopyMem(script, combined + bootstrap_length, script_length);
    combined[bootstrap_length + script_length] = '\0';

    signal(SIGPIPE, SIG_IGN);
    result = NodeAttachIsolatedV8Context((CONST_STRPTR)combined);
    FreeVec(combined);
    return result;
    AROS_LIBFUNC_EXIT
}

AROS_LH4(LONG, ElectronPollNativeRequest,
    AROS_LHA(STRPTR, method, A0),
    AROS_LHA(ULONG, method_size, D0),
    AROS_LHA(STRPTR, payload, A1),
    AROS_LHA(ULONG, payload_size, D1),
    LIBBASETYPEPTR, LIBBASE, 26, Electron)
{
    AROS_LIBFUNC_INIT
    struct ElectronNativeRequest *request;

    (void)LIBBASE;
    if (method == NULL || method_size == 0 ||
        payload == NULL || payload_size == 0)
        return 20;

    Forbid();
    if (electron_request_head == electron_request_tail)
    {
        Permit();
        return 0;
    }
    request = &electron_requests[electron_request_head];
    strncpy(method, request->method, method_size - 1);
    method[method_size - 1] = '\0';
    strncpy(payload, request->payload, payload_size - 1);
    payload[payload_size - 1] = '\0';
    electron_request_head =
        (electron_request_head + 1) % ELECTRON_REQUEST_CAPACITY;
    Permit();
    return 1;
    AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, ElectronAttachNodeOwnedV8ContextWithModule,
    AROS_LHA(CONST_STRPTR, script, A0),
    LIBBASETYPEPTR, LIBBASE, 27, Electron)
{
    AROS_LIBFUNC_INIT
    const size_t bootstrap_length = sizeof(electron_module_bootstrap) - 1;
    const size_t script_length =
        script != NULL ? strlen((const char *)script) : 0;
    char *combined;
    LONG result;
    LONG worker_threads;

    (void)LIBBASE;
    combined = AllocVec(bootstrap_length + script_length + 1, MEMF_ANY);
    if (combined == NULL)
        return 20;

    CopyMem(electron_module_bootstrap, combined, bootstrap_length);
    if (script_length != 0)
        CopyMem(script, combined + bootstrap_length, script_length);
    combined[bootstrap_length + script_length] = '\0';

    worker_threads = CEFDefaultV8WorkerThreadCount();
    signal(SIGPIPE, SIG_IGN);
    result = NodeConfigureOwnedV8WorkerThreads(worker_threads);
    if (result == 0)
        result = NodeAttachOwnedV8Context((CONST_STRPTR)combined);
    FreeVec(combined);
    return result;
    AROS_LIBFUNC_EXIT
}
