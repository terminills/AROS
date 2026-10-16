"""Run queued build commands inside an isolated AROS bootstrap guest.

Usage: python3 native-runner.py /SYS/NativeJobs
Publish request.json atomically with argv (list), cwd, and optional env (dict).
The runner captures output in output.log and records the child status in
result.json. Submit one request at a time; retain results before the next one.
An empty argv stops the runner. This uses the hosted guest's private filesystem,
not a network service.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

assert sys.platform.startswith("aros"), sys.platform
directory = Path(sys.argv[1])
directory.mkdir(parents=True, exist_ok=True)
request = directory / "request.json"
processing = directory / "processing.json"
if processing.exists():
    raise SystemExit("Unfinished request exists; inspect it before restarting")
(directory / "ready").write_text("AROS native runner ready\n")
while True:
    if not request.exists():
        time.sleep(1)
        continue
    request.rename(processing)
    task = json.loads(processing.read_text())
    if not task["argv"]:
        processing.unlink()
        break
    result = {"argv": task["argv"], "cwd": task["cwd"]}
    try:
        env = os.environ.copy()
        env.update(task.get("env", {}))
        with (directory / "output.log").open("w") as log:
            child = subprocess.run(task["argv"], cwd=task["cwd"], env=env,
                                   stdout=log, stderr=subprocess.STDOUT)
        result["returncode"] = child.returncode
    except Exception as error:
        result["error"] = repr(error)
    temporary = directory / "result.tmp"
    temporary.write_text(json.dumps(result, indent=2) + "\n")
    temporary.replace(directory / "result.json")
    processing.unlink()
(directory / "ready").unlink()
