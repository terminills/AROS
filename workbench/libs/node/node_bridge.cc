/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: node.library bridge - Node 22 embedder entry points and library
          lifecycle glue (JOB-0092)
*/

#include <aros/libcall.h>
#include <aros/symbolsets.h>
#include <clib/exec_protos.h>
#include <exec/libraries.h>
#include <libraries/node.h>
#include <proto/node-v8.h>
#include <proto/v8.h>

#include <cstdlib>
#include <cstring>

#include <node.h>
#include <node_version.h>
#include <uv.h>

#include "node_libdefs.h"

/*
 * Diagnostics.
 *
 * kprintf() lives in the kernel's debug output path and pulls in
 * _arosdebuglock, which no linklib in a non-debug tree defines - node.library
 * then fails to link with a single undefined symbol after everything else has
 * built. Route the bridge's diagnostics through NodeBridgeLog() instead, which
 * is a no-op unless the library is built with -DNODE_AROS_BRIDGE_DEBUG.
 *
 * These are error and lifecycle messages rather than hot-path logging, so
 * turning them on costs nothing but a debug-capable tree.
 */
#ifdef NODE_AROS_BRIDGE_DEBUG
extern "C" int kprintf(const char *format, ...);
#define NodeBridgeLog(...) kprintf(__VA_ARGS__)
#else
#define NodeBridgeLog(...) ((void)0)
#endif
extern "C" int getentropy(void *buffer, size_t length);

/*
 * OpenSSL probes getentropy through a weak reference.  A weak-only undefined
 * symbol does not pull the AROS PosixC linklib wrapper from its archive, while
 * the ELF loader still has to relocate the call site.  Keep one strong
 * reference so bundled OpenSSL resolves to the shared PosixC implementation.
 */
static auto *const NodeGetEntropyAnchor __attribute__((used)) = &getentropy;

/*
 * Expunge runs on whichever task asked exec for memory when lddemon flushed
 * the library list - after C:Node has exited that is typically a Chromium
 * thread.  node's static destructors (node::Mutex and friends) call into
 * uv1.library, a pertaskbase library whose internal base lookups go through
 * the *current* task's storage slot, not the base the caller jumped through.
 * A task that never opened uv1 (and whose parent did not either) has no slot,
 * so uv_mutex_destroy() faulted in uv1's own memset relwrapper with
 * CR2 = the StdCBase offset inside the missing base (JOB-0092, 2026-09-14).
 *
 * The expunge cannot give itself a base either: lddemon's LDFlush holds
 * exec's LibList WRITE lock across RemLibrary()/Expunge, and
 * OpenLibrary("uv1.library") takes the same lock in READ mode - on SMP the
 * expunging task then spins forever in KrnSpinLock on its own lock (the
 * pc-x86_64 guest froze that way on the first Chromium launch after C:Node
 * had run, 2026-09-14: CPU0 in KrnSpinLock, s_Owner == the Chromium
 * process, TDNestCnt 2).  Anything Expunge does must hold no exec list
 * lock and must not load.
 *
 * Worse than the base: the expunge SLEEPS.  The pthread emulation linked
 * into node.library registers __pthread_Exit_Func in the EXIT set, which
 * reaps every thread it still knows about with Delay(1)/pthread_join - a
 * Wait() under LDFlush's LibList write lock.  The next OpenLibrary on any
 * task then spins in LDRequestObject's ObtainSystemLock(LibList, READ,
 * LOCKF_FORBID); with the sleeper READY but pinned to the same CPU as the
 * spinner (both Chromium threads on CPU0), it never runs again and the
 * whole guest stops taking input (rtest6, 2026-09-14, logs/rtest6-
 * deadlock.md).  Checking "does this task have a uv1 base" was not enough:
 * the flushing renderer thread DID have node's uv1 dup base in its slot.
 *
 * So the expunge is only allowed from node.library's own last closer.
 * genmodule's CloseLib decrements lib_OpenCnt, runs the CLOSELIB set, and
 * only then expunges when the count is 0 and LIBF_DELEXP is set - so the
 * CLOSELIB hook sees exactly the close that is about to expunge and
 * records that task's exec unique ID (task pointers are recycled; C:Node's
 * was already a Chromium thread by the time of the flush).  The EXPUNGELIB
 * hook passes only for that task, and lddemon's CloseLibrary path holds no
 * list lock (Forbid only), so the sleep in __pthread_Exit_Func is harmless
 * there.  Any other caller - LDFlush on whatever task's AllocMem failed -
 * is vetoed: LIBF_DELEXP stays set and the library stays resident until a
 * node process closes it, which is the only outcome that is safe from a
 * foreign task under the list lock.
 */
