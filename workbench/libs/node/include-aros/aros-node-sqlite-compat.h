/*
 * SQLite's amalgamation defines an internal GLOBAL(t, v) macro before it
 * includes pthread.h.  AROS exec/types.h also defines GLOBAL as a linkage
 * qualifier.  Load the guarded pthread declarations before the amalgamation,
 * then release the AROS qualifier so SQLite can define its own macro.
 */
#ifndef AROS_NODE_SQLITE_COMPAT_H
#define AROS_NODE_SQLITE_COMPAT_H

#include <pthread.h>
#undef GLOBAL

#endif
