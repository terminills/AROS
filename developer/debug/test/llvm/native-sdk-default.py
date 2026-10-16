"""Native Clang finds its installed SDK without a --sysroot argument."""
from pathlib import Path
import subprocess
import sys
import tempfile

assert sys.platform.startswith('aros'), sys.platform
with tempfile.TemporaryDirectory(prefix='native-sdk-') as directory:
    root = Path(directory)
    for tool, suffix, source in [
        ('clang', 'c', '#include <stdio.h>\nint main(void) { puts("SDK-C-PASS"); return 0; }\n'),
        ('clang++', 'cpp', '#include <vector>\n#include <cstdio>\nint main() { std::vector<int> v{1,2,3}; if(v.size()!=3) return 1; puts("SDK-CXX-PASS"); }\n')]:
        src = root / ('main.' + suffix)
        out = root / tool
        src.write_text(source)
        command = ['/SYS/Developer/bin/' + tool, '-mcmodel=large', '-mno-red-zone', '-nix', str(src), '-o', str(out)]
        subprocess.run(command, check=True)
        subprocess.run([str(out)], check=True)
        trace = subprocess.run(command + ['--sysroot=/SDK-override-probe', '-###'],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=True).stdout
        assert '/SDK-override-probe/include' in trace, trace
        assert '/SDK-override-probe/lib/startup.o' in trace, trace
print('NATIVE-SDK-DEFAULT-PASS', flush=True)