extern "C" int uv_aros_task_has_base(void);

static ULONG NodeCloserID;

static int NodeCloseLast(LIBBASETYPEPTR lh)
{
  if (lh->lib_OpenCnt == 0 && (lh->lib_Flags & LIBF_DELEXP))
    NodeCloserID = GetETaskID(FindTask(NULL));
  return 1;
}

static int NodeExpungePrepare(LIBBASETYPEPTR lh)
{
  (void)lh;
  ULONG closer = NodeCloserID;
  NodeCloserID = 0;
  return closer != 0 && closer == GetETaskID(FindTask(NULL)) &&
         uv_aros_task_has_base();
}

ADD2CLOSELIB(NodeCloseLast, 0)
ADD2EXPUNGELIB(NodeExpungePrepare, 0)

static std::shared_ptr<node::InitializationResult> NodeEmbedInitialization;
static node::IsolateData *NodeEmbedIsolateData;
static node::Environment *NodeEmbedEnvironment;
static uv_loop_t *NodeEmbedLoop;
static bool NodeEmbedLoopInitialized;
static v8::Isolate *NodeEmbedIsolate;
static v8::Global<v8::Context> *NodeEmbedContext;
static node::ArrayBufferAllocator *NodeEmbedArrayBufferAllocator;
static std::unique_ptr<node::MultiIsolatePlatform> NodeEmbedPlatform;
static LONG NodeEmbedConfiguredWorkerThreads;
static bool NodeEmbedOwnsIsolate;
/* Node 22's highest reserved ContextEmbedderIndex (NODE_CONTEXT_TAG). */
static constexpr int NodeContextTagEmbedderIndex = 39;
static constexpr size_t NodeLinkedBindingCapacity = 8;
static constexpr size_t NodeLinkedBindingNameCapacity = 64;

using NodeV8InitializePlatformFn = void (*)(v8::Platform *);
using NodeV8InitializeCppgcFn = void (*)(v8::PageAllocator *, size_t);
using NodeV8GetCurrentPlatformFn = v8::Platform *(*)();
using NodeV8SetCurrentPlatformFn = void (*)(v8::Platform *);
using NodeV8TryCatchStackTraceFn = v8::MaybeLocal<v8::Value> (*)(
    const v8::TryCatch *, v8::Local<v8::Context>);

static NodeV8GetCurrentPlatformFn NodeV8GetCurrentPlatform;
static NodeV8SetCurrentPlatformFn NodeV8SetCurrentPlatform;

class NodeV8PlatformScope {
 public:
  explicit NodeV8PlatformScope(v8::Platform *platform)
      : previous_(nullptr), active_(false) {
    if (NodeV8GetCurrentPlatform == nullptr ||
        NodeV8SetCurrentPlatform == nullptr)
      return;
    previous_ = NodeV8GetCurrentPlatform();
    if (platform != nullptr)
      NodeV8SetCurrentPlatform(platform);
    active_ = true;
  }

  ~NodeV8PlatformScope() {
    if (active_)
      NodeV8SetCurrentPlatform(previous_);
  }

  NodeV8PlatformScope(const NodeV8PlatformScope &) = delete;
  NodeV8PlatformScope &operator=(const NodeV8PlatformScope &) = delete;

 private:
  v8::Platform *previous_;
  bool active_;
};

struct NodePendingLinkedBinding {
  char name[NodeLinkedBindingNameCapacity];
  NodeEmbedderLinkedBindingInvoke invoke;
  void *private_data;
};

static NodePendingLinkedBinding
    NodePendingLinkedBindings[NodeLinkedBindingCapacity];
static size_t NodePendingLinkedBindingCount;

