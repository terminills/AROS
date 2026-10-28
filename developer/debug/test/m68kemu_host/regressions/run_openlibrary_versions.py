#!/usr/bin/env python3
"""Compile the production OpenLibrary thunks with a small host fixture.

The selected function bodies are extracted verbatim from m68kemu_thunks.c.
No AROS SDK, Moira checkout or emulated machine is needed.
"""
# Copyright (C) 2026, The AROS Development Team. All rights reserved.

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def extract_function(source, name):
    masked = re.sub(
        r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: ''.join('\n' if c == '\n' else ' ' for c in match[0]),
        source,
    )
    match = re.search(
        r'(?m)^static IPTR\s+' + re.escape(name) +
        r'\s*\([^;{}]*\)[^;{}]*\{', masked)
    if not match:
        raise ValueError(f'definition of {name} not found')
    end = masked.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (masked[end] == '{') - (masked[end] == '}')
        end += 1
    line = source[:match.start()].count('\n') + 1
    return f'#line {line} "m68kemu_thunks.c"\n' + source[match.start():end] + '\n'


def run(args, output):
    source = (args.source_root / 'rom/m68kemu/m68kemu_thunks.c').read_text()
    names = ('open_library', 'thunk_exec_OpenLibrary',
             'thunk_exec_OldOpenLibrary')
    functions = []
    for name in names:
        if name == 'open_library' and not re.search(r'^static IPTR open_library\s*\(', source, re.M):
            continue  # The unmodified parent has no shared helper.
        functions.append(extract_function(source, name))
    offset_text = (args.source_root / 'rom/m68kemu/m68kemu_offsets.h').read_text()
    version_offset = re.search(r'^#define\s+M68K_LIB_VERSION\s+(\S+)',
                               offset_text, re.M)
    if not version_offset:
        raise ValueError('M68K_LIB_VERSION not found')
    output.mkdir(parents=True, exist_ok=True)
    (output / 'functions.inc').write_text('\n'.join(functions))
    (output / 'constants.inc').write_text(
        f'#define M68K_LIB_VERSION {version_offset[1]}\n')
    command = shlex.split(args.cxx) + [
        '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
        '-g', '-O1', '-I' + str(output),
        str(Path(__file__).with_name('test_openlibrary_versions.cpp')),
        '-o', str(output / 'test_openlibrary_versions'),
    ]
    if args.sanitize:
        command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    subprocess.run(command, check=True)
    env = os.environ.copy()
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
    subprocess.run([str(output / 'test_openlibrary_versions')], check=True, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path,
                        default=Path(__file__).resolve().parents[5])
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    if args.build_dir:
        run(args, args.build_dir.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix='m68kemu-openlibrary-') as path:
            run(args, Path(path))


if __name__ == '__main__':
    main()
