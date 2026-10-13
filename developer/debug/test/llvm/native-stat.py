"""Check large-file stat metadata through descriptor-relative Python APIs."""
import errno
import os
from pathlib import Path
import sys
import tempfile

assert sys.platform.startswith('aros'), sys.platform
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    nested = root / 'nested'
    nested.mkdir()
    item = nested / 'item'
    item.write_bytes(b'AROS stat64\n')
    fd = os.open(nested, os.O_RDONLY)
    try:
        expected = os.stat(item)
        for follow in [False, True]:
            actual = os.stat('item', dir_fd=fd, follow_symlinks=follow)
            assert actual == expected, (actual, expected)
        try:
            os.stat('missing', dir_fd=fd)
        except OSError as error:
            assert error.errno == errno.ENOENT, error
        else:
            raise AssertionError('stat of missing file succeeded')
    finally:
        os.close(fd)
    # TemporaryDirectory must safely remove nested directories using fstatat.
assert not root.exists()
print('NATIVE-STAT-PASS', flush=True)
