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
code = (here/'reset-host.c.in').read_text()
defs = '\n'.join(line for name in ['i8042_common.h','i8042_kbd.h']
    for line in (src/name).read_text(errors='replace').splitlines()
    if re.match(r'#define KBD_(OUTCMD|REPLY|MODE|CTRLCMD)_',line))
code=code.replace('@DEFS@',defs).replace('@RESET@',function('i8042_kbdclass.c','static int i8042_kbd_reset(struct i8042base *i8042Base'))
(out/'reset-host.c').write_text(code)
subprocess.run(['cc','-std=c11','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(out/'reset-host.c'),'-o',str(out/'reset-host')],check=True)
subprocess.run([str(out/'reset-host')],check=True,timeout=10)
code=(here/'acpi-host.c.in').read_text()
for key,name in [('ACPI','i8042_acpi.c'),('CLASSINIT','i8042_classinit.c')]:
    text=(src/name).read_text()
    text=re.sub(r'^#include[^\n]*','',text,flags=re.M)
    code=code.replace('@'+key+'@',text)
(out/'acpi-host.c').write_text(code)
subprocess.run(['cc','-std=c11','-g','-O1','-fsanitize=address,undefined',str(out/'acpi-host.c'),'-o',str(out/'acpi-host')],check=True)
subprocess.run([str(out/'acpi-host')],check=True,timeout=10)
code=(here/'key-host.c.in').read_text()
root=src.parents[3]
raw=(root/'compiler/include/devices/rawkeycodes.h').read_text(errors='replace')
defs='\n'.join(line for name in ['i8042_common.h','keys.h','i8042_kbdclass.c']
    for line in (src/name).read_text(errors='replace').splitlines()
    if re.match(r'#define\s+(K_|KBD_REPLY_|FLAG_)',line))
code=code.replace('@DEFS@',raw+'\n'+defs)
code=code.replace('@TABLES@','\n'.join((src/name).read_text(errors='replace').replace('__attribute__((section(".text")))','') for name in ['stdkeytable.h','e0keytable.h']))
code=code.replace('@KEY@',function('i8042_kbdclass.c','static void i8042_kbd_process_irq_key(struct kbd_data *data'))
(out/'key-host.c').write_text(code)
subprocess.run(['cc','-std=c11','-g','-O1','-fsanitize=address,undefined',str(out/'key-host.c'),'-o',str(out/'key-host')],check=True)
subprocess.run([str(out/'key-host')],check=True,timeout=10)
