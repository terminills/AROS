#!/usr/bin/env python3
"""Run actual driver function bodies with fault-injecting host OS/controller mocks.
This checks source behavior, not the AROS ABI, scheduler, hardware or IRQ routing.
Usage: run-host.py OUTPUT_DIR [SOURCE_DIR]
"""
import pathlib, re, subprocess, sys
here = pathlib.Path(__file__).resolve().parent
src = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv)>2 else here.parent
out = pathlib.Path(sys.argv[1]).resolve(); out.mkdir(parents=True, exist_ok=True)
def function(file, signature):
    text = (src/file).read_text(errors='replace')
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1; end = brace+1
    while depth:
        depth += (text[end]=='{') - (text[end]=='}'); end += 1
    return text[start:end]
code = (here/'startup-host.c.in').read_text()
for key,file,signature in [
    ('START','i8042_common.c','BOOL i8042_start_task('),
    ('CONTROLLER','i8042_kbdclass.c','void i8042_kbd_controller_task('),
]:
    code = code.replace('@'+key+'@', function(file,signature))
(out/'startup-host.c').write_text(code)
cmd=['cc','-std=c11','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(out/'startup-host.c'),'-o',str(out/'startup-host')]
subprocess.run(cmd,check=True)
subprocess.run([str(out/'startup-host')],check=True,timeout=10)
