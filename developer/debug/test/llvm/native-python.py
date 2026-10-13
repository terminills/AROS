"""Run inside AROS after installing the bootstrap interpreter and its Lib tree."""
import os
import pathlib
import pickle
import subprocess
import sys
import sysconfig
import tempfile
import threading
import time

sys.stderr = sys.stdout
print("PYTHON-EXECUTABLE", sys.executable, flush=True)
assert sys.platform.startswith("aros"), sys.platform
assert sysconfig.get_config_var("MACHDEP") == "aros"
assert sysconfig.get_path("purelib").endswith("/lib/python3.12/site-packages")
print("PYTHON-SYSCONFIG-PASS", flush=True)
assert pickle.loads(pickle.dumps({"value": [1, 2, 3]})) == {"value": [1, 2, 3]}
with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory) / "path with spaces.txt"
    path.write_text("AROS Python bootstrap\n")
    assert path.read_text() == "AROS Python bootstrap\n"
print("PYTHON-FILES-PASS", flush=True)

values = []
thread = threading.Thread(target=lambda: values.append(42))
thread.start()
thread.join(timeout=10)
assert not thread.is_alive() and values == [42]
start = time.monotonic()
time.sleep(0.01)
assert time.monotonic() >= start
print("PYTHON-THREADS-PASS", flush=True)

for _ in range(10):
    child = subprocess.run(
        [sys.executable, "-c", "print('CHILD-PASS')"],
        capture_output=True, text=True, check=True,
    )
    assert child.stdout.strip() == "CHILD-PASS", child
failed = subprocess.run([sys.executable, "-c", "raise SystemExit(7)"])
assert failed.returncode == 7, failed
try:
    subprocess.run([sys.executable, "-c", "pass"], preexec_fn=lambda: None)
except ValueError:
    pass
else:
    raise AssertionError("unsafe preexec_fn unexpectedly accepted")
print("PYTHON-SPAWN-PASS", flush=True)

# ADT configure requires both packages; merely starting Python is insufficient.
import mako.template
import yaml

assert mako.template.Template("${value + 1}").render(value=41) == "42"
assert yaml.safe_load("answer: 42") == {"answer": 42}
print("NATIVE-PYTHON-PASS", flush=True)
