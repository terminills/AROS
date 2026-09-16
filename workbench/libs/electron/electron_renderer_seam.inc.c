/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 *
 * Renderer and protocol seam of electron.library.
 *
 * Included by electron_bridge.c (not compiled separately) so the seam shares
 * the request queue and the node binding without exporting private symbols.
 *
 * Three data paths meet here:
 *
 *  - Custom-scheme requests (vscode-file://...): CEF asks the scheme factory
 *    for a resource handler on the IO thread; the handler parks the request in
 *    the pending list, posts a `protocol` event, and continues once the node
 *    main script answered through `protocol.response` with a native file path
 *    (served by DOS) or an error code.
 *
 *  - Renderer -> main: the `aros/electron_renderer` V8 extension gives page
 *    scripts `AROSElectronRenderer.send(text)`; its native handler posts an
 *    `ipc` event that the bootstrap's poll loop dispatches to ipcMain.
 *
 *  - Main -> renderer: `web-contents.send` runs
 *    `AROSElectronRenderer.deliver(<json literal>)` in the active browser's
 *    main frame through CefFrame::ExecuteJavaScript.
 *
 * Events are a semaphore-guarded list of heap strings, popped one at a time by
 * `native.poll`; the popped text stays valid until the next poll, which is
 * enough because node.library copies binding results into a V8 string before
 * returning to script.
 */

#define ELECTRON_MAX_SCHEMES 8

struct ElectronEvent
{
    struct MinNode node;
    char *text;
    /* Which app this event belongs to when one process hosts several of them
       (JOB-0098 option C). 0 means "any app may take it", which is what a
       single-app process produces and consumes. */
    LONG slot;
};

struct ElectronResource
{
    cef_resource_handler_t handler;     /* must stay first: CEF hands it back */
    struct MinNode node;                /* pending list membership */
    LONG refcount;
    ULONG id;
    char *scheme;
    char *url;
    cef_callback_t *open_callback;
    char *path;
    char *headers;                      /* flat JSON object from the handler */
    LONG error;                         /* cef_errorcode_t, 0 = serve path */
    BPTR file;
    LONG length;
};

static int electron_state_ready;
static struct SignalSemaphore electron_event_lock;
static struct MinList electron_events;
static char *electron_event_current;
static struct SignalSemaphore electron_resource_lock;
static struct MinList electron_pending_resources;
static ULONG electron_resource_next_id;
static cef_scheme_handler_factory_t electron_scheme_factory;
static int electron_scheme_factory_ready;
static cef_v8handler_t electron_renderer_handler;
static char *electron_declared_schemes[ELECTRON_MAX_SCHEMES];
static ULONG electron_declared_scheme_count;
static cef_browser_t *electron_active_browser;
static ULONG electron_window_id;
static char *electron_window_preload_path;
static char *electron_window_preload_source;
static char *electron_window_process_snapshot;

static const cef_char_t electron_renderer_extension_name[] =
    u"aros/electron_renderer";
static const cef_char_t electron_renderer_extension_code[] =
    u"var AROSElectronRenderer;"
    u"if(!AROSElectronRenderer)AROSElectronRenderer={};"
    u"(function(){"
    u"AROSElectronRenderer.send=function(text){"
    u"native function AROSElectronSend(text);"
    u"return AROSElectronSend(text);"
    u"};"
    u"})();";
static const cef_char_t electron_renderer_script_url[] =
    u"aros://electron/preload";
static const cef_char_t electron_renderer_deliver_url[] =
    u"aros://electron/deliver";
static const cef_char_t electron_renderer_execute_url[] =
    u"aros://electron/execute-script";

/*
 * The renderer-side `require('electron')` for a sandboxed, context-isolated
 * preload: ipcRenderer, webFrame, contextBridge, webUtils and a process shim
 * built from the snapshot the main bootstrap took when it created the window.
 * The preload source is spliced in between the two halves and runs with the
 * CommonJS wrapper signature so its `require`/`module`/`process` resolve here.
 */
static const char electron_renderer_runtime_head[] =
    "(function(){"
    "const snapshot=";
static const char electron_renderer_runtime_middle[] =
    ";"
    "const preloadPath=";
static const char electron_renderer_runtime_body[] =
    ";"
    "let nextId=1;"
    "const pending=new Map();"
    "const listeners=new Map();"
    "const binToBase64=bytes=>{"
    "let text='';"
    "for(let index=0;index<bytes.length;index+=8192)"
    "text+=String.fromCharCode.apply(null,bytes.subarray(index,index+8192));"
    "return btoa(text);"
    "};"
    "const base64ToBin=text=>{"
    "const raw=atob(text);"
    "const bytes=new Uint8Array(raw.length);"
    "for(let index=0;index<raw.length;index++)bytes[index]=raw.charCodeAt(index);"
    "return bytes;"
    "};"
    "const replacer=(key,value)=>{"
    "if(value instanceof Uint8Array)return {__arosBin:binToBase64(value)};"
    "if(value instanceof ArrayBuffer)"
    "return {__arosBin:binToBase64(new Uint8Array(value))};"
    "return value;"
    "};"
    "const reviver=(key,value)=>"
    "(value&&typeof value==='object'&&typeof value.__arosBin==='string')?"
    "base64ToBin(value.__arosBin):value;"
    "const post=message=>AROSElectronRenderer.send("
    "JSON.stringify(message,replacer));"
    "const ipcRenderer={"
    "invoke:(channel,...args)=>new Promise((resolve,reject)=>{"
    "const id=nextId++;"
    "pending.set(id,{resolve,reject});"
    "post({type:'invoke',id,channel:String(channel),args});"
    "}),"
    "send:(channel,...args)=>{post({type:'send',channel:String(channel),args});},"
    "sendSync:channel=>{"
    "throw new Error('ipcRenderer.sendSync('+channel+') is not supported on AROS');"
    "},"
    "postMessage:(channel,message)=>{"
    "post({type:'send',channel:String(channel),args:[message]});"
    "},"
    "on:(channel,listener)=>{"
    "const key=String(channel);"
    "if(!listeners.has(key))listeners.set(key,new Set());"
    "listeners.get(key).add(listener);"
    "return ipcRenderer;"
    "},"
    "once:(channel,listener)=>{"
    "const wrapper=(...values)=>{"
    "ipcRenderer.removeListener(channel,wrapper);"
    "listener(...values);"
    "};"
    "wrapper._arosOriginal=listener;"
    "return ipcRenderer.on(channel,wrapper);"
    "},"
    "removeListener:(channel,listener)=>{"
    "const set=listeners.get(String(channel));"
    "if(set)for(const entry of [...set])"
    "if(entry===listener||entry._arosOriginal===listener)set.delete(entry);"
    "return ipcRenderer;"
    "},"
    "off:(channel,listener)=>ipcRenderer.removeListener(channel,listener),"
    "removeAllListeners:channel=>{"
    "listeners.delete(String(channel));"
    "return ipcRenderer;"
    "}"
    "};"
    /* A MessagePortMain the main process transferred to this window arrives
       as a port id: pair it with a DOM MessageChannel, keep the near end as
       the relay (renderer->main port-message), hand the far end to the page
       as event.ports[0] so window.postMessage(nonce,'*',ports) transfers a
       real MessagePort into the workbench. */
    "const rendererPorts=new Map();"
    "const importPort=id=>{"
    "const channel=new MessageChannel();"
    "rendererPorts.set(id,channel.port1);"
    "channel.port1.onmessage=event=>{"
    "post({type:'port-message',port:id,data:event.data});"
    "};"
    "return channel.port2;"
    "};"
    "const importPorts=ids=>Array.isArray(ids)?ids.map(importPort):[];"
    "AROSElectronRenderer.deliver=text=>{"
    "let message;"
    "try{message=JSON.parse(text,reviver);}"
    "catch(error){console.error('[AROS-ELECTRON] bad delivery: '+error);return;}"
    "if(message.type==='reply'){"
    "const entry=pending.get(message.id);"
    "if(!entry)return;"
    "pending.delete(message.id);"
    "if(message.error!==undefined)entry.reject(new Error(String(message.error)));"
    "else entry.resolve(message.result);"
    "return;"
    "}"
    "if(message.type==='port-message'){"
    "const port=rendererPorts.get(message.port);"
    "if(port)port.postMessage(message.data,importPorts(message.ports));"
    "return;"
    "}"
    "if(message.type==='port-close'){"
    "const port=rendererPorts.get(message.port);"
    "if(port){rendererPorts.delete(message.port);port.close();}"
    "return;"
    "}"
    "if(message.type==='message'){"
    "const ports=importPorts(message.ports);"
    "const set=listeners.get(message.channel);"
    "if(!set)return;"
    "const event={sender:ipcRenderer,senderId:0,ports,"
    "reply:(channel,...args)=>ipcRenderer.send(channel,...args)};"
    "for(const listener of [...set])listener(event,...(message.args||[]));"
    "}"
    "};"
    "const webFrame={"
    "setZoomLevel:level=>{"
    "post({type:'web-frame.set-zoom-level',level:Number(level)||0});"
    "},"
    "getZoomLevel:()=>0,"
    "setZoomFactor:()=>{},"
    "getZoomFactor:()=>1"
    "};"
    "const contextBridge={"
    "exposeInMainWorld:(name,value)=>{globalThis[String(name)]=value;}"
    "};"
    "const webUtils={"
    "getPathForFile:file=>(file&&typeof file.path==='string')?file.path:''"
    "};"
    "const electron={ipcRenderer,webFrame,contextBridge,webUtils};"
    "const processListeners=new Map();"
    "const process={"
    "type:'renderer',"
    "platform:snapshot.platform,"
    "arch:snapshot.arch,"
    "env:snapshot.env||{},"
    "versions:snapshot.versions||{},"
    "execPath:snapshot.execPath||'',"
    "pid:snapshot.pid||0,"
    "argv:snapshot.argv||[],"
    "sandboxed:true,"
    "contextIsolated:true,"
    "cwd:()=>snapshot.cwd||'',"
    "on:(name,listener)=>{"
    "const key=String(name);"
    "if(!processListeners.has(key))processListeners.set(key,new Set());"
    "processListeners.get(key).add(listener);"
    "return process;"
    "},"
    "once:(name,listener)=>process.on(name,listener),"
    "removeListener:(name,listener)=>{"
    "const set=processListeners.get(String(name));"
    "if(set)set.delete(listener);"
    "return process;"
    "},"
    "getProcessMemoryInfo:()=>ipcRenderer.invoke('aros:process-memory-info'),"
    "getSystemVersion:()=>'',"
    "nextTick:callback=>{Promise.resolve().then(callback);}"
    "};"
    "const require=name=>{"
    "if(name==='electron')return electron;"
    "throw new Error('Cannot find module \\''+name+"
    "'\\' in the AROS Electron renderer');"
    "};"
    "const module={exports:{}};"
    "const preloadDirectory=preloadPath.slice(0,Math.max("
    "preloadPath.lastIndexOf('/'),preloadPath.lastIndexOf(':')+1));"
    "try{"
    "(function(require,module,exports,process,__filename,__dirname,global){\n";
static const char electron_renderer_runtime_tail[] =
    "\n}).call(globalThis,require,module,module.exports,process,preloadPath,"
    "preloadDirectory,globalThis);"
    "}catch(error){"
    "console.error('[AROS-ELECTRON] preload failed: '+"
    "(error&&error.stack||error));"
    "}"
    "})();";

static void electron_ensure_state(void)
{
    Forbid();
    if (!electron_state_ready)
    {
        NEWLIST((struct List *)&electron_events);
        NEWLIST((struct List *)&electron_pending_resources);
        InitSemaphore(&electron_event_lock);
        InitSemaphore(&electron_resource_lock);
        electron_state_ready = 1;
    }
    Permit();
}

static char *electron_strdup(const char *text)
{
    size_t length = text != NULL ? strlen(text) : 0;
    char *copy = AllocVec(length + 1, MEMF_ANY);

    if (copy == NULL)
        return NULL;
    if (length != 0)
        CopyMem((APTR)text, copy, length);
    copy[length] = '\0';
    return copy;
}

static char *electron_concat(const char *const *parts, ULONG count)
{
    size_t total = 0;
    ULONG index;
    char *result;
    char *cursor;

    for (index = 0; index < count; ++index)
        total += parts[index] != NULL ? strlen(parts[index]) : 0;
    result = AllocVec(total + 1, MEMF_ANY);
    if (result == NULL)
        return NULL;
    cursor = result;
    for (index = 0; index < count; ++index)
    {
        size_t length = parts[index] != NULL ? strlen(parts[index]) : 0;

        if (length != 0)
            CopyMem((APTR)parts[index], cursor, length);
        cursor += length;
    }
    *cursor = '\0';
    return result;
}

static LONG electron_parse_long(const char *text)
{
    LONG value = 0;
    int negative = 0;

    if (text == NULL)
        return 0;
    if (*text == '-')
    {
        negative = 1;
        ++text;
    }
    while (*text >= '0' && *text <= '9')
        value = value * 10 + (*text++ - '0');
    return negative ? -value : value;
}

static char *electron_utf16_to_utf8(const cef_char_t *text, size_t length)
{
    size_t index;
    size_t size = 0;
    char *result;
    char *out;

    if (text == NULL)
        length = 0;
    for (index = 0; index < length; ++index)
    {
        ULONG code = text[index];

        if (code >= 0xD800 && code <= 0xDBFF && index + 1 < length &&
            text[index + 1] >= 0xDC00 && text[index + 1] <= 0xDFFF)
        {
            size += 4;
            ++index;
        }
        else if (code < 0x80)
            size += 1;
        else if (code < 0x800)
            size += 2;
        else
            size += 3;
    }
    result = AllocVec(size + 1, MEMF_ANY);
    if (result == NULL)
        return NULL;
    out = result;
    for (index = 0; index < length; ++index)
    {
        ULONG code = text[index];

        if (code >= 0xD800 && code <= 0xDBFF && index + 1 < length &&
            text[index + 1] >= 0xDC00 && text[index + 1] <= 0xDFFF)
        {
            code = 0x10000 + ((code - 0xD800) << 10) +
                (text[index + 1] - 0xDC00);
            ++index;
        }
        if (code < 0x80)
            *out++ = (char)code;
        else if (code < 0x800)
        {
            *out++ = (char)(0xC0 | (code >> 6));
            *out++ = (char)(0x80 | (code & 0x3F));
        }
        else if (code < 0x10000)
        {
            *out++ = (char)(0xE0 | (code >> 12));
            *out++ = (char)(0x80 | ((code >> 6) & 0x3F));
            *out++ = (char)(0x80 | (code & 0x3F));
        }
        else
        {
            *out++ = (char)(0xF0 | (code >> 18));
            *out++ = (char)(0x80 | ((code >> 12) & 0x3F));
            *out++ = (char)(0x80 | ((code >> 6) & 0x3F));
            *out++ = (char)(0x80 | (code & 0x3F));
        }
    }
    *out = '\0';
    return result;
}

static cef_char_t *electron_utf8_to_utf16(const char *text,
    size_t *length_out)
{
    const unsigned char *in = (const unsigned char *)(text != NULL ? text : "");
    size_t length = strlen((const char *)in);
    size_t index = 0;
    size_t out = 0;
    cef_char_t *result = AllocVec((length + 1) * sizeof(cef_char_t), MEMF_ANY);

    if (result == NULL)
        return NULL;
    while (index < length)
    {
        ULONG code = in[index];
        size_t extra = 0;

        if (code >= 0xF0 && code < 0xF8)
        {
            code &= 0x07;
            extra = 3;
        }
        else if (code >= 0xE0)
        {
            code &= 0x0F;
            extra = 2;
        }
        else if (code >= 0xC0)
        {
            code &= 0x1F;
            extra = 1;
        }
        else if (code >= 0x80)
            code = 0xFFFD;
        ++index;
        while (extra != 0 && index < length && (in[index] & 0xC0) == 0x80)
        {
            code = (code << 6) | (in[index] & 0x3F);
            ++index;
            --extra;
        }
        if (extra != 0 || code > 0x10FFFF)
            code = 0xFFFD;
        if (code >= 0x10000)
        {
            code -= 0x10000;
            result[out++] = (cef_char_t)(0xD800 + (code >> 10));
            result[out++] = (cef_char_t)(0xDC00 + (code & 0x3FF));
        }
        else
            result[out++] = (cef_char_t)code;
    }
    result[out] = 0;
    if (length_out != NULL)
        *length_out = out;
    return result;
}

static void electron_set_cef_string(cef_string_t *target,
    const cef_char_t *value, size_t length)
{
    target->str = (cef_char_t *)value;
    target->length = length;
    target->dtor = NULL;
}

/* ASCII into a caller-provided UTF-16 buffer, for the short fixed strings. */
static void electron_set_ascii_string(cef_string_t *target,
    cef_char_t *buffer, size_t capacity, const char *ascii)
{
    size_t index = 0;

    while (ascii[index] != '\0' && index + 1 < capacity)
    {
        buffer[index] = (cef_char_t)(unsigned char)ascii[index];
        ++index;
    }
    buffer[index] = 0;
    electron_set_cef_string(target, buffer, index);
}

static char *electron_userfree_to_utf8(cef_string_userfree_t text)
{
    char *result;

    if (text == NULL)
        return electron_strdup("");
    result = electron_utf16_to_utf8(text->str, text->length);
    CEFStringUserfreeFree(text);
    return result;
}

/*
 * Which app is being serviced right now.
 *
 * With several apps in one process each has its own Node runtime, and each of
 * those runtimes polls this queue from its own pump. Without a tag the first
 * poller swallows every event, including the did-finish-load another app's
 * window is waiting on to become visible - so events carry the slot that was
 * current when they were posted, and a poll only takes its own (or untagged
 * ones, which is every event a single-app process makes).
 */
static LONG electron_current_slot;

/* Takes ownership of text. */
static LONG electron_post_event_for(char *text, LONG slot)
{
    struct ElectronEvent *event;

    if (text == NULL)
        return 20;
    event = AllocVec(sizeof(*event), MEMF_ANY);
    if (event == NULL)
    {
        FreeVec(text);
        return 20;
    }
    event->text = text;
    event->slot = slot;
    electron_ensure_state();
    ObtainSemaphore(&electron_event_lock);
    AddTail((struct List *)&electron_events, (struct Node *)&event->node);
    ReleaseSemaphore(&electron_event_lock);
    return 0;
}

static LONG electron_post_event(char *text)
{
    return electron_post_event_for(text, electron_current_slot);
}

/*
 * The shell owns the native window, so window-system facts (the user closed
 * it, resized it, ...) enter the Electron object model from there:
 * `window\t<id>\t<name>\t<payload>` reaches the bootstrap's poll loop,
 * which turns it into the BrowserWindow event of the same name.
 */
static LONG electron_post_window_event(CONST_STRPTR name, CONST_STRPTR payload)
{
    char id[16];
    const char *parts[6];
    ULONG value = electron_window_id;
    int index = sizeof(id) - 1;

    if (name == NULL || name[0] == '\0')
        return 20;
    id[index] = '\0';
    do
    {
        id[--index] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0 && index > 0);
    parts[0] = "window\t";
    parts[1] = id + index;
    parts[2] = "\t";
    parts[3] = (const char *)name;
    parts[4] = "\t";
    parts[5] = payload != NULL ? (const char *)payload : "";
    return electron_post_event(electron_concat(parts, 6));
}

static CONST_STRPTR electron_poll_event(void)
{
    struct ElectronEvent *event;

    electron_ensure_state();
    if (electron_event_current != NULL)
    {
        FreeVec(electron_event_current);
        electron_event_current = NULL;
    }
    ObtainSemaphore(&electron_event_lock);
    {
        struct ElectronEvent *scan;
        struct ElectronEvent *next;

        event = NULL;
        for (scan = (struct ElectronEvent *)electron_events.mlh_Head;
             (next = (struct ElectronEvent *)scan->node.mln_Succ) != NULL;
             scan = next)
        {
            if (scan->slot == 0 || scan->slot == electron_current_slot)
            {
                Remove((struct Node *)&scan->node);
                event = scan;
                break;
            }
        }
    }
    ReleaseSemaphore(&electron_event_lock);
    if (event == NULL)
        return "";
    electron_event_current = event->text;
    FreeVec(event);
    return electron_event_current;
}

/* ---- scheme handler factory + resource handler ---------------------- */

static void electron_resource_dispose(struct ElectronResource *resource)
{
    if (resource->open_callback != NULL)
        resource->open_callback->base.release(&resource->open_callback->base);
    if (resource->file != BNULL)
        Close(resource->file);
    FreeVec(resource->scheme);
    FreeVec(resource->url);
    FreeVec(resource->path);
    FreeVec(resource->headers);
    FreeVec(resource);
}

static void CEF_CALLBACK electron_resource_add_ref(
    cef_base_ref_counted_t *base)
{
    struct ElectronResource *resource = (struct ElectronResource *)base;

    __sync_add_and_fetch(&resource->refcount, 1);
}

static int CEF_CALLBACK electron_resource_release(
    cef_base_ref_counted_t *base)
{
    struct ElectronResource *resource = (struct ElectronResource *)base;

    if (__sync_sub_and_fetch(&resource->refcount, 1) == 0)
    {
        electron_resource_dispose(resource);
        return 1;
    }
    return 0;
}

static int CEF_CALLBACK electron_resource_has_one_ref(
    cef_base_ref_counted_t *base)
{
    return ((struct ElectronResource *)base)->refcount == 1;
}

static int CEF_CALLBACK electron_resource_has_at_least_one_ref(
    cef_base_ref_counted_t *base)
{
    return ((struct ElectronResource *)base)->refcount >= 1;
}

static int CEF_CALLBACK electron_resource_open(cef_resource_handler_t *self,
    cef_request_t *request, int *handle_request, cef_callback_t *callback)
{
    struct ElectronResource *resource = (struct ElectronResource *)self;
    char id_text[16];
    const char *parts[6];
    char *text;

    (void)request;
    *handle_request = 0;
    *electron_append_long(id_text, (LONG)resource->id) = '\0';
    parts[0] = "protocol\t";
    parts[1] = id_text;
    parts[2] = "\t";
    parts[3] = resource->scheme;
    parts[4] = "\t";
    parts[5] = resource->url;
    text = electron_concat(parts, 6);
    if (text == NULL)
        return 0;

    callback->base.add_ref(&callback->base);
    resource->open_callback = callback;
    /* The pending list holds its own reference until the script answers. */
    electron_resource_add_ref(&self->base);
    ObtainSemaphore(&electron_resource_lock);
    AddTail((struct List *)&electron_pending_resources,
        (struct Node *)&resource->node);
    ReleaseSemaphore(&electron_resource_lock);
    electron_post_event(text);
    return 1;
}

static const char *electron_mime_for_path(const char *path)
{
    static const struct
    {
        const char *extension;
        const char *mime;
    } table[] = {
        { "html", "text/html" }, { "htm", "text/html" },
        { "js", "text/javascript" }, { "mjs", "text/javascript" },
        { "css", "text/css" }, { "json", "application/json" },
        { "svg", "image/svg+xml" }, { "png", "image/png" },
        { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" },
        { "gif", "image/gif" }, { "webp", "image/webp" },
        { "ico", "image/x-icon" }, { "woff", "font/woff" },
        { "woff2", "font/woff2" }, { "ttf", "font/ttf" },
        { "otf", "font/otf" }, { "wasm", "application/wasm" },
        { "map", "application/json" }, { "txt", "text/plain" },
        { "md", "text/markdown" }, { "xml", "text/xml" },
        { "mp3", "audio/mpeg" }, { "mp4", "video/mp4" },
        { "webm", "video/webm" }, { "wav", "audio/wav" },
    };
    const char *dot = NULL;
    const char *cursor;
    ULONG index;

    for (cursor = path; *cursor != '\0'; ++cursor)
        if (*cursor == '.')
            dot = cursor + 1;
        else if (*cursor == '/' || *cursor == ':')
            dot = NULL;
    if (dot == NULL)
        return "application/octet-stream";
    for (index = 0; index < sizeof(table) / sizeof(table[0]); ++index)
    {
        const char *a = table[index].extension;
        const char *b = dot;

        while (*a != '\0' && *b != '\0' && *a == (*b | 0x20))
        {
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0')
            return table[index].mime;
    }
    return "application/octet-stream";
}

/* Read one JSON string starting at `*cursor` (pointing at the quote). */
static char *electron_json_string(const char **cursor)
{
    const char *read = *cursor;
    char *result;
    char *write;

    if (*read != '"')
        return NULL;
    result = AllocVec(strlen(read) + 1, MEMF_ANY);
    if (result == NULL)
        return NULL;
    write = result;
    ++read;
    while (*read != '\0' && *read != '"')
    {
        if (*read == '\\' && read[1] != '\0')
        {
            ++read;
            switch (*read)
            {
            case 'n': *write++ = '\n'; break;
            case 't': *write++ = '\t'; break;
            case 'r': *write++ = '\r'; break;
            default: *write++ = *read; break;
            }
            ++read;
        }
        else
            *write++ = *read++;
    }
    *write = '\0';
    if (*read == '"')
        ++read;
    *cursor = read;
    return result;
}

static void electron_apply_headers(cef_response_t *response,
    const char *headers)
{
    const char *cursor = headers;

    while (*cursor != '\0')
    {
        char *name;
        char *value;

        while (*cursor != '\0' && *cursor != '"')
            ++cursor;
        if (*cursor == '\0')
            break;
        name = electron_json_string(&cursor);
        while (*cursor == ' ' || *cursor == ':')
            ++cursor;
        value = electron_json_string(&cursor);
        if (name != NULL && value != NULL)
        {
            cef_string_t name16;
            cef_string_t value16;
            size_t name_length;
            size_t value_length;
            cef_char_t *name_text = electron_utf8_to_utf16(name, &name_length);
            cef_char_t *value_text =
                electron_utf8_to_utf16(value, &value_length);

            if (name_text != NULL && value_text != NULL)
            {
                electron_set_cef_string(&name16, name_text, name_length);
                electron_set_cef_string(&value16, value_text, value_length);
                response->set_header_by_name(response, &name16, &value16, 1);
            }
            FreeVec(name_text);
            FreeVec(value_text);
        }
        FreeVec(name);
        FreeVec(value);
        while (*cursor != '\0' && *cursor != ',')
        {
            if (*cursor == '}')
                return;
            ++cursor;
        }
        if (*cursor == ',')
            ++cursor;
    }
}

static void CEF_CALLBACK electron_resource_get_response_headers(
    cef_resource_handler_t *self, cef_response_t *response,
    int64_t *response_length, cef_string_t *redirect_url)
{
    struct ElectronResource *resource = (struct ElectronResource *)self;
    cef_char_t text_buffer[96];
    cef_string_t text;

    (void)redirect_url;
    *response_length = 0;
    if (resource->error != 0 || resource->path == NULL)
    {
        response->set_error(response,
            resource->error != 0 ? resource->error : ERR_FAILED);
        return;
    }

    resource->file = Open(resource->path, MODE_OLDFILE);
    if (resource->file == BNULL)
    {
        bug("[ELECTRON-PROTOCOL] cannot open '%s' for %s (IoErr %ld)\n",
            resource->path, resource->url, (long)IoErr());
        response->set_error(response, ERR_FILE_NOT_FOUND);
        return;
    }
    Seek(resource->file, 0, OFFSET_END);
    resource->length = Seek(resource->file, 0, OFFSET_BEGINNING);

    response->set_status(response, 200);
    electron_set_ascii_string(&text, text_buffer,
        sizeof(text_buffer) / sizeof(text_buffer[0]), "OK");
    response->set_status_text(response, &text);
    electron_set_ascii_string(&text, text_buffer,
        sizeof(text_buffer) / sizeof(text_buffer[0]),
        electron_mime_for_path(resource->path));
    response->set_mime_type(response, &text);
    if (resource->headers != NULL)
        electron_apply_headers(response, resource->headers);
    *response_length = resource->length >= 0 ? resource->length : -1;
}

static int CEF_CALLBACK electron_resource_read(cef_resource_handler_t *self,
    void *data_out, int bytes_to_read, int *bytes_read,
    cef_resource_read_callback_t *callback)
{
    struct ElectronResource *resource = (struct ElectronResource *)self;
    LONG count;

    (void)callback;
    if (resource->file == BNULL)
    {
        *bytes_read = 0;
        return 0;
    }
    count = Read(resource->file, data_out, bytes_to_read);
    if (count > 0)
    {
        *bytes_read = count;
        return 1;
    }
    *bytes_read = count == 0 ? 0 : ERR_FAILED;
    return 0;
}

static void CEF_CALLBACK electron_resource_cancel(cef_resource_handler_t *self)
{
    struct ElectronResource *resource = (struct ElectronResource *)self;
    BPTR file;

    Forbid();
    file = resource->file;
    resource->file = BNULL;
    Permit();
    if (file != BNULL)
        Close(file);
}

static cef_resource_handler_t *CEF_CALLBACK electron_scheme_factory_create(
    cef_scheme_handler_factory_t *self, cef_browser_t *browser,
    cef_frame_t *frame, const cef_string_t *scheme_name,
    cef_request_t *request)
{
    struct ElectronResource *resource;

    (void)self;
    (void)browser;
    (void)frame;
    resource = AllocVec(sizeof(*resource), MEMF_CLEAR);
    if (resource == NULL)
        return NULL;
    resource->handler.base.size = sizeof(cef_resource_handler_t);
    resource->handler.base.add_ref = electron_resource_add_ref;
    resource->handler.base.release = electron_resource_release;
    resource->handler.base.has_one_ref = electron_resource_has_one_ref;
    resource->handler.base.has_at_least_one_ref =
        electron_resource_has_at_least_one_ref;
    resource->handler.open = electron_resource_open;
    resource->handler.get_response_headers =
        electron_resource_get_response_headers;
    resource->handler.read = electron_resource_read;
    resource->handler.cancel = electron_resource_cancel;
    resource->refcount = 1;
    resource->scheme = scheme_name != NULL ?
        electron_utf16_to_utf8(scheme_name->str, scheme_name->length) :
        electron_strdup("");
    resource->url = electron_userfree_to_utf8(request->get_url(request));
    if (resource->scheme == NULL || resource->url == NULL)
    {
        electron_resource_dispose(resource);
        return NULL;
    }
    electron_ensure_state();
    ObtainSemaphore(&electron_resource_lock);
    resource->id = ++electron_resource_next_id;
    ReleaseSemaphore(&electron_resource_lock);
    return &resource->handler;
}

static void CEF_CALLBACK electron_static_add_ref(cef_base_ref_counted_t *base)
{
    (void)base;
}

static int CEF_CALLBACK electron_static_release(cef_base_ref_counted_t *base)
{
    (void)base;
    return 0;
}

static int CEF_CALLBACK electron_static_has_ref(cef_base_ref_counted_t *base)
{
    (void)base;
    return 1;
}

static void electron_ensure_scheme_factory(void)
{
    if (electron_scheme_factory_ready)
        return;
    memset(&electron_scheme_factory, 0, sizeof(electron_scheme_factory));
    electron_scheme_factory.base.size = sizeof(electron_scheme_factory);
    electron_scheme_factory.base.add_ref = electron_static_add_ref;
    electron_scheme_factory.base.release = electron_static_release;
    electron_scheme_factory.base.has_one_ref = electron_static_has_ref;
    electron_scheme_factory.base.has_at_least_one_ref = electron_static_has_ref;
    electron_scheme_factory.create = electron_scheme_factory_create;
    electron_scheme_factory_ready = 1;
}

static CONST_STRPTR electron_register_scheme_factory(CONST_STRPTR scheme,
    int remove)
{
    cef_string_t name;
    cef_char_t *text;
    size_t length;
    LONG rc;

    if (scheme == NULL || *scheme == '\0')
        return "no-scheme";
    electron_ensure_scheme_factory();
    text = electron_utf8_to_utf16((const char *)scheme, &length);
    if (text == NULL)
        return "no-memory";
    electron_set_cef_string(&name, text, length);
    rc = CEFRegisterSchemeHandlerFactory(&name, NULL,
        remove ? NULL : &electron_scheme_factory);
    FreeVec(text);
    bug("[ELECTRON-PROTOCOL] scheme '%s' factory %s rc=%ld\n",
        (const char *)scheme, remove ? "removed" : "registered", (long)rc);
    return rc != 0 ? "registered" : "register-failed";
}

/* `id \t ok|<errcode> \t path \t headers` from the protocol handler. */
static CONST_STRPTR electron_protocol_response(CONST_STRPTR payload)
{
    char *copy;
    char *fields[4] = { NULL, NULL, NULL, NULL };
    char *cursor;
    ULONG field_count = 0;
    ULONG id;
    struct ElectronResource *resource = NULL;
    struct MinNode *node;
    cef_callback_t *callback;

    copy = electron_strdup(payload != NULL ? (const char *)payload : "");
    if (copy == NULL)
        return "no-memory";
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
    id = (ULONG)electron_parse_long(fields[0]);

    electron_ensure_state();
    ObtainSemaphore(&electron_resource_lock);
    /* The list node is not the first member, so walk MinNodes and recover
       the container by hand instead of using ForeachNode. */
    for (node = electron_pending_resources.mlh_Head; node->mln_Succ != NULL;
        node = node->mln_Succ)
    {
        struct ElectronResource *candidate = (struct ElectronResource *)
            ((char *)node - offsetof(struct ElectronResource, node));

        if (candidate->id == id)
        {
            Remove((struct Node *)node);
            resource = candidate;
            break;
        }
    }
    ReleaseSemaphore(&electron_resource_lock);
    if (resource == NULL)
    {
        bug("[ELECTRON-PROTOCOL] response for unknown request %lu\n",
            (unsigned long)id);
        FreeVec(copy);
        return "unknown";
    }

    if (strcmp(fields[1], "ok") == 0)
    {
        resource->error = 0;
        resource->path = electron_strdup(fields[2]);
        if (fields[3][0] != '\0')
            resource->headers = electron_strdup(fields[3]);
    }
    else
    {
        resource->error = electron_parse_long(fields[1]);
        if (resource->error == 0)
            resource->error = ERR_FAILED;
        bug("[ELECTRON-PROTOCOL] %s -> error %ld\n", resource->url,
            (long)resource->error);
    }
    FreeVec(copy);

    callback = resource->open_callback;
    resource->open_callback = NULL;
    if (callback != NULL)
    {
        callback->cont(callback);
        callback->base.release(&callback->base);
    }
    electron_resource_release(&resource->handler.base);
    return "continued";
}

/* ---- declared schemes ------------------------------------------------ */

static LONG electron_declare_scheme(CONST_STRPTR name, ULONG options)
{
    char *copy;

    (void)options;
    if (name == NULL || *name == '\0' ||
        electron_declared_scheme_count >= ELECTRON_MAX_SCHEMES)
        return 20;
    copy = electron_strdup((const char *)name);
    if (copy == NULL)
        return 20;
    electron_declared_schemes[electron_declared_scheme_count++] = copy;
    return 0;
}

static CONST_STRPTR electron_check_privileged_schemes(CONST_STRPTR payload)
{
    char *copy = electron_strdup(payload != NULL ? (const char *)payload : "");
    char *cursor;

    if (copy == NULL)
        return "no-memory";
    cursor = copy;
    while (*cursor != '\0')
    {
        char *name = cursor;
        ULONG index;
        int declared = 0;

        while (*cursor != '\0' && *cursor != ',')
            ++cursor;
        if (*cursor == ',')
            *cursor++ = '\0';
        for (index = 0; index < electron_declared_scheme_count; ++index)
            if (strcmp(electron_declared_schemes[index], name) == 0)
                declared = 1;
        if (!declared)
            bug("[ELECTRON-PROTOCOL] scheme '%s' registered as privileged "
                "but not declared at CEF init: pass --standard-schemes=%s "
                "(and --secure-schemes/--cors-schemes/--fetch-schemes as "
                "needed) to the shell\n", name, name);
    }
    FreeVec(copy);
    return "checked";
}

/* ---- renderer extension + main<->renderer execution ------------------- */

static int CEF_CALLBACK electron_renderer_execute(cef_v8handler_t *self,
    const cef_string_t *name, cef_v8value_t *object, size_t argument_count,
    cef_v8value_t *const *arguments, cef_v8value_t **retval,
    cef_string_t *exception)
{
    (void)self;
    (void)name;
    (void)object;
    (void)retval;
    (void)exception;
    if (argument_count >= 1 && arguments[0]->is_string(arguments[0]))
    {
        char *text = electron_userfree_to_utf8(
            arguments[0]->get_string_value(arguments[0]));
        char id_text[16];
        const char *parts[4];

        if (text != NULL)
        {
            *electron_append_long(id_text, (LONG)electron_window_id) = '\0';
            parts[0] = "ipc\t";
            parts[1] = id_text;
            parts[2] = "\t";
            parts[3] = text;
            electron_post_event(electron_concat(parts, 4));
            FreeVec(text);
        }
    }
    return 1;
}

static LONG electron_register_renderer_extension(void)
{
    cef_string_t name;
    cef_string_t code;

    memset(&electron_renderer_handler, 0, sizeof(electron_renderer_handler));
    electron_renderer_handler.base.size = sizeof(electron_renderer_handler);
    electron_renderer_handler.base.add_ref = electron_static_add_ref;
    electron_renderer_handler.base.release = electron_static_release;
    electron_renderer_handler.base.has_one_ref = electron_static_has_ref;
    electron_renderer_handler.base.has_at_least_one_ref =
        electron_static_has_ref;
    electron_renderer_handler.execute = electron_renderer_execute;
    electron_set_cef_string(&name, electron_renderer_extension_name,
        sizeof(electron_renderer_extension_name) / sizeof(cef_char_t) - 1);
    electron_set_cef_string(&code, electron_renderer_extension_code,
        sizeof(electron_renderer_extension_code) / sizeof(cef_char_t) - 1);
    return CEFRegisterV8Extension(&name, &code, &electron_renderer_handler);
}

static void electron_set_active_browser(cef_browser_t *browser)
{
    cef_browser_t *previous = electron_active_browser;

    if (browser != NULL)
        browser->base.add_ref(&browser->base);
    electron_active_browser = browser;
    if (previous != NULL)
        previous->base.release(&previous->base);
}

static CONST_STRPTR electron_execute_in_renderer(const char *code_utf8,
    const cef_char_t *url_text, size_t url_length)
{
    cef_browser_t *browser = electron_active_browser;
    cef_frame_t *frame;
    cef_string_t code;
    cef_string_t url;
    cef_char_t *text;
    size_t length;

    if (browser == NULL)
        return "no-browser";
    frame = browser->get_main_frame(browser);
    if (frame == NULL)
        return "no-frame";
    text = electron_utf8_to_utf16(code_utf8, &length);
    if (text == NULL)
    {
        frame->base.release(&frame->base);
        return "no-memory";
    }
    electron_set_cef_string(&code, text, length);
    electron_set_cef_string(&url, url_text, url_length);
    frame->execute_java_script(frame, &code, &url, 0);
    frame->base.release(&frame->base);
    FreeVec(text);
    return "executed";
}

static CONST_STRPTR electron_deliver_to_renderer(CONST_STRPTR literal)
{
    const char *parts[3];
    char *code;
    CONST_STRPTR result;

    parts[0] = "AROSElectronRenderer.deliver(";
    parts[1] = literal != NULL ? (const char *)literal : "\"\"";
    parts[2] = ")";
    code = electron_concat(parts, 3);
    if (code == NULL)
        return "no-memory";
    result = electron_execute_in_renderer(code, electron_renderer_deliver_url,
        sizeof(electron_renderer_deliver_url) / sizeof(cef_char_t) - 1);
    FreeVec(code);
    return result;
}

static CONST_STRPTR electron_set_zoom_level(CONST_STRPTR payload)
{
    cef_browser_t *browser = electron_active_browser;
    cef_browser_host_t *host;
    const char *level = payload != NULL ? (const char *)payload : "";

    while (*level != '\0' && *level != '\t')
        ++level;
    if (*level == '\t')
        ++level;
    if (browser == NULL)
        return "no-browser";
    host = browser->get_host(browser);
    if (host == NULL)
        return "no-host";
    host->set_zoom_level(host, strtod(level, NULL));
    host->base.release(&host->base);
    return "zoomed";
}

/* `browser-window.create` payload: id \t preload \t additional args... */
static CONST_STRPTR electron_prepare_window(CONST_STRPTR payload)
{
    const char *cursor = payload != NULL ? (const char *)payload : "";
    const char *preload_start;
    size_t preload_length = 0;
    BPTR file;
    LONG length;

    electron_window_id = (ULONG)electron_parse_long(cursor);
    while (*cursor != '\0' && *cursor != '\t')
        ++cursor;
    if (*cursor == '\t')
        ++cursor;
    preload_start = cursor;
    while (*cursor != '\0' && *cursor != '\t')
    {
        ++cursor;
        ++preload_length;
    }

    FreeVec(electron_window_preload_path);
    FreeVec(electron_window_preload_source);
    electron_window_preload_path = NULL;
    electron_window_preload_source = NULL;
    if (preload_length != 0)
    {
        electron_window_preload_path = AllocVec(preload_length + 1, MEMF_ANY);
        if (electron_window_preload_path != NULL)
        {
            CopyMem((APTR)preload_start, electron_window_preload_path,
                preload_length);
            electron_window_preload_path[preload_length] = '\0';
            file = Open(electron_window_preload_path, MODE_OLDFILE);
            if (file != BNULL)
            {
                Seek(file, 0, OFFSET_END);
                length = Seek(file, 0, OFFSET_BEGINNING);
                if (length >= 0)
                {
                    electron_window_preload_source =
                        AllocVec((ULONG)length + 1, MEMF_ANY);
                    if (electron_window_preload_source != NULL)
                    {
                        LONG got = Read(file, electron_window_preload_source,
                            length);

                        if (got < 0)
                            got = 0;
                        electron_window_preload_source[got] = '\0';
                    }
                }
                Close(file);
            }
            bug("[ELECTRON-RENDERER] window %lu preload '%s' %s (%ld bytes)\n",
                (unsigned long)electron_window_id,
                electron_window_preload_path,
                electron_window_preload_source != NULL ? "loaded" :
                "NOT readable",
                electron_window_preload_source != NULL ?
                (long)strlen(electron_window_preload_source) : 0L);
        }
    }
    return electron_queue_request("browser-window.create", payload) == 0 ?
        "queued" : "queue-full";
}

static CONST_STRPTR electron_store_process_snapshot(CONST_STRPTR payload)
{
    FreeVec(electron_window_process_snapshot);
    electron_window_process_snapshot =
        electron_strdup(payload != NULL ? (const char *)payload : "{}");
    return electron_window_process_snapshot != NULL ? "stored" : "no-memory";
}

static char *electron_json_quote(const char *text)
{
    size_t length = strlen(text);
    char *result = AllocVec(length * 2 + 3, MEMF_ANY);
    char *write;

    if (result == NULL)
        return NULL;
    write = result;
    *write++ = '"';
    while (*text != '\0')
    {
        if (*text == '"' || *text == '\\')
            *write++ = '\\';
        *write++ = *text++;
    }
    *write++ = '"';
    *write = '\0';
    return result;
}

static LONG electron_inject_renderer_context(cef_browser_t *browser,
    cef_frame_t *frame, cef_v8context_t *context)
{
    const char *parts[8];
    char *quoted_path;
    char *script;
    cef_char_t *text;
    size_t length;
    cef_string_t code;
    cef_string_t url;
    cef_v8value_t *retval = NULL;
    cef_v8exception_t *exception = NULL;
    int rc;

    (void)browser;
    if (frame == NULL || context == NULL || !frame->is_main(frame))
        return 0;
    if (electron_window_preload_source == NULL)
    {
        bug("[ELECTRON-RENDERER] main frame context created, no preload to "
            "inject\n");
        return 0;
    }
    quoted_path = electron_json_quote(electron_window_preload_path != NULL ?
        electron_window_preload_path : "");
    if (quoted_path == NULL)
        return 20;
    parts[0] = electron_renderer_runtime_head;
    parts[1] = electron_window_process_snapshot != NULL ?
        electron_window_process_snapshot : "{}";
    parts[2] = electron_renderer_runtime_middle;
    parts[3] = quoted_path;
    parts[4] = electron_renderer_runtime_body;
    parts[5] = electron_window_preload_source;
    parts[6] = electron_renderer_runtime_tail;
    script = electron_concat(parts, 7);
    FreeVec(quoted_path);
    if (script == NULL)
        return 20;
    text = electron_utf8_to_utf16(script, &length);
    FreeVec(script);
    if (text == NULL)
        return 20;
    electron_set_cef_string(&code, text, length);
    electron_set_cef_string(&url, electron_renderer_script_url,
        sizeof(electron_renderer_script_url) / sizeof(cef_char_t) - 1);

    context->enter(context);
    rc = context->eval(context, &code, &url, 1, &retval, &exception);
    if (exception != NULL)
    {
        char *message = electron_userfree_to_utf8(
            exception->get_message(exception));

        bug("[ELECTRON-RENDERER] preload injection threw at line %ld: %s\n",
            (long)exception->get_line_number(exception),
            message != NULL ? message : "?");
        FreeVec(message);
        exception->base.release(&exception->base);
    }
    if (retval != NULL)
        retval->base.release(&retval->base);
    context->exit(context);
    FreeVec(text);
    bug("[ELECTRON-RENDERER] preload injected into main frame rc=%ld "
        "(%lu chars)\n", (long)rc, (unsigned long)length);
    return rc != 0 ? 0 : 20;
}
