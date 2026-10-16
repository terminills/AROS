#!/bin/sh
# Bootstrap native collect-aros with the native binutils BFD backend.
# Usage: build-collect-aros.sh BUILD SYSROOT CROSSTOOLS
set -eu
build=$1
sysroot=$2
cross=$3
source=$(CDPATH= cd -- "$(dirname -- "$0")/../collect-aros" && pwd)
libiberty="$build/bin/linux-x86_64/gen/contrib/gnu/binutils/binutils/libiberty/libiberty.a"
# development-binutils must have installed native libbfd and its headers.
# The generic backend uses popen/pclose, which is incomplete on ADT.
"$cross/x86_64-aros-gcc" --sysroot="$sysroot" -O2 \
    -mcmodel=large -mno-red-zone -nix -D_GNU_SOURCE -D_NATIVE_ \
    -D_HAVE_LIBIBERTY_ '-DOBJLIBDIR="/Developer/lib"' \
    -I"$build/bin/linux-x86_64/gen/tools/collect-aros" -I"$source" \
    "$source/collect-aros.c" "$source/gensets.c" "$source/misc.c" \
    "$source/backend-bfd.c" "$source/docommand-exec.c" \
    -lbfd "$libiberty" -lz -o "$sysroot/bin/collect-aros"
