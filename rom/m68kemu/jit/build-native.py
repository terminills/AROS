#!/usr/bin/env python3
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
"""Build a standalone diagnostic library in a NEW private output directory.

Source-DOS headers and a C++ overlay are explicit integration inputs, never a
claim of published SDK qualification. No source tree or SDK is modified.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("build", "sdk", "contrib", "cc", "genmodule"):
        p.add_argument("--" + name, type=Path, required=True)
    p.add_argument("--cxx-overlay", type=Path)
    p.add_argument("--source-dos-headers", action="store_true")
    p.add_argument("--gcc-version", default="15.2.0")
    args = p.parse_args()
    src = Path(__file__).resolve().parent.parent
    repo = src.parent.parent
    b = args.build.resolve()
    b.mkdir(parents=True, exist_ok=False)
    gen = b / "generated"
    gen.mkdir()
    logs = b / "logs"
    logs.mkdir()
    env = dict(os.environ, TMPDIR=str(b))
    commands = []

    def run(cmd, name):
        cmd = [str(x) for x in cmd]
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, env=env)
        (logs / (name + ".log")).write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(f"{name} failed ({result.returncode}); see {logs}")

    tool = b / "genmodule"
    shutil.copy2(args.genmodule.resolve(), tool)
    provenance = {"source": str(src), "diagnostic_cxx_overlay": str(args.cxx_overlay),
                  "source_dos_headers": args.source_dos_headers,
                  "compiler": str(args.cc.resolve()),
                  "compiler_sha256": hashlib.file_digest(args.cc.open("rb"), "sha256").hexdigest(),
                  "genmodule_sha256": hashlib.file_digest(tool.open("rb"), "sha256").hexdigest()}
    (b / "provenance.txt").write_text(json.dumps(provenance, indent=2))
    for action in ("writefiles", "writelibdefs"):
        cmd = [tool, "-c", src / "m68kemu.conf", "-d", gen, action, "m68kemu", "library"]
        commands.append([str(x) for x in cmd])
        run(cmd, action)
    base = [str(args.cc.resolve()), "--sysroot=" + str(args.sdk.resolve()),
            "-O2", "-mcmodel=large", "-mno-red-zone", "-fno-strict-aliasing",
            "-DNO_INLINE_STDARG", "-DM68KJIT_NATIVE_EXECMEM", "-I" + str(src),
            "-I" + str(src / "Moira"), "-I" + str(gen),
            '-DLC_LIBDEFS_FILE="' + str(gen / "m68kemu_libdefs.h") + '"']
    if args.source_dos_headers:
        headers = b / "source-headers" / "dos"
        headers.mkdir(parents=True)
        for name in ("dosextens.h", "dostags.h"):
            shutil.copy2(repo / "compiler/include/dos" / name, headers / name)
        base += ["-I" + str(headers.parent)]
    lib = args.contrib.resolve() / "lib/gcc/x86_64-aros" / args.gcc_version
    cxx = ["-std=c++20", "-nostdinc++"]
    includes = ([args.cxx_overlay.resolve()] if args.cxx_overlay else [])
    includes += [lib / "include/c++", lib / "include/c++/x86_64-aros", lib / "include/c++/backward"]
    for path in includes:
        cxx += ["-isystem", str(path)]
    names = ("init", "run", "memory", "thunks", "shadow", "m68klib", "stubs",
             "math", "devices", "boopsi", "thunks_gen")
    sources = [gen / "m68kemu_start.c"] + [src / ("m68kemu_" + n + ".c") for n in names]
    sources += [src / n for n in ("m68kemu_moira.cpp", "jit/moveq_cache.cpp",
                                  "Moira/Moira/Moira.cpp", "Moira/Moira/MoiraDebugger.cpp")]
    sources += [gen / "m68kemu_end.c"]
    inputs = list(sources) + [src / "m68kemu.conf"]
    inputs += list((src / "jit").glob("*.h")) + list((src / "jit").glob("*.inc"))
    if args.source_dos_headers:
        inputs += list((b / "source-headers/dos").glob("*.h"))
    if args.cxx_overlay:
        inputs += [f for f in args.cxx_overlay.resolve().rglob("*") if f.is_file()]
    (b / "input-hashes.txt").write_text("\n".join(
        hashlib.file_digest(f.open("rb"), "sha256").hexdigest() + " " + str(f)
        for f in inputs))
    objects = [b / (f.stem + ".o") for f in sources]
    compile_commands = [base + (cxx if f.suffix == ".cpp" else ["-std=gnu99"]) +
                        ["-c", str(f), "-o", str(o)] for f, o in zip(sources, objects)]
    commands += compile_commands
    (b / "commands.txt").write_text(json.dumps(commands, indent=2))
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(run, cmd, f.stem) for cmd, f in zip(compile_commands, sources)]
        # Observe every outcome before raising; preserve all failed logs.
        failures = []
        for future in futures:
            try:
                future.result()
            except Exception as error:
                failures.append(str(error))
        if failures:
            raise RuntimeError("\n".join(failures))
    link = [str(args.cc.resolve()), "--sysroot=" + str(args.sdk.resolve()),
            "-mcmodel=large", "-mno-red-zone", "-nostartfiles", "-static"]
    link += [str(o) for o in objects]
    link += ["-L" + str(lib), "-Wl,--start-group", "-lstdc++", "-lposixc", "-lstdcio",
             "-lstdc", "-Wl,--end-group", "-Wl,-Map," + str(b / "m68kemu.map"),
             "-o", str(b / "m68kemu.library")]
    commands.append(link)
    (b / "commands.txt").write_text(json.dumps(commands, indent=2))
    run(link, "link")
    digest = hashlib.file_digest((b / "m68kemu.library").open("rb"), "sha256").hexdigest()
    (b / "result.txt").write_text(digest + " m68kemu.library\n")
    print("Built diagnostic library:", b / "m68kemu.library", digest)


if __name__ == "__main__":
    main()
