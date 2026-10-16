#!/usr/bin/env python3
"""Check nested Python process status under concurrent subprocess launches.

Run with the native interpreter: python3 native-python-thread-exit.py WORKDIR
WORKDIR must be writable. Each of three fresh workers launches 32 children.
"""
import concurrent.futures
import json
from pathlib import Path
import subprocess
import sys


def worker(number):
    for repeat in range(8):
        expected = 7 if repeat % 2 else 0
        code = "import time; time.sleep(0.02); print('CHILD-%d'); raise SystemExit(%d)" % (expected, expected)
        child = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
        assert child.returncode == expected, (number, repeat, child.returncode, expected, child.stderr)
        assert child.stdout.strip() == "CHILD-%d" % expected, (number, repeat, child.stdout)
    return number


if len(sys.argv) == 2 and sys.argv[1] == "--worker":
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        assert sorted(pool.map(worker, range(4))) == list(range(4))
    print("PYTHON-THREADED-WORKER-PASS children=32", flush=True)
else:
    if len(sys.argv) != 2:
        raise SystemExit("usage: native-python-thread-exit.py WORKDIR")
    root = Path(sys.argv[1])
    root.mkdir(parents=True, exist_ok=True)
    rows = []
    for repeat in range(3):
        log = root / ("worker-%d.log" % repeat)
        with log.open("w") as output:
            child = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--worker"], stdout=output, stderr=subprocess.STDOUT)
        text = log.read_text()
        rows.append({"repeat": repeat, "returncode": child.returncode, "log": str(log)})
        print(json.dumps(rows[-1]), flush=True)
        assert child.returncode == 0 and "PYTHON-THREADED-WORKER-PASS children=32" in text, text
    print("NATIVE-PYTHON-THREAD-EXIT-PASS workers=12 children=96", flush=True)
