"""Cross-bootstrap CMake 3.31.8 for native AROS builds; does not install it.

Requires an existing AROS SDK, matching uv1 module/import library/headers,
configured curl source/build tree, SSL SDK and AROS cross tools. The output
must pass developer/debug/test/llvm/native-cmake.py inside AROS before use.
The uv internal-symbol thunks are coupled to the supplied uv1 module.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

p = argparse.ArgumentParser(description=__doc__)
for name in ("archive", "output", "sdk", "ssl-sdk", "curl-source", "curl-build",
             "uv-include", "uv-import", "uv-module", "uv-generator"):
    p.add_argument("--" + name, type=Path, required=True)
p.add_argument("--cross-prefix", required=True)
p.add_argument("--jobs", type=int, default=4)
a = p.parse_args()
for name, value in vars(a).items():
    if isinstance(value, Path):
        setattr(a, name, value.resolve())
assert hashlib.sha256(a.archive.read_bytes()).hexdigest() == (
    "e3cde3ca83dc2d3212105326b8f1b565116be808394384007e7ef1c253af6caa")
a.output.mkdir(parents=True, exist_ok=False)
here = Path(__file__).resolve().parent
source = a.output / "cmake-3.31.8"
build = a.output / "build"
commands = []


def run(argv, cwd=a.output):
    argv = list(map(str, argv))
    commands.append({"argv": argv, "cwd": str(cwd)})
    (a.output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    print("RUN", *argv, flush=True)
    subprocess.run(argv, cwd=cwd, check=True)


with tarfile.open(a.archive) as archive:
    archive.extractall(a.output, filter="data")
run(["patch", "-p1", "-i", here / "cmake-3.31.8-aros.patch"], source)
platform = a.output / "modules/Platform"
platform.mkdir(parents=True)
shutil.copy2(here / "AROS.cmake", platform / "AROS.cmake")
cc, cxx = a.cross_prefix + "gcc", a.cross_prefix + "g++"
ar, nm = a.cross_prefix + "ar", a.cross_prefix + "nm"
flags = ["-mcmodel=large", "-mno-red-zone", "-nix"]
# This curl archive must not depend on POSIX fnmatch, absent from this SDK.
matcher = a.output / "curl_fnmatch.c"
text = (a.curl_source / "lib/curl_fnmatch.c").read_text()
assert '#include "curl_setup.h"' in text
matcher.write_text(text.replace('#include "curl_setup.h"',
                               '#include "curl_setup.h"\n#undef HAVE_FNMATCH'))
obj = a.output / "libcurl_la-curl_fnmatch.o"
run([cc, "--sysroot=" + str(a.sdk), *flags, "-O2", "-D_XOPEN_SOURCE=600",
     "-D__BSD_VISIBLE=1", "-DHAVE_CONFIG_H", "-DBUILDING_LIBCURL",
     "-I" + str(a.curl_source / "include"), "-I" + str(a.curl_build / "lib"),
     "-I" + str(a.curl_source / "lib"), "-isystem", a.ssl_sdk / "include",
     "-c", matcher, "-o", obj])
curl = a.output / "libcurl-cmake.a"
shutil.copy2(a.curl_build / "lib/.libs/libcurl.a", curl)
run([ar, "r", curl, obj])
thunks = a.output / "libuv1-cmake.a"
run([ar, "rcs", thunks])
toolchain = a.output / "aros-toolchain.cmake"
toolchain.write_text(
    'set(CMAKE_SYSTEM_NAME AROS)\nset(CMAKE_SYSTEM_PROCESSOR x86_64)\n' +
    f'set(CMAKE_SYSROOT "{a.sdk}")\nset(CMAKE_C_COMPILER "{cc}")\n' +
    f'set(CMAKE_CXX_COMPILER "{cxx}")\n' +
    f'set(CMAKE_MODULE_PATH "{platform.parent}")\n' +
    f'set(CMAKE_FIND_ROOT_PATH "{a.sdk}")\n' +
    'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n' +
    'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n' +
    'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n')
run(["cmake", "-S", source, "-B", build,
     "-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain),
     "-DCMAKE_C_FLAGS=" + " ".join(flags) + " -D_BSD_SOURCE",
     "-DCMAKE_CXX_FLAGS=" + " ".join(flags) + " -D_BSD_SOURCE -Drestrict=__restrict__",
     "-DCMAKE_CXX_STANDARD=17", "-DBUILD_TESTING=OFF", "-DBUILD_CursesDialog=OFF",
     "-DCMAKE_USE_OPENSSL=OFF", "-DCMAKE_USE_SYSTEM_LIBUV=ON",
     "-DCMAKE_USE_SYSTEM_CURL=ON", "-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=TRUE",
     "-DLibUV_INCLUDE_DIR=" + str(a.uv_include), "-DLibUV_LIBRARY=" + str(a.uv_import),
     "-DCURL_INCLUDE_DIR=" + str(a.curl_source / "include"),
     "-DCURL_LIBRARY_RELEASE=" + str(curl),
     "-DZLIB_INCLUDE_DIR=" + str(a.ssl_sdk / "include"),
     "-DZLIB_LIBRARY_RELEASE=" + str(a.ssl_sdk / "lib/libz.a"),
     "-DCMAKE_EXE_LINKER_FLAGS=-Wl,--wrap=uv_pipe_open",
     f"-DCMAKE_CXX_STANDARD_LIBRARIES={thunks} -L{a.ssl_sdk / 'lib'} "
     "-lssl -lcrypto -lz -lmiami -lnet"])
run(["cmake", "--build", build, "--target", "CMakeLib", "-j", a.jobs])
assembly = a.output / "uv1-cmake.S"
run([sys.executable, a.uv_generator, "--nm", nm, "--needed-archive",
     build / "Source/libCMakeLib.a", "--link-archive", a.uv_import,
     "--library-module", a.uv_module, "--output", assembly])
run([cc, *flags, "-c", assembly, "-o", a.output / "uv1-cmake.o"])
run([ar, "rcs", thunks, a.output / "uv1-cmake.o"])
run(["cmake", "--build", build, "--target", "cmake", "-j", a.jobs])
for artifact in (build / "bin/cmake", a.uv_module, a.uv_import):
    print(hashlib.sha256(artifact.read_bytes()).hexdigest(), artifact)
print("Cross-bootstrap built; native validation is still required.")
