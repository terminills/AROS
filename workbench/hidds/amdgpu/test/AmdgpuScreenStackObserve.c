/* Preserve the v1 public rendering sequence; only add fixed read-only samples
 * of the already-booted driver's actual scanout allocation at each hold.
 */
#define AMDGPU_STACK_SCANOUT_OBSERVER 1
#include "AmdgpuScreenStackProbe.c"
