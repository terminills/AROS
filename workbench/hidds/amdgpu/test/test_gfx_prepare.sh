#!/bin/sh
set -eu
test_src_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d)
trap 'rm -f -- "$test_build_dir/gfx_prepare.inc" "$test_build_dir/test"; rmdir -- "$test_build_dir"' EXIT HUP INT TERM
awk '/^void amdgpuGfxVRAMHandoff\(/ { copy=1 }
     copy {print}
     copy && /^}/ {exit}' \
    "$test_src_dir/../amdgpu_gfx_support.c" > "$test_build_dir/gfx_prepare.inc"
test -s "$test_build_dir/gfx_prepare.inc"
${CC:-cc} ${CFLAGS:-} -std=c11 -Wall -Wextra -Werror -O2 -I"$test_build_dir" \
    "$test_src_dir/test_gfx_prepare.c" -o "$test_build_dir/test"
"$test_build_dir/test"
