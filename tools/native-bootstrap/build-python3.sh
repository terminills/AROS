#!/bin/sh
# Experimental build-host interpreter for ADT self-hosting (JOB-0127).
# Usage: build-python3.sh WORK SYSROOT CROSSTOOLS [BUILD_PYTHON]
set -eu
work=$1
sysroot=$2
cross=$3
build_python=${4:-$(command -v python3)}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$work"
cd "$work"
if [ ! -f Python-3.12.12.tar.xz ]; then
    curl -fL --retry 3 -o Python-3.12.12.tar.xz \
        https://www.python.org/ftp/python/3.12.12/Python-3.12.12.tar.xz
fi
echo 'fb85a13414b028c49ba18bbd523c2d055a30b56b18b92ce454ea2c51edc656c4  Python-3.12.12.tar.xz' | sha256sum -c -
if [ ! -d Python-3.12.12 ]; then
    tar xf Python-3.12.12.tar.xz
    patch -d Python-3.12.12 -p1 < "$script_dir/Python-3.12.12-aros.patch"
fi
mkdir -p build
cd build
CC="$cross/x86_64-aros-gcc" AR="$cross/x86_64-aros-ar" \
RANLIB="$cross/x86_64-aros-ranlib" READELF="$cross/x86_64-aros-readelf" \
CFLAGS="--sysroot=$sysroot -O2 -mcmodel=large -mno-red-zone -D_GNU_SOURCE" \
LDFLAGS="--sysroot=$sysroot -nix" \
ac_cv_file__dev_ptmx=no ac_cv_file__dev_ptc=no \
ac_cv_buggy_getaddrinfo=no MODULE_BUILDTYPE=static \
../Python-3.12.12/configure --host=x86_64-unknown-aros \
    --build="$(../Python-3.12.12/config.guess)" --prefix=/Developer \
    --disable-shared --without-ensurepip --disable-test-modules \
    --disable-ipv6 --with-pkg-config=no --with-build-python="$build_python"
cat > Modules/Setup.local <<'EOF'
# Bootstrap-only: optional modules requiring additional AROS interfaces.
*disabled*
_socket
syslog
termios
_multiprocessing
EOF
make -j"${JOBS:-8}" python pybuilddir.txt
mkdir -p "$sysroot/bin" "$sysroot/lib/python3.12"
cp python "$sysroot/bin/python3"
cp -R ../Python-3.12.12/Lib/. "$sysroot/lib/python3.12/"
# sysconfig needs the target build metadata, not just the source Lib directory.
cp "$(cat pybuilddir.txt)"/_sysconfigdata_*.py "$sysroot/lib/python3.12/"
