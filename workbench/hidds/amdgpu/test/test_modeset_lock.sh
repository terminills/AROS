#!/bin/sh
set -eu
test_src_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d)
trap 'rm -f -- "$test_build_dir/modeset_lock.inc" "$test_build_dir/test"; rmdir -- "$test_build_dir"' EXIT HUP INT TERM
test_source=${MODESET_LOCK_SOURCE:-$test_src_dir/../amdgpu_modeset.c}
awk '/^static const ULONG otg_(control|master_update_lock)_dw/ {print}
     /^#define (OTG_MASTER_UPDATE_LOCK_BIT|OTG_UPDATE_LOCK_STATUS_BIT|AMDGPU_OTG_INST_MAX)/ {print}
     /^static BOOL amdgpu_modeset_device_alive\(/ {copy=1}
     /^BOOL amdgpu_modeset_lock\(/ {copy=1}
     /^void amdgpu_modeset_unlock\(/ {copy=1}
     /^\/\*/ {copy=0}
     copy' "$test_source" > "$test_build_dir/modeset_lock.inc"
test -s "$test_build_dir/modeset_lock.inc"
${CC:-cc} ${CFLAGS:-} -std=c11 -Wall -Wextra -Werror -O2 -I"$test_build_dir" \
    "$test_src_dir/test_modeset_lock.c" -o "$test_build_dir/test"
"$test_build_dir/test"
