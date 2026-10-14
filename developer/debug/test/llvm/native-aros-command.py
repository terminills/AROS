"""Build and run the stock AROS Which command with the installed native SDK.

This is a focused source/SDK linking check, not the full system make target.
Keep the resulting object and executable for ELF/symbol inspection.
"""
from pathlib import Path
import subprocess
import sys
assert sys.platform.startswith('aros'), sys.platform
out = Path('/SYS/NativeChecks/aros-command')
out.mkdir(parents=True, exist_ok=True)
source = '/SYS/Sources/AROS/workbench/c/Which.c'
compiler = '/SYS/Developer/bin/clang'
flags = ['-mcmodel=large', '-mno-red-zone', '-nix']
print('Compiling AROS workbench/c/Which.c with native Clang', flush=True)
subprocess.run([compiler, *flags, '-c', source, '-o', str(out/'Which.o')], check=True)
print('Linking AROS NativeChecks/aros-command/Which', flush=True)
subprocess.run([compiler, *flags, str(out/'Which.o'), '-o', str(out/'Which')], check=True)
print('Running the newly linked AROS Which command', flush=True)
result = subprocess.run([str(out/'Which'), 'SYS:C/Version'], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True)
print(result.stdout, end='', flush=True)
assert result.returncode == 0, result.returncode
assert 'Version' in result.stdout, result.stdout
print('NATIVE-AROS-COMMAND-PASS', flush=True)
