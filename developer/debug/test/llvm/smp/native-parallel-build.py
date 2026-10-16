"""Exercise native make jobserver recursion and concurrent C/C++ compile/link.

Retains per-command timing, process IDs and logs. Overlap proves concurrent
compiler jobs, not physical multicore execution in a hosted UP guest.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

assert sys.platform.startswith("aros"), sys.platform
out = Path(os.environ.get("AROS_PARALLEL_OUTPUT", "/SYS/NativeChecks")) / (
    "parallel-" + str(time.time_ns()))
out.mkdir(parents=True)
worker = r'''
import json, subprocess, sys, time
from pathlib import Path
label = sys.argv[1]
with open(label + '.log', 'w') as log:
    cpu = int(label[-1])
    command = 'SYS:Developer/bin/python3 /SYS/Parallel/affinity-child.py ' + str(cpu) + ' ' + ' '.join(sys.argv[2:])
    child = subprocess.Popen(['/SYS/Parallel/affinity-launch', str(cpu), command], stdout=log, stderr=subprocess.STDOUT)
    start = time.monotonic_ns()
    status = child.wait()
    end = time.monotonic_ns()
Path(label + '.json').write_text(json.dumps(dict(
    label=label, pid=child.pid, start=start, end=end, status=status,
    argv=sys.argv[2:])))
print(label, 'exit', status, flush=True)
sys.exit(status)
'''
(out / "worker.py").write_text(worker)
rules = ["SHELL := /SYS/Developer/bin/sh", ".PHONY: all phase",
         "all:", "\t+$(MAKE) -f Makefile phase",
         "phase: app0 app1 app2 app3"]
expected = {}
for index in range(4):
    suffix = "c" if index < 2 else "cpp"
    compiler = "/SYS/Developer/bin/clang" + ("" if index < 2 else "++")
    values = [(n * 17 + index * 23) % 997 for n in range(8192)]
    expected[index] = sum(values)
    helper = "static const int values[] = {" + ",".join(map(str, values)) + "};\n"
    helper += "long compute(void) { long s=0; int n; for(n=0;n<8192;n++) s+=values[n]; return s; }\n"
    (out / ("helper%d.%s" % (index, suffix))).write_text(helper)
    main = "#include <stdio.h>\nlong compute(void);\n"
    if index >= 2:
        main += "#include <vector>\n#include <numeric>\n"
    main += "int main(void) { long value=compute();\n"
    if index >= 2:
        main += "std::vector<long> v{value, 1, 2}; value=std::accumulate(v.begin(),v.end(),0L)-3;\n"
    main += 'printf("BUILD-%d %%ld\\n",value); return value!=%d; }\n' % (index, expected[index])
    (out / ("main%d.%s" % (index, suffix))).write_text(main)
    prefix = "/SYS/Developer/bin/python3 worker.py "
    flags = " -mcmodel=large -mno-red-zone -nix -O1"
    if index >= 2:
        flags += " -std=c++17"
    for unit in ("main", "helper"):
        stem = unit + str(index)
        rules += [stem + ".o: " + stem + "." + suffix,
                  "\t" + prefix + "compile-" + stem + " " + compiler + flags + " -c $< -o $@"]
    rules += ["app%d: main%d.o helper%d.o" % (index, index, index),
              "\t" + prefix + "link-app%d " % index + compiler + flags + " $^ -o $@"]
(out / "Makefile").write_text("\n".join(rules) + "\n")
print("Native parallel build evidence:", out, flush=True)
subprocess.run(["/SYS/Developer/bin/make", "-j4"], cwd=out, check=True)
events = [json.loads(p.read_text()) for p in out.glob("compile-*.json")]
assert len(events) == 8 and all(e["status"] == 0 for e in events), events
points = sorted([(e["start"], 1) for e in events] +
                [(e["end"], -1) for e in events])
active = maximum = 0
for _, delta in points:
    active += delta
    maximum = max(maximum, active)
assert maximum >= 2, ("No overlapping compiler processes measured", events)
for index in range(4):
    result = subprocess.run([str(out / ("app%d" % index))], cwd=out,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, check=True)
    assert result.stdout == "BUILD-%d %d\n" % (index, expected[index]), result.stdout
    print(result.stdout, end="", flush=True)
(out / "result.json").write_text(json.dumps({"max_compile_overlap": maximum,
    "scope": "explicit per-process CPU affinity; check guest CPU probes and host vCPU samples",
    "programs_passed": 4}, indent=2) + "\n")
print("NATIVE-PARALLEL-BUILD-PASS overlap=%d" % maximum, flush=True)
