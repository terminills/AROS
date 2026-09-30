#!/bin/sh
set -eu
test_src_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d)
trap 'rm -f -- "$test_build_dir/compositor_toggle.inc" "$test_build_dir/test"; rmdir -- "$test_build_dir"' EXIT HUP INT TERM
test_source=${COMPOSITOR_SOURCE:-$test_src_dir/../amdgpu_compositorclass.c}
awk '/^static (BOOL|VOID) HIDDCompositorToggleCompositing\(/ {copy=1}
     /^static VOID HIDDCompositorPurgeBitMapStack\(/ {copy=0}
     copy' "$test_source" > "$test_build_dir/compositor_toggle.inc"
test -s "$test_build_dir/compositor_toggle.inc"
${CC:-cc} ${CFLAGS:-} -std=c11 -Wall -Wextra -Werror -O2 -I"$test_build_dir" \
    "$test_src_dir/test_compositor_switch.c" -o "$test_build_dir/test"
"$test_build_dir/test"
