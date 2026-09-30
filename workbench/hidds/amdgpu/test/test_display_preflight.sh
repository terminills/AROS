#!/bin/sh
set -eu
test_src_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d)
trap 'rm -f -- "$test_build_dir/display_switch.inc" "$test_build_dir/test"; rmdir -- "$test_build_dir"' EXIT HUP INT TERM
test_source=${DISPLAY_SWITCH_SOURCE:-$test_src_dir/../amdgpu_displayclass.c}
awk '/^static BOOL amdgpu_select_plane\(/ {copy=1}
     /^\/\*/ {copy=0}
     copy' "$test_source" > "$test_build_dir/display_switch.inc"
test -s "$test_build_dir/display_switch.inc"
${CC:-cc} ${CFLAGS:-} -std=c11 -Wall -Wextra -Werror -O2 -I"$test_build_dir" \
    "$test_src_dir/test_display_preflight.c" -o "$test_build_dir/test"
"$test_build_dir/test"
