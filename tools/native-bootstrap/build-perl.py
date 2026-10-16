"""Build the Contrib Perl bootstrap with native LLVM 20.

Run on AROS: python3 build-perl.py ARCHIVE PATCH BUILD_PARENT
Requires the corrected Coreutils pwd and Contrib x86_64 patch. Installs only
the Fcntl/IO bootstrap interpreter, not the complete Perl extension suite.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

assert sys.platform.startswith("aros"), sys.platform
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("archive", type=Path)
parser.add_argument("patch", type=Path)
parser.add_argument("build_parent", type=Path)
args = parser.parse_args()
archive = args.archive.resolve()
patch = args.patch.resolve()
assert hashlib.sha256(archive.read_bytes()).hexdigest() == (
    "dd1901c1bef660374accb959c49e4c8ef600b1915bb7bd31a924597ec2c6fc2f")
sdk = Path("/SYS/Developer")
os.environ["PATH"] = str(sdk / "bin") + ":/SYS/C"
os.environ["TMPDIR"] = "/T"
parent = args.build_parent.resolve()
parent.mkdir(parents=True, exist_ok=True)
source = parent / "perl-5.7.2"


def run(argv, cwd=source):
    print("RUN", *map(str, argv), flush=True)
    subprocess.run(list(map(str, argv)), cwd=cwd, check=True)


# Fresh source avoids silently mixing different patch revisions on retries.
if source.exists():
    raise SystemExit("Choose a fresh build parent; preserve earlier evidence")
run([sdk / "bin/tar", "-xzf", archive, "-C", parent], parent)
run([sdk / "bin/patch", "-p1", "-i", patch])
config = source / "config.sh"
text = config.read_text()
flags = ("-mcmodel=large -mno-red-zone -nix -std=gnu89 "
         "-fno-strict-aliasing -Wno-implicit-function-declaration "
         "-Wno-int-conversion -Wno-compound-token-split-by-macro")
values = {
    "cc": str(sdk / "bin/clang"), "ld": str(sdk / "bin/clang"),
    "ccflags": flags, "ldflags": "-mcmodel=large -mno-red-zone -nix",
    "libs": "-lm", "perllibs": "-lm", "optimize": "-O1",
    "ar": str(sdk / "bin/ar"), "ranlib": str(sdk / "bin/ranlib"),
    "sh": str(sdk / "bin/sh"), "full_csh": "",
    "startsh": "#!/SYS/Developer/bin/sh", "i_time": "define",
    "cppstdin": "/SYS/Developer/bin/clang -E", "static_ext": "Fcntl IO",
}
for key, value in values.items():
    text, count = re.subn("^" + key + "='[^']*'",
                          key + "='" + value + "'", text, flags=re.M)
    assert count == 1, (key, count)
text = text.replace("/SYS/Development", str(sdk)).replace("/usr", str(sdk))
config.write_text(text)
for name in ("config_h.SH", "Makefile.SH", "cflags.SH", "makedepend.SH",
             "writemain.SH", "makedir.SH", "makeaperl.SH", "myconfig.SH",
             "pod/Makefile.SH", "x2p/Makefile.SH", "x2p/cflags.SH"):
    run([sdk / "bin/sh", name])
run([sdk / "bin/make", "SHELL=/SYS/Developer/bin/sh", "-j2", "perl"])
probe = source / "native-bootstrap-check.pl"
probe.write_text('use strict; use IO::Handle; use Getopt::Long; '
                 'print "NATIVE-PERL-BOOTSTRAP-PASS\\n";\n')
result = subprocess.run([str(source / "perl"), "-Ilib", str(probe)],
                        cwd=source, check=True, stdout=subprocess.PIPE)
assert result.stdout == b"NATIVE-PERL-BOOTSTRAP-PASS\n", result.stdout
shutil.copy2(source / "perl", sdk / "bin/perl")
shutil.copytree(source / "lib", sdk / "lib/perl5/5.7.2", dirs_exist_ok=True)
print(result.stdout.decode(), end="", flush=True)
