"""Extract and follow relative archive symlinks, including forward references."""
import io
from pathlib import Path
import subprocess
import tarfile
import tempfile

with tempfile.TemporaryDirectory(prefix='native-symlink-') as temporary:
    root = Path(temporary)
    archive = root / 'fixture.tar'
    links = {
        'tree/sub/parent': '../payload.txt',
        'tree/sub/deep/ancestor': '../../payload.txt',
        'tree/sub/forward': '../later.txt',
        'tree/sub/directory': '../other',
    }
    files = ['tree/payload.txt', 'tree/later.txt', 'tree/other/child.txt']
    with tarfile.open(archive, 'w') as stream:
        # Links deliberately precede the files to which they refer.
        for name, target in links.items():
            entry = tarfile.TarInfo(name)
            entry.type = tarfile.SYMTYPE
            entry.linkname = target
            entry.mode = 0o777
            entry.mtime = 1700000000
            stream.addfile(entry)
        for name in files:
            data = (name + '\n').encode()
            entry = tarfile.TarInfo(name)
            entry.size = len(data)
            entry.mode = 0o644
            entry.mtime = 1700000000
            stream.addfile(entry, io.BytesIO(data))
    subprocess.run(['tar', 'xf', str(archive)], cwd=root, check=True)
    expected = {
        'tree/sub/parent': files[0],
        'tree/sub/deep/ancestor': files[0],
        'tree/sub/forward': files[1],
        'tree/sub/directory/child.txt': files[2],
    }
    for name, target in expected.items():
        assert (root / name).read_text() == target + '\n', name
        assert (root / name).stat().st_size == len(target) + 1, name
print('NATIVE-SYMLINK-ARCHIVE-PASS', flush=True)
