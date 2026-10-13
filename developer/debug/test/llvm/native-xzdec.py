"""Check the standalone XZ decoder and tar's explicit decoder support."""
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

# Python tarfile archive containing payload.bin = 256 KiB of x, XZ/CRC64.
ARCHIVE = bytes.fromhex(
    'fd377a585a000004e6d6b4460200210116000000742fe5a3e40fff00a25d0038184b99750f46086f'
    'a91a79e48c18665890620e2265bbf02e35560d75d14d914f891353651217d4bfbe2131a086c4a16f'
    'bf09415c345bf8568fc8761c0f060831f77dcd95066ff11974b79d9dda359f8a95e6325248ed797b'
    'd6875078e500bdae9188f81739089859c6c5f0138cdb831820379ece10842e59819cc2fdc54c0bd4'
    '368accbdeeda6888dd56657e48f4d07456839c046eccba1043d2f7be98a84d000000000098337743'
    'b2b0f4740001be0180a01000627616a0b1c467fb020000000004595a'
)
decoder = sys.argv[1] if len(sys.argv) > 1 else 'xzdec'
with tempfile.TemporaryDirectory(prefix='native-xzdec-') as directory:
    root = Path(directory)
    archive = root / 'sample.tar.xz'
    archive.write_bytes(ARCHIVE)
    subprocess.run(['tar', '-xf', str(archive), '-I', decoder], cwd=root, check=True)
    assert (root / 'payload.bin').read_bytes() == b'x' * (256 * 1024)
    archive.write_bytes(ARCHIVE[:-8])
    result = subprocess.run(shlex.split(decoder) + [str(archive)], stdout=subprocess.DEVNULL)
    assert result.returncode != 0
print('NATIVE-XZDEC-PASS', flush=True)
