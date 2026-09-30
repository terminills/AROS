#!/bin/sh
set -eu
test_src_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d)
trap 'rm -f -- "$test_build_dir/test"; rmdir -- "$test_build_dir"' EXIT HUP INT TERM
if [ "$#" -gt 0 ]; then
    # Optional: compare the production encoders directly with Linux's ABI layout.
    ${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 -DHAVE_LINUX_MQD_LAYOUT -I"$1" \
        "$test_src_dir/test_gfx_mqd.c" -o "$test_build_dir/test"
else
    ${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 \
        "$test_src_dir/test_gfx_mqd.c" -o "$test_build_dir/test"
fi
"$test_build_dir/test"
