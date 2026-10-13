"""Build-shell behavior needed before trusting configure or make results."""
import os
import subprocess
import sys

sys.stderr = sys.stdout
assert sys.platform.startswith("aros"), sys.platform
os.environ["AROS_SHELL_INHERITED"] = "value with spaces"
tests = [
    ("/SYS/Developer/bin/false; exit $?", 1, ""),
    ("set -e; /SYS/Developer/bin/false; echo BAD", 1, ""),
    ("set -e; false; echo BAD", 1, ""),
    ("set -e; /SYS/Developer/bin/true; echo GOOD", 0, "GOOD\n"),
    ('printf "%s\\n" "$AROS_SHELL_INHERITED"', 0, "value with spaces\n"),
    ("if /SYS/Developer/bin/false; then echo BAD; else echo GOOD; fi", 0, "GOOD\n"),
    ("set -n; echo BAD", 0, ""),
    ("exit 7", 7, ""),
    ("trap 'echo CLEANUP' 0; exit 7", 7, "CLEANUP\n"),
    ("f() { return $1; }; g() { set +e; f $1; exit $1; }; g 7", 7, ""),
    ("printf 'flex 2.6.4\\n' | /SYS/Developer/bin/cut -d' ' -f2", 0, "2.6.4\n"),
    ("{ echo START && printf 'one\\ntwo\\n' | /SYS/Developer/bin/sed 's/$/!/' "
     "&& echo END; } > /T/native-shell-order; "
     "/SYS/Developer/bin/cat /T/native-shell-order",
     0, "START\none!\ntwo!\nEND\n"),
]
tests += [
    ('x="`printf \\"%s\\\\n\\" hello`"; printf "%s\\n" "$x"', 0, 'hello\n'),
    ('x="`printf clang20 | /SYS/Developer/bin/sed -e \\"s/clang//g\\"`"; printf "%s\\n" "$x"', 0, '20\n'),
    ('x=$(printf \'flex 2.6.4\\n\' | /SYS/Developer/bin/cut -d \' \' -f2); printf \'%s\\n\' "$x"', 0, '2.6.4\n'),
]
for command, status, output in tests:
    result = subprocess.run(["/SYS/Developer/bin/sh", "-c", command],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(repr(command), result.returncode, repr(result.stdout), flush=True)
    assert result.returncode == status and result.stdout == output, result
result = subprocess.run(["/SYS/Developer/bin/sh", "-c", "set -u; echo $AROS_MISSING_TEST_VAR"],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
assert result.returncode != 0, result
result = subprocess.run(["/SYS/Developer/bin/cut", "-d ", "-f2"],
                        input="flex 2.6.4\n", stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True)
assert result.returncode == 0 and result.stdout == "2.6.4\n", result
print("NATIVE-SHELL-PASS", flush=True)
