"""Check hosted rename directory-lock handling through Python and native mv."""
import errno
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.stderr = sys.stdout
assert sys.platform.startswith("aros"), sys.platform
original = os.getcwd()
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    source = root / "source"
    destination = root / "destination"
    source.mkdir()
    destination.mkdir()
    try:
        os.chdir(source)
        for command in ["python", "mv"]:
            for absolute_source in [False, True]:
                for absolute_destination in [False, True]:
                    (source / "item").write_text("native rename\n")
                    src = str(source / "item") if absolute_source else "item"
                    dst = (str(destination / "item") if absolute_destination
                           else "../destination/item")
                    if command == "python":
                        os.rename(src, dst)
                    else:
                        subprocess.run(["/SYS/Developer/bin/mv", src, dst], check=True)
                    assert not (source / "item").exists()
                    assert (destination / "item").read_text() == "native rename\n"
                    (destination / "item").unlink()
                    print("RENAME-PASS", command, absolute_source,
                          absolute_destination, flush=True)
        (source / "item").write_text("keep on failure\n")
        try:
            os.rename("item", str(destination / "missing" / "item"))
        except OSError as error:
            assert error.errno == errno.ENOENT, error
        else:
            raise AssertionError("rename to a missing parent succeeded")
        assert (source / "item").read_text() == "keep on failure\n"
        (source / "item").unlink()
        source.rmdir()
        destination.rmdir()
    finally:
        os.chdir(original)
print("NATIVE-RENAME-PASS", flush=True)
