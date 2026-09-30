#!/bin/sh
set -eu
test_src_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d)
trap 'rm -f -- "$test_build_dir/test"; rmdir -- "$test_build_dir"' EXIT HUP INT TERM
if [ "$#" -eq 1 ]; then
    ${CC:-cc} ${CFLAGS:-} -std=c11 -Wall -Wextra -Werror -O2 -DVERIFY_LINUX_DCN41 \
        -I"$1" "$test_src_dir/test_plane.c" -o "$test_build_dir/test"
else
    ${CC:-cc} ${CFLAGS:-} -std=c11 -Wall -Wextra -Werror -O2 \
        "$test_src_dir/test_plane.c" -o "$test_build_dir/test"
fi
"$test_build_dir/test"
