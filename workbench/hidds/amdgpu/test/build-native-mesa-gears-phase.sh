#!/bin/sh
# New userspace diagnostic only. No installation or resident driver loading.
set -eu
if [ "$#" -ne 4 ]; then
    echo "usage: sh build-native-mesa-gears-phase.sh SOURCE NATIVE_SDK CROSS_GCC OUTPUT" >&2
    exit 2
fi
phase_source=$1
phase_sdk=$2
phase_cc=$3
phase_output=$4
test -f "$phase_sdk/lib/startup.o"
test -f "$phase_sdk/lib/libglut.a"
test ! -e "$phase_output"
test "$(sha256sum "$phase_source/contrib/Demo/GL/MesaGL/gears.c" | cut -d ' ' -f1)" = \
    dcc52bdace248ef0550607bb6a2d01122d3f4fc790fbd7fe1b9e9f1ecc6440a9
set -x
"$phase_cc" --sysroot="$phase_sdk" -B"$phase_sdk/lib/" \
    -O2 -g -Wall -Wextra -Werror=format -m64 -mcmodel=large -mno-red-zone -mno-ms-bitfields \
    "$phase_source/workbench/hidds/amdgpu/test/NativeMesaGearsPhase.c" \
    -lglut -lGL -lGLU -lm -Wl,-Map,"$phase_output.map" -o "$phase_output"
