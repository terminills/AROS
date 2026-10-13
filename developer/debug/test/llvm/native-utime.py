"""Check timestamp round trips across winter, summer and a leap day."""
from pathlib import Path
import os
import tempfile

with tempfile.TemporaryDirectory(prefix='native-utime-') as directory:
    path = Path(directory) / 'timestamp.txt'
    path.write_text('timestamp probe\n')
    for stamp in [1700000000, 1710000000, 1750000000, 1709164800] * 2:
        os.utime(path, (stamp, stamp))
        actual = path.stat().st_mtime
        assert actual == stamp, (stamp, actual)
print('UTIME-ROUNDTRIP-PASS', flush=True)
