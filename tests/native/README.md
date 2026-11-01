# Native regression tests

Run on Linux (or WSL) with a C++20 compiler, Linux headers, CMake 3.22.1+,
make, binutils (`readelf`, `strip`), and a JDK with JNI headers (JDK 17+):

```bash
bash tests/native/run.sh
```

Set `CXX` to choose another compiler and optionally set `CXXFLAGS` or `LDFLAGS`.
The runner detects JNI headers from `JAVA_HOME` or `javac`. It compiles into
temporary directories and removes them on exit; it does not start a JVM or use
an Android device.

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
- Request interface/header validation, immutable caller Parcels and command
  streams, private-copy failures, partial writes, retries and nested Binder
  replies (including failed/frozen synchronous calls).
- Package targets covering private processes without prefix overmatching,
  exact process rules, protected packages and disabled configuration.
- Cache cleanup with failure injection at 31 JNI call sites, caller exception
  preservation, string lifetime and local reference/frame cleanup.
- Exact feature matching and bounded reply provenance. A second test uses real
  JNI declarations with a fake function table to verify private requests,
  RemoteException preservation, scoped enumeration and Parcel reuse cleanup.
- Resource matching and the actual resource hook code under fake JNI: dynamic
  package IDs, unrelated `0x3f`, manager/loader changes, original exceptions,
  missing mappings and legacy/modern failure return values.
- ELF allowlist acceptance/rejection, and a Release CMake build of every
  production source using real JNI headers plus an Android logging shim.
  The linked library must expose only the Zygisk entry and have no `.symtab`.

These tests exercise native logic without a Binder driver. They do not replace
on-device Android/ART integration and cross-thread Parcel lifetime checks.
The host build is not an NDK ABI or Android device test. Follow
[`tests/device/README.md`](../device/README.md) for device acceptance.

To run the logic tests under UBSan:

```bash
CXXFLAGS='-fsanitize=undefined -fno-sanitize-recover=all' \
LDFLAGS='-fsanitize=undefined' bash tests/native/run.sh
```
