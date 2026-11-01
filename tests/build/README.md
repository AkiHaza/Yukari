# Build infrastructure regression tests

Run `bash tests/build/run.sh` on Linux/WSL with Bash, Git, Python 3.9+, a C++20
compiler and binutils. The tests use isolated temporary projects and clean them
on exit. They do not download an SDK or Gradle distribution and do not accept
real SDK licenses.

- SDK fixtures check explicit package selection, roots containing spaces,
  version attributes, installation errors, metadata, missing/corrupt packages,
  license SIGPIPE handling and GitHub environment output.
- Package fixtures check library selection, ZIP paths and CRC, module metadata,
  atomic output replacement and preserving the original file after failure.
  Their host ELF has only the machine header changed to exercise validation;
  this fixture does not prove Android ABI compatibility.
- Coordinator fixtures check phase order, failure propagation, checkout SHA
  validation, JDK selection and staged CI versions without changing source metadata.

Real Android validation requires the SDK/NDK path in `scripts/build.sh android`;
host fixture results do not imply that Gradle, the NDK or a device passed.
