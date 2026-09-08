/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    linuxso -- load a Linux x86-64 ELF shared object (ET_DYN) into an AROS
    process and run it against a glibc-ABI shim.

    The loader maps the PT_LOAD segments into exec memory, applies the
    dynamic relocations, resolves every undefined symbol against the shim
    table (glibc/libpthread/libm/libdl/libgcc_s entry points implemented
    on top of stdc/posixc/pthread), registers the object's .eh_frame with
    the program's libgcc unwinder and runs the ELF constructors.  It is a
    linklib, not a library: the shim must reach posixc/pthread through the
    calling program's own bases from any thread the guest object creates,
    which is exactly what a statically linked linklib gets for free.

    Raw `syscall` instructions in the guest are trapped as #UD on native
    x86-64 (IA32_EFER.SCE is off) and emulated by linuxso_syscall(); on
    hosted AROS they reach the host kernel unchanged.
*/

#ifndef LINUXSO_H
#define LINUXSO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct LinuxSO;

/*
 * Load `path` (an AROS or unix-style path, opened through posixc).  On
 * failure NULL is returned and, if errbuf is non-NULL, a one-line reason is
 * copied into it (unresolved symbols are listed).
 */
struct LinuxSO *linuxso_open(const char *path, char *errbuf, size_t errlen);

/* Look up an exported (defined, global or weak) symbol; NULL if absent. */
void *linuxso_sym(struct LinuxSO *so, const char *name);

/* Run the destructors, unregister the unwind info and free the mapping. */
int linuxso_close(struct LinuxSO *so);

/*
 * Debug channel.  Set the LINUXSO_DEBUG environment variable (any value)
 * before linuxso_open() to get loader, shim and syscall-emulation traces on
 * the kernel debug output (serial on native, stderr on hosted).
 */
int linuxso_debug_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* LINUXSO_H */