static void NodeInvokeLinkedBinding(
    const v8::FunctionCallbackInfo<v8::Value>& arguments)
{
  if (arguments.Data().IsEmpty() || !arguments.Data()->IsExternal())
    return;

  auto *binding = static_cast<NodePendingLinkedBinding *>(
      v8::Local<v8::External>::Cast(arguments.Data())->Value());
  if (binding == nullptr || binding->invoke == nullptr)
    return;

  v8::Isolate *isolate = arguments.GetIsolate();
  v8::String::Utf8Value method(
      isolate, arguments.Length() > 0 ? arguments[0] : v8::Local<v8::Value>());
  v8::String::Utf8Value payload(
      isolate, arguments.Length() > 1 ? arguments[1] : v8::Local<v8::Value>());
  CONST_STRPTR result = binding->invoke(
      *method != nullptr ? *method : "",
      *payload != nullptr ? *payload : "",
      binding->private_data);
  if (result == nullptr)
    result = "";

  v8::Local<v8::String> value;
  if (v8::String::NewFromUtf8(
          isolate,
          reinterpret_cast<const char *>(result),
          v8::NewStringType::kNormal).ToLocal(&value))
    arguments.GetReturnValue().Set(value);
}

static void NodeInitializeLinkedBinding(v8::Local<v8::Object> exports,
                                        v8::Local<v8::Value> module,
                                        v8::Local<v8::Context> context,
                                        void *private_data)
{
  auto *binding = static_cast<NodePendingLinkedBinding *>(private_data);
  v8::Isolate *isolate = context->GetIsolate();
  v8::Local<v8::String> name;
  v8::Local<v8::Function> function;

  (void)module;
  if (!v8::String::NewFromUtf8(
          isolate, "invoke", v8::NewStringType::kNormal).ToLocal(&name))
    return;
  if (!v8::Function::New(
          context,
          NodeInvokeLinkedBinding,
          v8::External::New(isolate, binding)).ToLocal(&function))
    return;
  v8::Maybe<bool> set_result = exports->Set(context, name, function);
  (void)set_result;
}

static LONG NodeEnsureEmbedLoop()
{
  if (NodeEmbedLoopInitialized)
    return 0;

  NodeEmbedLoop =
      static_cast<uv_loop_t *>(std::calloc(1, uv_loop_size()));
  if (NodeEmbedLoop == nullptr)
    return UV_ENOMEM;

  int uv_rc = uv_loop_init(NodeEmbedLoop);
  if (uv_rc != 0)
  {
    std::free(NodeEmbedLoop);
    NodeEmbedLoop = nullptr;
    return uv_rc;
  }
  NodeEmbedLoopInitialized = true;
  return 0;
}

static void NodeCleanupFailedAttach()
{
  if (NodeEmbedEnvironment != nullptr) {
    node::FreeEnvironment(NodeEmbedEnvironment);
    NodeEmbedEnvironment = nullptr;
  }
  if (NodeEmbedIsolateData != nullptr) {
    node::FreeIsolateData(NodeEmbedIsolateData);
    NodeEmbedIsolateData = nullptr;
  }
  if (NodeEmbedLoopInitialized) {
    int loop_rc = uv_loop_close(NodeEmbedLoop);
    if (loop_rc == 0) {
      std::free(NodeEmbedLoop);
      NodeEmbedLoop = nullptr;
      NodeEmbedLoopInitialized = false;
    } else {
      NodeBridgeLog("[NodeEmbed] failed-attach uv_loop_close rc=%d\n", loop_rc);
    }
  }
}

