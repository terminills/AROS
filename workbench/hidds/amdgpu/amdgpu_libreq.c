/*
 * amdgpu.hidd — optional-library overrides for early cold-start.
 *
 * amdgpu.hidd runs at residentpri 49 (before USB), which is BEFORE the drive
 * mounts and before dos.library / stdc.library / intuition.library exist. The
 * genmodule autoinit (set_open_libraries) opens the module's whole library set
 * and, by default, FAILS the whole InitResident if any base is NULL — so a
 * pre-mount cold-start dies before detection ever runs.
 *
 * The autoinit already supports OPTIONAL libraries: compiler/autoinit/libraries.c
 * treats a NEGATIVE version requirement as "open if present, never fatal"
 *   ( if (version < 0) { version = -(version + 1); do_not_fail = 1; } ).
 *
 * The per-library requirement lives in the weak symbol __aros_libreq_<Base>.
 * We override the three that are not up at cold-start with a strong NEGATIVE
 * value, so the module inits on oop.library alone (like gfx.hidd) and runs
 * detection / BAR map / IP discovery / framebuffer. dos (firmware load) and
 * intuition (modeset) then open lazily when their features run, after AROS
 * finishes booting; stdc is likewise tolerated until present.
 *
 * Encoding: effective_version = -(value + 1).  -1 => optional v0.
 * oop.library / exec stay MANDATORY (they ARE up at pri 49 and are required to
 * be a HIDD at all) — we do NOT override those.
 */

#include <exec/types.h>

const LONG __aros_libreq_DOSBase       = -1; /* optional, effective v0 */
const LONG __aros_libreq_StdCBase      = -1; /* optional, effective v0 */
const LONG __aros_libreq_IntuitionBase = -1; /* optional, effective v0 */
