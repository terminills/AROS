"""Check native GNU Make environment import, recursive make and failure status."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

assert sys.platform.startswith("aros"), sys.platform
with tempfile.TemporaryDirectory() as temporary:
    path = Path(temporary) / "Makefile"
    path.write_text('''SHELL := /SYS/Developer/bin/sh
export ADT_MAKE_EXPORT := recipe-export
all:
\t@test "$(ADT_ENV_PROBE)" = inherited
\t@echo MAKE-ENV-PASS
\t@$(MAKE) -f $(firstword $(MAKEFILE_LIST)) child
child:
\t@test "$(ADT_MAKE_EXPORT)" = recipe-export
\t@test "$$ADT_MAKE_EXPORT" = recipe-export
\t@test "$(ADT_ENV_PROBE)" = inherited
\t@echo MAKE-CHILD-PASS
fail:
\t@exit 7
''')
    # SetClock LOAD has whole-second precision; allow its rounding error.
    assert abs(path.stat().st_mtime - time.time()) < 2, "Load the guest clock"
    time.sleep(2)
    env = os.environ.copy()
    env["ADT_ENV_PROBE"] = "inherited"
    command = ["/SYS/Developer/bin/make", "-f", str(path)]
    result = subprocess.run(command, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end="", flush=True)
    assert result.returncode == 0, result.returncode
    assert "MAKE-ENV-PASS" in result.stdout
    assert "MAKE-CHILD-PASS" in result.stdout
    assert "Clock skew" not in result.stdout, "Load the guest clock before builds"
    result = subprocess.run(command + ["fail"], env=env)
    assert result.returncode != 0, "make swallowed a failing recipe"
print("NATIVE-MAKE-PASS", flush=True)
