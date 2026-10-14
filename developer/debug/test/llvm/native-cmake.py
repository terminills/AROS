"""Exercise native CMake configure, parallel build, and executable loading.

Run inside AROS with a fresh output directory argument. Requires the native
SDK, CMake modules, matching uv1.library, bin:/usr: assignments and bsdsocket.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

assert sys.platform.startswith("aros"), sys.platform
root = Path(sys.argv[1]).resolve()
root.mkdir(parents=True, exist_ok=False)
source = root / "source"
source.mkdir()
sdk = Path("/SYS/Developer")
os.environ["PATH"] = str(sdk / "bin") + ":/SYS/C"
os.environ["TMPDIR"] = "/T"
results = []


def run(name, argv, expected=None):
    result = subprocess.run(list(map(str, argv)), cwd=root,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (root / (name + ".log")).write_bytes(result.stdout)
    print(result.stdout.decode(errors="replace"), end="", flush=True)
    results.append({"name": name, "argv": list(map(str, argv)),
                    "returncode": result.returncode})
    (root / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    assert result.returncode == 0, (name, result.returncode)
    if expected is not None:
        assert result.stdout == expected, (name, result.stdout)


(source / "CMakeLists.txt").write_text(
    "cmake_minimum_required(VERSION 3.20)\n"
    "project(NativeProbe LANGUAGES C CXX)\n"
    "add_executable(cprobe cprobe.c)\n"
    "add_executable(cxxprobe cxxprobe.cpp)\n")
(source / "cprobe.c").write_text(
    '#include <stdio.h>\nint main(void) { puts("CMAKE-C-PASS"); return 0; }\n')
(source / "cxxprobe.cpp").write_text(
    '#include <cstdio>\n#include <vector>\n'
    'int main() { std::vector<int> v{3,5,7}; int s=0; for(int x:v)s+=x; '
    'std::printf("CMAKE-CXX-%s sum=%d\\n",s==15?"PASS":"FAIL",s); '
    'return s!=15; }\n')
(source / "toolchain.cmake").write_text(
    'set(CMAKE_SYSTEM_NAME AROS)\nset(CMAKE_SYSTEM_PROCESSOR x86_64)\n'
    'set(CMAKE_C_COMPILER /SYS/Developer/bin/clang)\n'
    'set(CMAKE_CXX_COMPILER /SYS/Developer/bin/clang++)\n'
    'set(CMAKE_MAKE_PROGRAM /SYS/Developer/bin/make CACHE FILEPATH "")\n'
    'set(CMAKE_C_FLAGS_INIT "-nix")\nset(CMAKE_CXX_FLAGS_INIT "-nix")\n')
probe = root / "process.cmake"
probe.write_text(
    'execute_process(COMMAND /SYS/Developer/bin/sh -c '
    '"printf CMAKE-EXEC-PASS; exit 7" OUTPUT_VARIABLE out RESULT_VARIABLE rc)\n'
    'if(NOT out STREQUAL "CMAKE-EXEC-PASS" OR NOT rc STREQUAL "7")\n'
    '  message(FATAL_ERROR "Subprocess failed: ${rc} / ${out}")\nendif()\n')
cmake = sdk / "bin/cmake"
run("process", [cmake, "-P", probe])
run("configure", [cmake, "-S", source, "-B", root / "build", "-G",
                  "Unix Makefiles", "-DCMAKE_TOOLCHAIN_FILE=" +
                  str(source / "toolchain.cmake")])
run("build", [cmake, "--build", root / "build", "--parallel", "4", "--verbose"])
run("c-run", [root / "build/cprobe"], b"CMAKE-C-PASS\n")
run("cxx-run", [root / "build/cxxprobe"], b"CMAKE-CXX-PASS sum=15\n")
print("NATIVE-CMAKE-PASS", flush=True)