static LONG NodeAttachV8Context(v8::Isolate *isolate,
                                v8::Local<v8::Context> context,
                                CONST_STRPTR script,
                                bool defer_failed_cleanup)
{
  LONG loop_rc = NodeEnsureEmbedLoop();
  if (loop_rc != 0)
    return loop_rc;

  NodeEmbedIsolateData =
      node::CreateIsolateData(
          isolate, NodeEmbedLoop, NodeEmbedPlatform.get(),
          NodeEmbedArrayBufferAllocator);
  if (NodeEmbedIsolateData == nullptr) {
    uv_loop_close(NodeEmbedLoop);
    std::free(NodeEmbedLoop);
    NodeEmbedLoop = nullptr;
    NodeEmbedLoopInitialized = false;
    return 20;
  }

  const uint64_t environment_flags =
      node::EnvironmentFlags::kDefaultFlags |
      node::EnvironmentFlags::kNoRegisterESMLoader |
      node::EnvironmentFlags::kNoNativeAddons |
      node::EnvironmentFlags::kNoCreateInspector |
      node::EnvironmentFlags::kNoStartDebugSignalHandler |
      node::EnvironmentFlags::kNoWaitForInspectorFrontend;

  NodeEmbedEnvironment = node::CreateEnvironment(
      NodeEmbedIsolateData,
      context,
      NodeEmbedInitialization->args(),
      NodeEmbedInitialization->exec_args(),
      static_cast<node::EnvironmentFlags::Flags>(environment_flags));
  if (NodeEmbedEnvironment == nullptr)
    goto fail;

  for (size_t index = 0; index < NodePendingLinkedBindingCount; ++index) {
    NodePendingLinkedBinding& binding = NodePendingLinkedBindings[index];
    node::AddLinkedBinding(NodeEmbedEnvironment,
                           binding.name,
                           NodeInitializeLinkedBinding,
                           &binding);
  }

  if (script != nullptr) {
    /*
     * This context shares Chromium's isolate but is deliberately not a Blink
     * context.  Never let a startup exception reach Blink's isolate-global
     * message listener: it expects its ScriptState embedder slot and treats
     * an isolated Node context as a security invariant violation.
     */
    v8::TryCatch try_catch(isolate);
    if (node::LoadEnvironment(
            NodeEmbedEnvironment, reinterpret_cast<const char *>(script))
            .IsEmpty()) {
      v8::String::Utf8Value exception(isolate, try_catch.Exception());
      NodeBridgeLog("[NodeEmbed] startup exception: %s\n",
              *exception != nullptr ? *exception : "<unavailable>");
      auto stack_trace_fn = reinterpret_cast<NodeV8TryCatchStackTraceFn>(
          NodeV8ResolveEngineSymbol(
              "_ZNK2v88TryCatch10StackTraceENS_5LocalINS_7ContextEEE"));
      v8::Local<v8::Value> stack_trace;
      if (stack_trace_fn != nullptr &&
          stack_trace_fn(&try_catch, context).ToLocal(&stack_trace)) {
        v8::String::Utf8Value stack(isolate, stack_trace);
        NodeBridgeLog("[NodeEmbed] startup stack: %s\n",
                *stack != nullptr ? *stack : "<unavailable>");
      }
      goto fail;
    }
  }

  NodeEmbedIsolate = isolate;
  NodeEmbedContext = new v8::Global<v8::Context>(isolate, context);
  NodeBridgeLog("[NodeEmbed] attached isolate=%p context=%p env=%p\n",
          isolate,
          *context,
          NodeEmbedEnvironment);
  return 0;

fail:
  if (!defer_failed_cleanup)
    NodeCleanupFailedAttach();
  return 20;
}

/*
 * Bundled c-ares uses the classic AROS SocketBase symbol.  Keep that base
 * owned by node.library instead of pulling in the bsdsocket link library:
 * the latter makes loading Node fail before AROSTCP has started.
 */
extern "C" {
struct Library *SocketBase = nullptr;
}

static void NodeTraceOpenUvHandle(uv_handle_t *handle, void *arg)
{
  (void)arg;
  uv_handle_type type = handle->type;

  NodeBridgeLog("[NodeLibrary] uv handle=%p type=%d active=%d closing=%d\n",
          handle,
          static_cast<int>(type),
          uv_is_active(handle),
          uv_is_closing(handle));
}

