"""Exercise cached archive unpacking, patching and repeat calls through sh."""
from pathlib import Path
import io
import subprocess
import sys
import tarfile
import tempfile

shell, script = sys.argv[1:3]
with tempfile.TemporaryDirectory(prefix='native-fetch-') as directory:
    root = Path(directory)
    cache = root / 'cache'
    output = root / 'output'
    cache.mkdir()
    payload = b'before\n'
    with tarfile.open(cache / 'sample.tar', 'w') as archive:
        member = tarfile.TarInfo('sample/input.txt')
        member.size = len(payload)
        archive.addfile(member, io.BytesIO(payload))
        member = tarfile.TarInfo('sample/large.bin')
        member.size = 256 * 1024
        archive.addfile(member, io.BytesIO(b'x' * member.size))
    subprocess.run(['gzip', str(cache / 'sample.tar')], check=True)
    (cache / 'sample.patch').write_text('--- a/input.txt\n+++ b/input.txt\n@@ -1 +1 @@\n-before\n+after\n')
    command = [shell, script, '-a', 'sample', '-s', 'tar.gz', '-ao', str(cache),
               '-l', str(cache), '-d', str(output), '-b', str(output),
               '-po', str(cache), '-p', 'sample.patch:sample:-p1']
    for attempt in range(2):
        subprocess.run(command, check=True)
        assert (output / 'sample/input.txt').read_text() == 'after\n'
        assert (output / 'sample/large.bin').read_bytes() == b'x' * (256 * 1024)
        assert not (cache / 'sample.fetch').exists()
    result = subprocess.run([shell, script, '-a', '-f'])
    assert result.returncode != 0
print('FETCH-SHELL-PASS', flush=True)
