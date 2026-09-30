#!/bin/sh
set -eu
test_src_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d)
trap 'rm -f -- "$test_build_dir/test"; rmdir -- "$test_build_dir"' EXIT HUP INT TERM
if [ "$#" -eq 1 ]; then
    ${CC:-cc} -std=c11 -Wall -Wextra -Werror -DVERIFY_LINUX_GFX12 -I"$1" \
        "$test_src_dir/test_gfx_state_regs.c" -o "$test_build_dir/test"
else
    ${CC:-cc} -std=c11 -Wall -Wextra -Werror \
        "$test_src_dir/test_gfx_state_regs.c" -o "$test_build_dir/test"
fi
"$test_build_dir/test"
