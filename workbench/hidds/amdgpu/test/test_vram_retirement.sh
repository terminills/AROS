#!/bin/sh
set -eu
test_src_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d)
trap 'rm -f -- "$test_build_dir/pool_structure.inc" "$test_build_dir/pool_functions.inc" "$test_build_dir/test"; rmdir -- "$test_build_dir"' EXIT HUP INT TERM
awk '/^struct HWData$/ {copy=1} copy {print} copy && /^};/ {exit}' \
    "$test_src_dir/../amdgpu_gfx_support.h" > "$test_build_dir/pool_structure.inc"
awk '/^static BOOL amdgpu_vram_alloc_locked\(/ {copy=1}
     /^void amdgpuGfxVRAMHandoff\(/ {exit} copy {print}' \
    "$test_src_dir/../amdgpu_gfx_support.c" > "$test_build_dir/pool_functions.inc"
test -s "$test_build_dir/pool_structure.inc"
test -s "$test_build_dir/pool_functions.inc"
${CC:-cc} ${CFLAGS:-} -std=c11 -Wall -Wextra -Werror -O2 -I"$test_build_dir" \
    "$test_src_dir/test_vram_retirement.c" -o "$test_build_dir/test"
"$test_build_dir/test"
