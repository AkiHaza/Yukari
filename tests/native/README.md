# Native regression tests

Run on Linux (or WSL) with a C++20 compiler and standard Linux headers:

```bash
bash tests/native/run.sh
```

Set `CXX` to choose another compiler and optionally set `CXXFLAGS` or `LDFLAGS`.
The runner compiles into a temporary
directory and removes it on exit; it does not use a JVM or Android device.

The test executable includes the production Binder implementation directly
and supplies no-op log functions plus minimal JNI declarations. Linker wrappers
count native allocations and simulate allocation failures. Tests cover:

- Case-insensitive keyword matching, exact-only `profile`, and malformed String16.
- Four-byte scanning, parcel boundaries, and 20,000 deterministic randomized
  comparisons against the original parser and matching semantics.
- No allocation or pointer replacement for clean replies.
- Filtered reply contents, offsets, buffer growth/reuse, allocation failure,
  active-buffer exclusion, upper size limits and overflow rejection.
- Restoring the original Binder buffer address in `BC_FREE_BUFFER`.
- Releasing cached storage through the pthread key destructor on thread exit.

These tests exercise native logic without a Binder driver. They do not replace
on-device Android/ART integration and cross-thread Parcel lifetime checks.
