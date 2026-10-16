#!/usr/bin/env python3
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
"""Generate an eight-file native compiler regression in a NEW output directory.

Usage: parallel-check.py --output /tmp/parallel-check [--cc gcc]
After this command succeeds, pack output/cd as a Rock Ridge ISO named PAR8.
On AROS with Development: installed, run Execute PAR8:check. Each clean/build/run
must return zero at -j1, -j2, -j4 and -j8, with the expected 64-bit result.
The script stages files in RAM:Parallel8; use a disposable validation session.

Python modulo-2**64 arithmetic computes the oracle independently of C execution.
The host compiler checks it before the guest runner is written. This is a
parallel-build correctness test, not a CPU utilization or throughput benchmark.
"""
import argparse
from pathlib import Path
import subprocess
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cc', default='gcc')
args=parser.parse_args()
p=args.output
p.mkdir(parents=True, exist_ok=False)
d=p/'cd/Parallel8'; d.mkdir(parents=True)
mask=(1<<64)-1
seed=0x123456789abcdef0
expected=0
for unit in range(8):
 s='#include <stdint.h>\n'
 for f in range(16):
  k=unit*16+f+1
  s+=f'__attribute__((noinline)) static uint64_t f{f}(uint64_t x) {{\n'
  x=(seed+k)&mask
  for j in range(24):
   n=((k+j)%31)+1;c=(0x9e3779b97f4a7c15*(k+j+1))&mask
   s+=f' x = (x ^ (x >> {n})) * UINT64_C({c});\n'
   x=((x^(x>>n))*c)&mask
  s+=' return x;\n}\n';expected=(expected+x)&mask
 s+=f'uint64_t unit{unit}(uint64_t x) {{ return '+ ' + '.join(f'f{f}(x+{unit*16+f+1})' for f in range(16))+'; }\n'
 if unit==0:
  s+='#include <stdio.h>\n'
  s+=''.join(f'extern uint64_t unit{i}(uint64_t);\n' for i in range(1,8))
  # Main appended after expected from all units is known.
 (d/f'unit{unit}.c').write_text(s)
s=(d/'unit0.c').read_text();s+=f'int main(void) {{ uint64_t x=UINT64_C({seed}); uint64_t got='+ '+'.join(f'unit{i}(x)' for i in range(8))+f'; printf("PARALLEL8 got=%016llx expected=%016llx\\n",(unsigned long long)got,(unsigned long long)UINT64_C({expected})); return got != UINT64_C({expected}); }}\n';(d/'unit0.c').write_text(s)
objects=' '.join(f'unit{i}.o' for i in range(8))
(d/'Makefile').write_text('SHELL := /Development/bin/sh\nCC := /Development/bin/clang\nSDKFLAGS := --sysroot=/Development -B/Development/bin -mcmodel=large -mno-red-zone -nix\n.PHONY: all run clean\nall: Parallel8\nParallel8: '+objects+'\n\t$(CC) $(SDKFLAGS) -Wl,--ld-path=/Development/bin/ld $^ -o $@\n%.o: %.c\n\t$(CC) $(SDKFLAGS) -std=c11 -O2 -Wall -Wextra -c $< -o $@\nrun: Parallel8\n\t./Parallel8\nclean:\n\t/Development/bin/rm -f '+objects+' Parallel8\n')
subprocess.run([args.cc,'-std=c11','-O2','-Wall','-Wextra',*[str(d/f'unit{i}.c') for i in range(8)],'-o',str(p/'host-reference')],check=True)
r=subprocess.run([str(p/'host-reference')],capture_output=True,text=True,check=True);(p/'host-reference.log').write_text(r.stdout);print(r.stdout)
s='FailAt 21\nStack 40960\nCopy PAR8:Parallel8 RAM:Parallel8 ALL QUIET\nCD RAM:Parallel8\n'
for j in [1,2,4,8]:
 for action,cmd in [('CLEAN','clean'),('BUILD',f'-j{j} all'),('RUN','run')]:
  s+=f'Development:bin/make {cmd} >SER:\nEcho "PAR8_J{j}_{action}_RC=$RC" >SER:\n'
s+='Echo PAR8_COMPLETE >SER:\n';(p/'cd/check').write_text(s)
