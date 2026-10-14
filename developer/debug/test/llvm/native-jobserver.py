"""Check a global job ceiling across three recursive make levels."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

root = Path(os.environ.get('AROS_JOBSERVER_OUTPUT', '/SYS/NativeChecks/jobserver'))
root.mkdir(parents=True, exist_ok=True)
make = sys.argv[1]
baseline = len(sys.argv) > 2 and sys.argv[2] == 'baseline'
worker = '''import json,os,sys,time
from pathlib import Path
name=sys.argv[1]
start=time.monotonic_ns()
time.sleep(.5)
status=7 if os.environ.get('FAIL_JOB')==name else 0
Path(name+'.json').write_text(json.dumps(dict(name=name,start=start,end=time.monotonic_ns(),status=status,flags=os.environ.get('MAKEFLAGS',''))))
sys.exit(status)
'''
results = []
cases = [(4, False)] if baseline else [(1, False), (2, False), (4, False), (4, True), (4, False)]
for index, (jobs, fail) in enumerate(cases):
    work = root / ('run-%s-%d-%d' % ('baseline' if baseline else 'candidate', time.time_ns(), index))
    work.mkdir()
    (work / 'worker.py').write_text(worker)
    (work / 'Makefile').write_text('''SHELL := /SYS/Developer/bin/sh
.PHONY: all g0 g1 g2
all: g0 g1 g2
g0 g1 g2:
\t+$(MAKE) -f branch.mk GROUP=$@
''')
    (work / 'branch.mk').write_text('''SHELL := /SYS/Developer/bin/sh
.PHONY: all h0 h1
all: h0 h1
h0 h1:
\t+$(MAKE) -f leaf.mk GROUP=$(GROUP) SUB=$@
''')
    (work / 'leaf.mk').write_text('''SHELL := /SYS/Developer/bin/sh
.PHONY: all w0 w1
all: w0 w1
w0 w1:
\t/SYS/Developer/bin/python3 worker.py $(GROUP)-$(SUB)-$@
''')
    env = os.environ.copy()
    env.pop('MAKEFLAGS', None)
    env.pop('MFLAGS', None)
    env.pop('MAKELEVEL', None)
    env['FAIL_JOB'] = 'g0-h0-w0' if fail else ''
    with (work / 'make.log').open('w') as log:
        result = subprocess.run([make, '-j%d' % jobs], cwd=work, env=env,
                                stdout=log, stderr=subprocess.STDOUT)
    events = [json.loads(p.read_text()) for p in work.glob('*.json')]
    points = sorted([(e['start'], 1) for e in events] + [(e['end'], -1) for e in events])
    active = maximum = 0
    for _, delta in points:
        active += delta
        maximum = max(maximum, active)
    row = dict(jobs=jobs,fail=fail,status=result.returncode,workers=len(events),
               max_overlap=maximum,directory=str(work))
    results.append(row)
    print(row, flush=True)
    (root / ('baseline-results.json' if baseline else 'candidate-results.json')).write_text(json.dumps(results, indent=2))
    assert (result.returncode != 0) == fail, row
    assert 'INTERNAL:' not in (work / 'make.log').read_text(), (work / 'make.log').read_text()
    if not fail:
        assert len(events) == 12 and all(e['status'] == 0 for e in events), row
    if not baseline:
        assert 1 <= maximum <= jobs, row
        if jobs > 1 and not fail:
            assert maximum >= 2, row
print('NATIVE-JOBSERVER-' + ('BASELINE-MEASURED' if baseline else 'PASS'), flush=True)
