#!/usr/bin/env python3
"""Regenerate and compare posixc dispatch vectors against an explicit revision."""
import argparse
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path)
parser.add_argument("genmodule", type=Path)
parser.add_argument("baseline")
parser.add_argument("output", type=Path)
args = parser.parse_args()
source = args.source.resolve()
generator = args.genmodule.resolve()
output = args.output.resolve()
config = "compiler/crt/posixc/posixc.conf"
baseline = subprocess.check_output(
    ["git", "show", args.baseline + ":" + config], cwd=source)
maps = {}
for label, contents in [("baseline", baseline),
                        ("candidate", (source / config).read_bytes())]:
    directory = output / label
    (directory / "linklib").mkdir(parents=True, exist_ok=True)
    (directory / "posixc.conf").write_bytes(contents)
    subprocess.run([str(generator), "-c", str(directory / "posixc.conf"),
                    "-d", str(directory), "-l", str(directory / "linklib"),
                    "writefiles", "posixc", "library"], check=True)
    text = (directory / "posixc_start.c").read_text()
    maps[label] = {name: int(slot) for name, slot in re.findall(
        r"&AROS_SLIB_ENTRY\((.+?),PosixC,(\d+)\)", text)}
old, new = maps["baseline"], maps["candidate"]
if not old:
    raise SystemExit("No baseline dispatch entries parsed; audit invalid")
changes = {name: [slot, new.get(name)] for name, slot in old.items()
           if new.get(name) != slot}
report = {
    "baseline": args.baseline,
    "existing_dispatch_entries": len(old),
    "changed_or_removed": changes,
    "added": {name: slot for name, slot in new.items() if name not in old},
}
result = json.dumps(report, indent=2) + "\n"
(output / "report.json").write_text(result)
print(result, end="")
raise SystemExit(bool(changes))
