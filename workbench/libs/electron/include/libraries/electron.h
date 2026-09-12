#ifndef LIBRARIES_ELECTRON_H
#define LIBRARIES_ELECTRON_H

#include <exec/types.h>

#define ELECTRONNAME "electron.library"
#define ELECTRONVERSION 1

/*
 * Capabilities describe which external native runtimes this adapter resolved.
 * They are intentionally independent: later embedders must not assume that a
 * browser engine also implies an available Node environment, or vice versa.
 */
#define ELECTRON_CAP_CEF  (1UL << 0)
#define ELECTRON_CAP_NODE (1UL << 1)
#define ELECTRON_CAP_MODULE_SHIM (1UL << 2)
#define ELECTRON_CAP_BROWSER_WINDOW_REQUESTS (1UL << 3)

/*
 * The first lifecycle ABI deliberately preserves each engine's native opaque
 * structures.  electron.library sequences and adapts policy; it does not
 * clone CEF or Node data structures into a competing ABI.
 *
 * ElectronRunNode currently uses Node's command-line lifecycle and therefore
 * remains a one-shot call.  The later simultaneous embedder will be appended
 * without changing these proven vectors.
 */
LONG ElectronAttachNodeIsolatedV8Context(CONST_STRPTR script);
LONG ElectronAttachNodeIsolatedV8ContextWithModule(CONST_STRPTR script);
LONG ElectronPollNativeRequest(STRPTR method, ULONG method_size,
    STRPTR payload, ULONG payload_size);
LONG ElectronAttachNodeOwnedV8ContextWithModule(CONST_STRPTR script);

#endif /* LIBRARIES_ELECTRON_H */
