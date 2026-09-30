#!/bin/sh
# Standalone userspace helper. Never install it or load a resident driver here.
# The explicit native SDK avoids the compiler's unrelated hosted default SDK.
set -eu
if [ "$#" -ne 4 ]; then
    echo "usage: sh build_file_sha256.sh AROS_SOURCE NATIVE_DEVELOPER_SDK CROSS_GCC OUTPUT" >&2
    exit 2
fi
source_root=$1
native_sdk=$2
cross_gcc=$3
output_path=$4
test -f "$native_sdk/lib/startup.o"
test -f "$native_sdk/lib/libposixc.a"
test ! -e "$output_path"
set -x
"$cross_gcc" --sysroot="$native_sdk" -B"$native_sdk/lib/" \
    -O2 -Wall -Wextra -m64 -mcmodel=large -mno-red-zone -mno-ms-bitfields \
    -I"$source_root/workbench/network/WirelessManager/src/utils" \
    -I"$source_root/workbench/network/WirelessManager/src/crypto" \
    "$source_root/workbench/hidds/amdgpu/test/AmdgpuFileSHA256.c" \
    -Wl,-Map,"$output_path.map" -o "$output_path"
