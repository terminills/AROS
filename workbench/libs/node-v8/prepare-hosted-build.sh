#!/bin/sh
#
# Repair the host-tool side of a cross-configured Node build without changing
# target linkage. Node propagates --shared-libuv/--shared-zlib target paths into
# node_js2c.host.mk; that makes the Linux host compiler consume AROS headers and
# archives. The target makefiles must retain those paths.
#
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 NODE_SOURCE HOST_LIB_DIR" >&2
    exit 2
fi

node_source=$1
host_lib_dir=$2
host_makefile=$node_source/out/node_js2c.host.mk
aros_root=/home/terminills/Documents/GitHub/aros-opal-hosted-clean/bin/linux-x86_64
aros_include=$aros_root/AROS/Developer/include
uv_include=$aros_root/Ports/libuv/libuv/libuv-v1.48.0/include
aros_lib=$aros_root/AROS/Developer/lib
host_uv_include=$node_source/deps/uv/include

if [ ! -f "$host_makefile" ]; then
    echo "missing configured host makefile: $host_makefile" >&2
    exit 1
fi

mkdir -p "$host_lib_dir"
ar rcs "$host_lib_dir/libuv1.a"
ln -sfn /usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 "$host_lib_dir/libuv1.so"
ln -sfn /usr/lib/x86_64-linux-gnu/libz.a "$host_lib_dir/libz1.a"

# These are generated makefiles, so keep the correction deterministic and
# fail if Node's generator stops emitting the expected target-only paths.
python3 - "$node_source/out" "$aros_include" "$uv_include" "$aros_lib" \
    "$host_lib_dir" "$host_uv_include" <<'PY'
from pathlib import Path
import sys

out_dir = Path(sys.argv[1])
aros_include, uv_include, aros_lib, host_lib_dir, host_uv_include = sys.argv[2:]
makefiles = sorted(out_dir.rglob("*.host.mk"))
if not makefiles:
    raise SystemExit(f"no generated host makefiles below {out_dir}")

include_hits = 0
lib_hits = 0
for makefile in makefiles:
    text = makefile.read_text()
    line = f"\t-I{aros_include} \\\n"
    include_hits += text.count(line)
    text = text.replace(line, "")
    line = f"\t-I{uv_include} \\\n"
    include_hits += text.count(line)
    text = text.replace(line, f"\t-I{host_uv_include} \\\n")
    line = f"\t-L{aros_lib} \\\n"
    count = text.count(line)
    lib_hits += count
    text = text.replace(line, f"\t-L{host_lib_dir} \\\n")
    makefile.write_text(text)

if include_hits == 0 or lib_hits == 0:
    raise SystemExit(
        "host makefile drift; expected target include/library paths absent "
        f"(include_hits={include_hits}, lib_hits={lib_hits})"
    )
PY

echo "prepared Linux host tool linkage below: $node_source/out"
