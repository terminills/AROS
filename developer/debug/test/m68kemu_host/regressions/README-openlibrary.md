# OpenLibrary version regression

Run from the repository root:

```sh
python3 developer/debug/test/m68kemu_host/regressions/run_openlibrary_versions.py
python3 developer/debug/test/m68kemu_host/regressions/run_openlibrary_versions.py --sanitize
```

This host test needs Python 3.9+ and a C++17 compiler. The sanitized profile
also needs ASan/UBSan. The runner extracts the actual `OpenLibrary` and
`OldOpenLibrary` thunk bodies, plus their shared helper when present, from
`m68kemu_thunks.c`; it does not copy their implementations. Small spies stand
in for native library lookup, BOOPSI class opening, cached-library version
reads, and disk loading. The fake CPU has 32-bit guest registers.

The cases cover null names, native fast returns, class libraries, cached 68k
libraries, cold disk loads, too-new requests, dirty high words in D0, and
`OldOpenLibrary` ignoring D0 entirely. The test also checks tail-call state
and that the thunk leaves the CPU register array unchanged. `--source-root`
selects a different AROS tree; `--build-dir` retains generated files.

This is function-level host validation. It does not establish a full linked
AROS library or target runtime result. Leak detection is disabled by default
in sanitized runs; it can be configured with `ASAN_OPTIONS`.
