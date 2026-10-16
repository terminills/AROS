#!/bin/sh
# Run INSIDE AROS. A successful cross-build is not a native-toolchain pass.
set -eu

CC=${CC:-clang}
CXX=${CXX:-clang++}
AR=${AR:-llvm-ar}
work=${1:-/T/llvm-native-test}
mkdir -p "$work"
cd "$work"

"$CC" --version
"$AR" --version

cat > answer.c <<'EOF'
int answer(void) { return 42; }
EOF
cat > main.c <<'EOF'
#include <stdio.h>
extern int answer(void);
int main(void) {
    if (answer() != 42) return 1;
    puts("NATIVE-C-PASS");
    return 0;
}
EOF

# Deliberately exercise separate compilation, archiving, and driver spawning
# of collect-aros/the linker, followed by loading the resulting executable.
"$CC" ${CFLAGS:-} -c answer.c -o answer.o
"$AR" rcs libanswer.a answer.o
"$CC" ${CFLAGS:-} main.c -L. -lanswer ${LDFLAGS:-} -o native-c
./native-c

# LLVM's argument parsing and AROS filename handling must preserve spaces.
mkdir -p 'include space'
printf '#define RESPONSE_VALUE 23\n' > 'include space/value.h'
cat > response.c <<'EOF'
#include <value.h>
int main(void) { return RESPONSE_VALUE != 23; }
EOF
printf '%s\n' '-I"include space"' '-c' 'response.c' '-o' 'response.o' > compile.rsp
"$CC" ${CFLAGS:-} @compile.rsp
"$CC" ${CFLAGS:-} response.o ${LDFLAGS:-} -o native-response
./native-response

cat > main.cpp <<'EOF'
#include <string>
#include <vector>
static std::string initialized("native");
int main() {
    std::vector<int> values{19, 23};
    return initialized != "native" || values[0] + values[1] != 42;
}
EOF
"$CXX" ${CXXFLAGS:-} -std=c++17 main.cpp ${LDFLAGS:-} -o native-cxx
./native-cxx

# Emulated TLS must provide different storage to two live AROS tasks.
cat > tls.c <<'EOF'
#include <pthread.h>
static _Thread_local int value = 7;
static void *worker(void *arg) {
    int *result = arg;
    if (value != 7) { *result = 1; return 0; }
    value = 42;
    *result = value != 42;
    return 0;
}
int main(void) {
    pthread_t thread;
    int result = -1;
    value = 19;
    if (pthread_create(&thread, 0, worker, &result)) return 1;
    if (pthread_join(thread, 0)) return 2;
    return result || value != 19;
}
EOF
"$CC" ${CFLAGS:-} -std=c11 -pthread tls.c ${LDFLAGS:-} -o native-tls
./native-tls

# A failed child must propagate failure through the compiler driver and shell.
printf '%s\n' '#error EXPECTED_NATIVE_COMPILE_FAILURE' > invalid.c
if "$CC" ${CFLAGS:-} -c invalid.c -o invalid.o; then
    echo 'FAIL: compiler accepted #error' >&2
    exit 1
fi
cat > unresolved.c <<'EOF'
extern int missing_native_test_symbol(void);
int main(void) { return missing_native_test_symbol(); }
EOF
if "$CC" ${CFLAGS:-} unresolved.c ${LDFLAGS:-} -o unresolved; then
    echo 'FAIL: unresolved symbol link returned success' >&2
    exit 1
fi

echo 'NATIVE-TOOLCHAIN-PASS'
