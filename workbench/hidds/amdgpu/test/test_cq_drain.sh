#!/bin/sh
set -eu
test_src_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d)
trap 'rm -f -- "$test_build_dir/cq_drain.inc" "$test_build_dir/test"; rmdir -- "$test_build_dir"' EXIT HUP INT TERM
awk '/^#define CQ_DRAIN_POLLS / { copy=1 } /^\/\* End compute queue drain helper/ { copy=0 } copy' \
    "$test_src_dir/../amdgpu_gfx.c" > "$test_build_dir/cq_drain.inc"
test -s "$test_build_dir/cq_drain.inc"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 -I"$test_build_dir" \
    "$test_src_dir/test_cq_drain.c" -o "$test_build_dir/test"
"$test_build_dir/test"