extern "C" {

AROS_LH2(LONG, NodeStart,
    AROS_LHA(LONG, argc, D0),
    AROS_LHA(STRPTR *, argv, A0),
    LIBBASETYPEPTR, LIBBASE, 5, Node)
{
  AROS_LIBFUNC_INIT
  (void)LIBBASE;
  bool close_socket_base = false;

  if (SocketBase == nullptr) {
    SocketBase = OpenLibrary("bsdsocket.library", 4);
    close_socket_base = SocketBase != nullptr;
  }

  int rc = node::Start(
      static_cast<int>(argc), reinterpret_cast<char **>(argv));
  uv_loop_t *loop = uv_default_loop();

  uv_run(loop, UV_RUN_NOWAIT);
  int loop_rc = uv_loop_close(loop);

  if (loop_rc != 0) {
    NodeBridgeLog("[NodeLibrary] uv_loop_close failed rc=%d; open handles follow\n",
            loop_rc);
    uv_walk(loop, NodeTraceOpenUvHandle, nullptr);
  }
  if (rc == 0 && loop_rc != 0)
    rc = loop_rc;
  if (close_socket_base) {
    CloseLibrary(SocketBase);
    SocketBase = nullptr;
  }
  return static_cast<LONG>(rc);
  AROS_LIBFUNC_EXIT
}

AROS_LH0(CONST_STRPTR, NodeVersion,
    LIBBASETYPEPTR, LIBBASE, 6, Node)
{
  AROS_LIBFUNC_INIT
  (void)LIBBASE;
  return NODE_VERSION;
  AROS_LIBFUNC_EXIT
}

AROS_LH0(LONG, NodePrepareEmbedder,
    LIBBASETYPEPTR, LIBBASE, 7, Node)
{
  AROS_LIBFUNC_INIT
  (void)LIBBASE;

  if (NodeEmbedInitialization)
    return 0;

  const std::vector<std::string> args{"AROS-Electron"};
  NodeEmbedInitialization = node::InitializeOncePerProcess(
      args,
      {
          node::ProcessInitializationFlags::kNoDefaultSignalHandling,
          node::ProcessInitializationFlags::kNoStdioInitialization,
          node::ProcessInitializationFlags::kNoInitializeV8,
          node::ProcessInitializationFlags::kNoInitializeNodeV8Platform,
          node::ProcessInitializationFlags::kNoInitializeCppgc,
          node::ProcessInitializationFlags::kNoInitOpenSSL,
          node::ProcessInitializationFlags::kNoParseGlobalDebugVariables,
          node::ProcessInitializationFlags::kNoAdjustResourceLimits,
          node::ProcessInitializationFlags::kNoUseLargePages,
      });

  if (!NodeEmbedInitialization)
    return 20;
  for (const std::string& error : NodeEmbedInitialization->errors())
    NodeBridgeLog("[NodeEmbed] initialize: %s\n", error.c_str());
  if (NodeEmbedInitialization->early_return()) {
    LONG rc = NodeEmbedInitialization->exit_code();
    NodeEmbedInitialization.reset();
    return rc != 0 ? rc : 20;
  }

  NodeBridgeLog("[NodeEmbed] process state prepared without V8 ownership\n");
  return 0;
  AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, NodeAttachCurrentV8Context,
    AROS_LHA(CONST_STRPTR, script, A0),
    LIBBASETYPEPTR, LIBBASE, 8, Node)
{
  AROS_LIBFUNC_INIT
  (void)LIBBASE;

  if (!NodeEmbedInitialization || NodeEmbedEnvironment != nullptr)
    return 20;

  v8::Isolate *isolate = v8::Isolate::GetCurrent();
  if (isolate == nullptr)
    return 20;
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if (context.IsEmpty())
    return 20;

  return NodeAttachV8Context(isolate, context, script, false);
  AROS_LIBFUNC_EXIT
}

AROS_LH0(LONG, NodePumpEmbedder,
    LIBBASETYPEPTR, LIBBASE, 9, Node)
{
  AROS_LIBFUNC_INIT
  (void)LIBBASE;
  if (!NodeEmbedLoopInitialized || NodeEmbedEnvironment == nullptr ||
      NodeEmbedIsolate == nullptr || NodeEmbedContext == nullptr)
    return 20;

  v8::Isolate *isolate = NodeEmbedIsolate;
  NodeV8PlatformScope platform_scope(
      NodeEmbedOwnsIsolate ? NodeEmbedPlatform.get() : nullptr);
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context =
      v8::Local<v8::Context>::New(isolate, *NodeEmbedContext);
  v8::Context::Scope context_scope(context);

  /*
   * Blink keeps renderer isolates on kScoped, while Node's callback machinery
   * requires kExplicit.  Match Electron's UvRunOnce boundary: change policy
   * only while entering Node through uv_run, then restore the policy belonging
   * to the isolate owner.
   */
  isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
  const LONG result = uv_run(NodeEmbedLoop, UV_RUN_NOWAIT);
  if (NodeEmbedPlatform != nullptr)
    NodeEmbedPlatform->DrainTasks(isolate);
  context->GetMicrotaskQueue()->PerformCheckpoint(isolate);
  isolate->SetMicrotasksPolicy(NodeEmbedOwnsIsolate
      ? v8::MicrotasksPolicy::kExplicit
      : v8::MicrotasksPolicy::kScoped);
  return result;
  AROS_LIBFUNC_EXIT
}

AROS_LH0(void, NodeDetachEmbedder,
    LIBBASETYPEPTR, LIBBASE, 10, Node)
{
  AROS_LIBFUNC_INIT
  (void)LIBBASE;

  v8::Isolate *isolate = NodeEmbedIsolate;
  const bool owned_isolate = NodeEmbedOwnsIsolate;
  {
    NodeV8PlatformScope platform_scope(
        owned_isolate ? NodeEmbedPlatform.get() : nullptr);
    if (isolate != nullptr) {
      v8::Isolate::Scope isolate_scope(isolate);
      if (NodeEmbedEnvironment != nullptr) {
        node::Stop(NodeEmbedEnvironment,
                   node::StopFlags::kDoNotTerminateIsolate);
        node::FreeEnvironment(NodeEmbedEnvironment);
        NodeEmbedEnvironment = nullptr;
      }
      if (NodeEmbedContext != nullptr) {
        NodeEmbedContext->Reset();
        delete NodeEmbedContext;
        NodeEmbedContext = nullptr;
      }
    }
    if (NodeEmbedIsolateData != nullptr) {
      node::FreeIsolateData(NodeEmbedIsolateData);
      NodeEmbedIsolateData = nullptr;
    }
    if (owned_isolate && NodeEmbedPlatform && isolate != nullptr)
      NodeEmbedPlatform->UnregisterIsolate(isolate);
    if (NodeEmbedLoopInitialized) {
      int loop_rc = uv_loop_close(NodeEmbedLoop);
      if (loop_rc != 0)
        NodeBridgeLog("[NodeEmbed] uv_loop_close rc=%d\n", loop_rc);
      else {
        std::free(NodeEmbedLoop);
        NodeEmbedLoop = nullptr;
        NodeEmbedLoopInitialized = false;
      }
    }
    if (owned_isolate && isolate != nullptr) {
      isolate->Dispose();
      node::FreeArrayBufferAllocator(NodeEmbedArrayBufferAllocator);
    }
  }
  NodeEmbedPlatform.reset();
  if (NodeEmbedInitialization) {
    node::TearDownOncePerProcess();
    NodeEmbedInitialization.reset();
  }
  NodeEmbedArrayBufferAllocator = nullptr;
  NodeEmbedOwnsIsolate = false;
  NodeEmbedIsolate = nullptr;
  NodeBridgeLog("[NodeEmbed] detached environment owned_isolate=%d\n",
          owned_isolate ? 1 : 0);
  AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, NodeAttachIsolatedV8Context,
    AROS_LHA(CONST_STRPTR, script, A0),
    LIBBASETYPEPTR, LIBBASE, 11, Node)
{
  AROS_LIBFUNC_INIT
  (void)LIBBASE;

  if (!NodeEmbedInitialization || NodeEmbedEnvironment != nullptr)
    return 20;

  v8::Isolate *isolate = v8::Isolate::GetCurrent();
  if (isolate == nullptr)
    return 20;

  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = v8::Context::New(isolate);
  if (context.IsEmpty())
    return 20;

  /*
   * CreateEnvironment() grows the embedder-data array through Node's context
   * tag slot.  V8 fills newly-created lower slots with tagged `undefined`,
   * which Blink's isolate-global callbacks can mistake for a ScriptState
   * pointer in gin's slot.  Grow the array first, then initialize every slot
   * as an aligned null pointer before Node assigns its reserved fields.
  */
  context->SetAlignedPointerInEmbedderData(
      NodeContextTagEmbedderIndex, nullptr);
  const int inherited_fields = context->GetNumberOfEmbedderDataFields();
  for (int index = 0; index < inherited_fields; ++index)
    context->SetAlignedPointerInEmbedderData(index, nullptr);
  v8::Context::Scope context_scope(context);
  if (node::InitializeContext(context).IsNothing())
    return 20;

  return NodeAttachV8Context(isolate, context, script, false);
  AROS_LIBFUNC_EXIT
}

AROS_LH3(LONG, NodeRegisterEmbedderLinkedBinding,
    AROS_LHA(CONST_STRPTR, name, A0),
    AROS_LHA(APTR, invoke, A1),
    AROS_LHA(APTR, private_data, A2),
    LIBBASETYPEPTR, LIBBASE, 12, Node)
{
  AROS_LIBFUNC_INIT
  (void)LIBBASE;

  if (name == nullptr || name[0] == '\0' || invoke == nullptr ||
      NodeEmbedEnvironment != nullptr)
    return 20;

  for (size_t index = 0; index < NodePendingLinkedBindingCount; ++index) {
    NodePendingLinkedBinding& binding = NodePendingLinkedBindings[index];
    if (std::strcmp(binding.name, reinterpret_cast<const char *>(name)) == 0) {
      binding.invoke =
          reinterpret_cast<NodeEmbedderLinkedBindingInvoke>(invoke);
      binding.private_data = private_data;
      return 0;
    }
  }

  if (NodePendingLinkedBindingCount == NodeLinkedBindingCapacity ||
      std::strlen(reinterpret_cast<const char *>(name)) >=
          NodeLinkedBindingNameCapacity)
    return 20;

  NodePendingLinkedBinding& binding =
      NodePendingLinkedBindings[NodePendingLinkedBindingCount++];
  std::strcpy(binding.name, reinterpret_cast<const char *>(name));
  binding.invoke = reinterpret_cast<NodeEmbedderLinkedBindingInvoke>(invoke);
  binding.private_data = private_data;
  return 0;
  AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, NodeAttachOwnedV8Context,
    AROS_LHA(CONST_STRPTR, script, A0),
    LIBBASETYPEPTR, LIBBASE, 13, Node)
{
  AROS_LIBFUNC_INIT
  (void)LIBBASE;

  if (!NodeEmbedInitialization || NodeEmbedEnvironment != nullptr ||
      NodeEmbedIsolate != nullptr)
    return 20;

  NodeV8GetCurrentPlatform =
      reinterpret_cast<NodeV8GetCurrentPlatformFn>(
          NodeV8ResolveEngineSymbol("__aros_v8_get_current_platform"));
  NodeV8SetCurrentPlatform =
      reinterpret_cast<NodeV8SetCurrentPlatformFn>(
          NodeV8ResolveEngineSymbol("__aros_v8_set_current_platform"));
  if (NodeV8GetCurrentPlatform == nullptr ||
      NodeV8SetCurrentPlatform == nullptr)
    return 20;

  /*
   * V8's platform context is task-local on AROS. Keep Chromium's platform
   * installed on the browser/UI task outside Node entry points so renderer
   * Processes inherit the platform whose worker count configured their heap.
   */
  v8::Platform *browser_platform = NodeV8GetCurrentPlatform();
  NodeV8PlatformScope browser_platform_restore(nullptr);

  /*
   * The resident engine keeps V8 process initialization global while its AROS
   * platform pointer is task/Process-local. Browser and renderer Processes
   * therefore install their own scheduler platform without copying the
   * engine. An Electron main Process has no gin renderer to do that first,
   * so initialize the authoritative v8.library platform before allocating its
   * owned isolate.
   */
  if (V8Initialize() == nullptr) {
    NodeBridgeLog("[NodeEmbed] shared v8.library platform initialization failed\n");
    return 20;
  }

  /*
   * NodePlatform includes one delayed-task scheduler in its reported worker
   * count in addition to the requested pool. V8 caches this count globally in
   * GC paths, so match the Chromium platform already installed in this
   * Process. Otherwise the later renderer observes a different core count and
   * mark-compact aborts even though each task has the correct platform.
   */
  int node_pool_size = 4;
  if (NodeEmbedConfiguredWorkerThreads > 1) {
    node_pool_size =
        static_cast<int>(NodeEmbedConfiguredWorkerThreads) - 1;
  } else if (browser_platform != nullptr) {
    const int browser_workers = browser_platform->NumberOfWorkerThreads();
    if (browser_workers > 1)
      node_pool_size = browser_workers - 1;
  }
  NodeEmbedPlatform = node::MultiIsolatePlatform::Create(node_pool_size);
  if (!NodeEmbedPlatform)
    return 20;
  NodeBridgeLog("[NodeEmbed] platform workers configured=%ld browser=%d node=%d\n",
          NodeEmbedConfiguredWorkerThreads,
          browser_platform != nullptr
              ? browser_platform->NumberOfWorkerThreads() : -1,
          NodeEmbedPlatform->NumberOfWorkerThreads());
  auto initialize_platform = reinterpret_cast<NodeV8InitializePlatformFn>(
      NodeV8ResolveEngineSymbol(
          "_ZN2v82V818InitializePlatformEPNS_8PlatformE"));
  if (initialize_platform == nullptr) {
    NodeEmbedPlatform.reset();
    return 20;
  }
  initialize_platform(NodeEmbedPlatform.get());

  /*
   * Node's IsolateData creates a CppHeap for an owned main isolate. The
   * renderer normally initializes cppgc through gin/Blink, but this browser
   * Process runs first. AROS v8.library makes cppgc process initialization
   * resident and idempotent, so initialize that shared engine state here.
   */
  auto initialize_cppgc = reinterpret_cast<NodeV8InitializeCppgcFn>(
      NodeV8ResolveEngineSymbol(
          "_ZN5cppgc17InitializeProcessEPN2v813PageAllocatorEm"));
  if (initialize_cppgc == nullptr) {
    NodeEmbedPlatform.reset();
    return 20;
  }
  initialize_cppgc(nullptr, 0);

  node::ArrayBufferAllocator *allocator =
      node::CreateArrayBufferAllocator();
  if (allocator == nullptr) {
    NodeEmbedPlatform.reset();
    return 20;
  }

  LONG loop_rc = NodeEnsureEmbedLoop();
  if (loop_rc != 0) {
    node::FreeArrayBufferAllocator(allocator);
    NodeEmbedPlatform.reset();
    return loop_rc;
  }

  v8::Isolate *isolate =
      node::NewIsolate(allocator, NodeEmbedLoop, NodeEmbedPlatform.get());
  if (isolate == nullptr) {
    node::FreeArrayBufferAllocator(allocator);
    NodeEmbedPlatform.reset();
    return 20;
  }
  NodeEmbedArrayBufferAllocator = allocator;

  LONG result;
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    if (context.IsEmpty()) {
      result = 20;
    } else {
      v8::Context::Scope context_scope(context);
      if (node::InitializeContext(context).IsNothing())
        result = 20;
      else
        result = NodeAttachV8Context(isolate, context, script, true);
    }
  }

  if (result != 0) {
    NodeCleanupFailedAttach();
    NodeEmbedPlatform->UnregisterIsolate(isolate);
    isolate->Dispose();
    node::FreeArrayBufferAllocator(allocator);
    NodeEmbedArrayBufferAllocator = nullptr;
    NodeEmbedPlatform.reset();
    return result;
  }
  NodeEmbedOwnsIsolate = true;
  NodeBridgeLog("[NodeEmbed] owns dedicated embedder isolate=%p\n", isolate);
  return 0;
  AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, NodeConfigureOwnedV8WorkerThreads,
    AROS_LHA(LONG, worker_threads, D0),
    LIBBASETYPEPTR, LIBBASE, 14, Node)
{
  AROS_LIBFUNC_INIT
  (void)LIBBASE;

  if (worker_threads < 2 || NodeEmbedIsolate != nullptr ||
      NodeEmbedEnvironment != nullptr)
    return 20;
  NodeEmbedConfiguredWorkerThreads = worker_threads;
  return 0;
  AROS_LIBFUNC_EXIT
}

}  // extern "C"
