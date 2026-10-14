#include <llvm/Demangle/Demangle.h>
#include <cstdio>
#include <string>
int main() {
 for (int i=0;i<1000;++i) {
  if (llvm::demangle("_Z3fooi") != "foo(int)") return 1;
  if (llvm::demangle("plain_symbol") != "plain_symbol") return 2;
 }
 std::puts("NATIVE-LLVM-DEMANGLE-PASS iterations=1000");
 return 0;
}
