#!/usr/bin/env python3
"""Build a signed, disposable native Software Center demo repository."""
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
import argparse
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--publisher', type=Path, required=True, help='arospkg-server checkout')
    ap.add_argument('--output', type=Path, required=True, help='new output directory')
    ap.add_argument('--url', default='http://10.0.2.2:18096/', help='repository URL visible to AROS')
    args = ap.parse_args()
    root = args.output.resolve()
    if root.exists():
        ap.error('output already exists; retain it and use a new directory')
    if any(c in args.url for c in '\r\n"') or not args.url.startswith(('http://', 'https://')):
        ap.error('url must be a single HTTP(S) URL without quotes')
    publisher = args.publisher.resolve()
    if not (publisher / 'arospkg/__main__.py').is_file():
        ap.error('publisher is not an arospkg-server checkout')
    root.mkdir(parents=True)
    with (root / 'prepare.log').open('w') as log:
        def publish(*params):
            subprocess.run([sys.executable, '-m', 'arospkg', *map(str, params)],
                           cwd=publisher, stdout=log, stderr=subprocess.STDOUT, check=True)
        publish('keygen', '--out', root / 'keys/demo')
        for name, version, target in [('AmiWestWelcome', '1.0.0', 'baseline'),
                                      ('AmiWestWelcome', '1.0.1', 'repo'),
                                      ('AmiWestTools', '1.0.0', 'repo')]:
            source = root / 'sources' / f'{name}-{version}'
            (source / 'files/S').mkdir(parents=True)
            (source / 'PKGINFO').write_text(
                f'Package: {name}\nVersion: {version}\nArch: all\n'
                'Summary: AmiWest native AROS demo\n'
                'Description: Signed demonstration package installed in disposable RAM storage.\n'
                'Section: utilities\nLicence: APL-1.1\nReboot: no\n'
                'Prefix: RAM:AmiWestDemo\n')
            (source / 'files/S' / name).write_text(f'Echo "{name} {version} installed successfully"\n')
            archive = root / 'archives' / f'{name}_{version}_all.tar.gz'
            archive.parent.mkdir(exist_ok=True)
            publish('mkpkg', source, '--out', archive)
            publish('publish', archive, '--repo', root / target)
        for target in ('baseline', 'repo'):
            publish('index', '--repo', root / target, '--key', root / 'keys/demo.sec',
                    '--origin', 'AmiWest ADT AROS demo', '--suite', 'testing')
        publish('announce', '--repo', root / 'repo', '--key', root / 'keys/demo.sec',
                '--severity', 'notice', '--title', 'AmiWest updates ready',
                '--body', 'Update AmiWestWelcome, then install AmiWestTools.',
                '--package', 'AmiWestWelcome')
        for target in ('baseline', 'repo'):
            publish('verify', '--repo', root / target, '--pub', root / 'keys/demo.pub')
    payload = root / 'payload/SoftwareCenterDemo'
    shutil.copytree(root / 'baseline', payload / 'baseline')
    shutil.copy2(root / 'keys/demo.pub', payload / 'demo.pub')
    startup = Path(__file__).with_name('SoftwareCenter-Demo-Startup').read_text()
    (payload / 'Startup').write_text(startup.replace('@REPO_URL@', args.url.rstrip('/') + '/'))
    print(f'Payload: {payload}\nRepository: {root / "repo"}\nPrivate signing key stays outside payload.')


if __name__ == '__main__':
    main()
