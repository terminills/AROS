#!/bin/sh
set -eu
test_src_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d)
trap 'rm -f -- "$test_build_dir/gfx_csb.inc" "$test_build_dir/test"; rmdir -- "$test_build_dir"' EXIT HUP INT TERM
test_linux_amd=${1:?usage: test_gfx_csb.sh /path/to/linux/drivers/gpu/drm/amd}
awk '/^\/\* Begin testable graphics CSIB helper/ { copy=1 } /^\/\* End testable graphics CSIB helper/ { copy=0 } copy' \
    "$test_src_dir/../amdgpu_gfx.c" > "$test_build_dir/gfx_csb.inc"
test -s "$test_build_dir/gfx_csb.inc"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -I"$test_build_dir" \
    -I"$test_linux_amd/include" -I"$test_linux_amd/amdgpu" \
    "$test_src_dir/test_gfx_csb.c" -o "$test_build_dir/test"
"$test_build_dir/test"
