#!/usr/bin/env python3
"""Copy a handed-off corpus into a NEW private workspace; never boot a VM.

Copyright (C) 2026, The AROS Development Team. All rights reserved.
Run only after relay scope ownership is established. Reference files are read-only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reference-qt', type=Path, required=True)
parser.add_argument('--maps', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
reference = args.reference_qt.resolve()
corpus = args.maps.resolve().parent
root = args.out.resolve()
if root.exists():
    parser.error('output must be a new directory; preserve previous runs')
root.mkdir(parents=True)
qt = root / 'qemu-test'
inputs = root / 'inputs'
manifest = []


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()


def copy(source, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, dest)
    manifest.append({'source': str(source), 'copy': str(dest), 'sha256': digest(dest)})


for name in ('logs', 'shots', 'disks', 'ovmf', 'evidence/m68kemu-corpus-20261024'):
    (qt / name).mkdir(parents=True, exist_ok=True)
for name in ('run-qemu.sh', 'mon.sh', 'pointer.py', 'boot-and-execute.sh', 'run-click.sh', 'run-batch.sh'):
    source = (corpus if name in ('run-click.sh', 'run-batch.sh') else reference) / name
    text = source.read_text().replace(str(reference.parent), str(root))
    text = text.replace('NAME=m68k', 'NAME=m68k-jit').replace('TAG=m68k ', 'TAG=m68k-jit ')
    text = text.replace('latest-m68k.txt', 'latest-m68k-jit.txt')
    text = text.replace('VNC:-9', 'VNC:-29').replace('VNCBIND:-0.0.0.0', 'VNCBIND:-127.0.0.1')
    text = text.replace('[ "${DELETE_ISO:-0}" = 1 ] && rm -f "$ISO"', '# Preserve image and failed attempts.')
    if name == 'boot-and-execute.sh':
        start = text.index('for pid in $(lsof')
        end = text.index('sleep 2', text.index('\ndone', start))
        text = text[:start] + '''if lsof -t "$HD" >/dev/null 2>&1; then
  echo "Private scratch disk in use; refusing to stop unknown process" >&2
  exit 2
fi
''' + text[end:]
    if name == 'run-click.sh':
        text = text.replace('echo "marker rc=$?"',
                            'rc=$?; echo "marker rc=$rc"; if [ "$rc" -ne 0 ]; then ./run-qemu.sh stop; exit 2; fi')
    if name == 'run-batch.sh':
        text = text.replace('echo "BATCH-DONE',
                            '''strings "$S" | /bin/grep -a -q 'cp done' || { echo "MISSING completion marker"; exit 2; }
echo "BATCH-DONE''')
    dest = qt / name
    dest.write_text(text)
    dest.chmod(0o755)
    manifest.append({'source': str(source), 'source_sha256': digest(source),
                     'adapted': str(dest), 'adapted_sha256': digest(dest)})

maps = shlex.split(args.maps.read_text())
if len(maps) % 3:
    raise ValueError('malformed mapping triples')
private_maps = []
for offset in range(0, len(maps), 3):
    option, source, target = maps[offset:offset + 3]
    if option != '-map' or not target.startswith('/') or '..' in Path(target).parts:
        raise ValueError('unsupported mapping: ' + repr(maps[offset:offset + 3]))
    dest = inputs / target.lstrip('/')
    copy(Path(source), dest)
    private_maps.extend(['-map', str(dest), target])
for name in ('gpq-corpus4.iso', 'aminet/batch1/gpq-batch1.iso',
             'aminet/batch1/gpq-ra68.iso', 'aminet/reaction/gpq-demo68.iso'):
    copy(corpus / name, inputs / Path(name).name)
copy(corpus / 'RECIPE.txt', root / 'RECIPE-original.txt')
(root / 'private-maps.json').write_text(json.dumps(private_maps, indent=2))
(root / 'input-manifest.json').write_text(json.dumps(manifest, indent=2))
print(root)
print('Prepared only. Claim the corpus runtime scope before starting a VM.')
