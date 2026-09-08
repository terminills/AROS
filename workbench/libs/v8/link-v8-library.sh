#!/bin/bash
# Manual v8.library link recipe (until folded into mmakefile.src).
# Inputs it expects in place:
#  - glue objects:   $B/gen/workbench/libs/v8/v8/*.o   (built by 'make
#    workbench-libs-v8' up to its link step, plus glibc15-stubs-aros.o
#    and the external/electron/v8 support objects, cross-compiled with
#    -std=c++20 -fno-pic -fno-PIE -DV8_USE_REAL_ENGINE)
#  - engine archive: $B/gen/workbench/libs/v8/linklibs/libv8monolith.a
#    (ninja v8_monolith from the pinned 12.4.254 tree; build/config
#    carries -fno-stack-protector and the PkeyAlloc disable)
#  - libstdc++-aros.a: host gcc-15 libstdc++.a with
#    _ZNSt6locale5facet15_S_get_c_localeEv weakened (objcopy
#    --weaken-symbol) so the glue's strong override wins.
B=/home/terminills/Documents/GitHub/aros-opal-pc-x86_64-smp/bin/pc-x86_64-smp
CT=/home/terminills/Documents/GitHub/aros-crosstools
$CT/x86_64-aros-g++ --sysroot $B/AROS/Developer \
  -m64 -mcmodel=large -mno-red-zone -mno-ms-bitfields -nostartfiles \
  $B/gen/compiler/libinit/libentry.o \
  $B/gen/workbench/libs/v8/v8/*.o \
  -static-libstdc++ -Wl,--no-relax \
  -L$B/gen/workbench/libs/v8/linklibs \
  -Wl,--whole-archive -lv8monolith -Wl,--no-whole-archive \
  -o $B/AROS/Libs/v8.library \
  -lstdcio -lstdc -lposixc -lpthread -lm -latomic \
  $B/gen/workbench/libs/v8/linklibs/libstdc++-aros.a
