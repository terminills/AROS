#!/bin/sh
set -eu
test_src_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d)
trap 'rm -f -- "$test_build_dir/display_publish.inc" "$test_build_dir/test"; rmdir -- "$test_build_dir"' EXIT HUP INT TERM
awk '/^\/\* BEGIN AMDGPU display publication/ {copy=1}
     copy {print}
     /^\/\* END AMDGPU display publication/ {copy=0}
     /^(BOOL|VOID) AmdgpuGfxDisplay__Hidd_Display__SetCursor/ {method=1}
     method {print}
     method && /^}/ {method=0}' \
    "$test_src_dir/../amdgpu_displayclass.c" > "$test_build_dir/display_publish.inc"
test -s "$test_build_dir/display_publish.inc"
${CC:-cc} ${CFLAGS:-} -std=c11 -Wall -Wextra -Werror -O2 -I"$test_build_dir" \
    "$test_src_dir/test_display_publish.c" -o "$test_build_dir/test"
"$test_build_dir/test"
