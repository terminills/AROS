#!/usr/bin/env python3
"""Package an already tested AROS-native toolchain without modifying its donor."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

TOOLS = ('clang', 'clang++', 'llvm-ar', 'llvm-ranlib', 'ld', 'collect-aros',
         'make', 'sh', 'rm', 'mkdir')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--developer', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--notices', type=Path, help='Licence and source provenance directory to include')
    args = parser.parse_args()
    donor = args.developer.resolve()
    root = args.output.resolve()
    if root.exists():
        parser.error('output must be a new directory; existing kits are never overwritten')
    for name in TOOLS:
        path = donor / 'bin' / name
        with path.open('rb') as f:
            header = f.read(20)
        if header[:5] != b'\x7fELF\x02' or header[18:20] != b'\x3e\x00' or header[7:9] != b'\x0f\x01':
            parser.error(f'{path}: expected an AROS ABIv1 x86_64 ELF tool')
    for name in ('include', 'lib'):
        if not (donor / name).is_dir():
            parser.error(f'missing SDK directory: {name}')
    here = Path(__file__).resolve().parent
    repo = here.parent.parent
    kit = root / 'Development'
    (kit / 'bin').mkdir(parents=True)
    for name in TOOLS:
        shutil.copy2(donor / 'bin' / name, kit / 'bin' / name)
    for name in ('include', 'lib', 'libexec', 'share'):
        if (donor / name).is_dir():
            shutil.copytree(donor / name, kit / name, symlinks=False)
    shutil.copytree(repo / 'developer/demos/HelloAROS', kit / 'Examples/HelloAROS')
    (kit / 'S').mkdir()
    shutil.copy2(here / 'Build-HelloAROS', kit / 'S/Build-HelloAROS')
    shutil.copy2(here / 'Development-Demo', kit / 'S/Development-Demo')
    shutil.copy2(here / 'Development-Startup', root / 'Development-Startup')
    shutil.copy2(here / 'Install-Development', root / 'Install-Development')
    shutil.copy2(here / 'README.md', root / 'README.md')
    if args.notices:
        shutil.copytree(args.notices, root / 'Notices', symlinks=False)
    files = []
    for path in sorted(root.rglob('*')):
        if path.is_file():
            h = hashlib.sha256()
            with path.open('rb') as f:
                for chunk in iter(lambda: f.read(1024 * 1024), b''):
                    h.update(chunk)
            files.append({'path': str(path.relative_to(root)), 'size': path.stat().st_size,
                          'sha256': h.hexdigest()})
    (root / 'manifest.json').write_text(json.dumps({
        'donor': str(donor), 'scope': 'packaged native bootstrap compiler, not a self-built compiler',
        'runtime_validation': 'pending on assembled kit', 'files': files}, indent=2) + '\n')
    print(f'Packaged {len(files)} files at {root}')


if __name__ == '__main__':
    main()
