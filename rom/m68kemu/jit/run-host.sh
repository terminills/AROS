#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
build=${1:?private build directory required}
mkdir -p "$build"
build=$(cd "$build" && pwd)
export TMPDIR="$build"
make -C "$build" -f "$here/host.mk" SOURCE="$here" -j2
"$build/moveq-test"
