#!/bin/sh
set -eu
test_src_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d)
trap 'rm -f -- "$test_build_dir/viewport.inc" "$test_build_dir/test"; rmdir -- "$test_build_dir"' EXIT HUP INT TERM
test_source=${VIEWPORT_SOURCE:-$test_src_dir/../amdgpu_modeset.c}
awk '/^static const ULONG hubp_(viewport_start|viewport_dim|surface_pitch)_dw/ {print}
     /^BOOL amdgpu_modeset_set_viewport\(/ {copy=1}
     /^\/\*/ {copy=0}
     copy' "$test_source" > "$test_build_dir/viewport.inc"
test -s "$test_build_dir/viewport.inc"
${CC:-cc} ${CFLAGS:-} -std=c11 -Wall -Wextra -Werror -O2 -I"$test_build_dir" \
    "$test_src_dir/test_viewport.c" -o "$test_build_dir/test"
"$test_build_dir/test"
