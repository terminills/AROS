#!/bin/sh
set -eu
test_src_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d)
trap 'rm -f -- "$test_build_dir/gfx_repeat.inc" "$test_build_dir/test"; rmdir -- "$test_build_dir"' EXIT HUP INT TERM
test_linux_include=${1:?usage: test_gfx_repeat.sh /path/to/linux/drivers/gpu/drm/amd/include}
awk '/^\/\* Begin testable graphics reuse helper/ { copy=1 } /^\/\* End testable graphics reuse helper/ { copy=0 } copy' \
    "$test_src_dir/../amdgpu_gfx.c" > "$test_build_dir/gfx_repeat.inc"
test -s "$test_build_dir/gfx_repeat.inc"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 -I"$test_build_dir" \
    -I"$test_linux_include" "$test_src_dir/test_gfx_repeat.c" -o "$test_build_dir/test"
"$test_build_dir/test"
