"""Native LLVM gate using checked subprocesses, independent of shell semantics."""
import os
from pathlib import Path
import subprocess
import sys

sys.stderr = sys.stdout
assert sys.platform.startswith("aros"), sys.platform
work = Path("/T/llvm-python-test")
work.mkdir(exist_ok=True)
os.chdir(work)
bin_dir = "/SYS/Developer/bin/"
flags = ["--sysroot=/SYS/Developer", "-mcmodel=large", "-mno-red-zone", "-nix"]


def run(args, expected=0):
    print("RUN", repr(args), flush=True)
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    print(result.stdout, end="", flush=True)
    if expected is None:
        assert result.returncode != 0, result
    else:
        assert result.returncode == expected, result


def compile(args, cxx=False, failure=False):
    run([bin_dir + ("clang++" if cxx else "clang"), *flags, *args],
        expected=None if failure else 0)


(work / "answer.c").write_text("int answer(void) { return 42; }\n")
(work / "main.c").write_text("extern int answer(void); int main(void) { return answer() != 42; }\n")
compile(["-c", "answer.c", "-o", "answer.o"])
run([bin_dir + "llvm-ar", "rcs", "libanswer.a", "answer.o"])
compile(["main.c", "-L.", "-lanswer", "-o", "native-c"])
assert (work / "native-c").stat().st_size > 0
run([str(work / "native-c")])
print("NATIVE-C-PASS", flush=True)

(work / "include space").mkdir(exist_ok=True)
(work / "include space/value.h").write_text("#define VALUE 23\n")
(work / "response.c").write_text("#include <value.h>\nint main(void) { return VALUE != 23; }\n")
(work / "compile.rsp").write_text('-I"include space" -c response.c -o response.o\n')
compile(["@compile.rsp"])
compile(["response.o", "-o", "native-response"])
run([str(work / "native-response")])
print("NATIVE-RESPONSE-PASS", flush=True)

(work / "main.cpp").write_text('''#include <string>
#include <vector>
static std::string value("native");
int main() { std::vector<int> v{19,23}; return value != "native" || v[0]+v[1] != 42; }
''')
compile(["-std=c++17", "main.cpp", "-o", "native-cxx"], cxx=True)
run([str(work / "native-cxx")])
print("NATIVE-CXX-PASS", flush=True)

(work / "tls.c").write_text('''#include <pthread.h>
static _Thread_local int value = 7;
static void *worker(void *p) {
    int *result = p; *result = value != 7; value = 42; return 0;
}
int main(void) {
    pthread_t t; int result = -1; value = 19;
    if (pthread_create(&t, 0, worker, &result)) return 1;
    if (pthread_join(t, 0)) return 2;
    return result || value != 19;
}
''')
compile(["-std=c11", "-pthread", "tls.c", "-o", "native-tls"])
run([str(work / "native-tls")])
print("NATIVE-TLS-PASS", flush=True)

(work / "invalid.c").write_text("#error EXPECTED_NATIVE_COMPILE_FAILURE\n")
compile(["-c", "invalid.c", "-o", "invalid.o"], failure=True)
(work / "unresolved.c").write_text("extern int missing(void); int main(void) { return missing(); }\n")
compile(["unresolved.c", "-o", "unresolved"], failure=True)
print("NATIVE-TOOLCHAIN-PASS", flush=True)
