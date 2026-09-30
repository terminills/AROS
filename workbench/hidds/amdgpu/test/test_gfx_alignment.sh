#!/bin/sh
set -eu
test_src_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d)
trap 'rm -f -- "$test_build_dir/helpers.inc" "$test_build_dir/test"; rmdir -- "$test_build_dir"' EXIT HUP INT TERM
# Exercise the actual driver helpers, not a separately maintained arithmetic model.
awk '/^#define GFX_DMA_ALIGN / { copy=1 } /^\/\* ---- GC BASE_IDX-1 / { copy=0 } copy' \
    "$test_src_dir/../amdgpu_gfx.c" > "$test_build_dir/helpers.inc"
test -s "$test_build_dir/helpers.inc"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 -I"$test_build_dir" \
    "$test_src_dir/test_gfx_alignment.c" -o "$test_build_dir/test"
"$test_build_dir/test"
