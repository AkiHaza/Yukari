#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf -- "$WORK_DIR"' EXIT
unset YUKARI_MODULE_VERSION YUKARI_MODULE_VERSION_CODE
FIXTURE="$WORK_DIR/source with spaces"
mkdir -p "$FIXTURE/scripts" "$FIXTURE/module"
cp -- "$ROOT_DIR/scripts/package.sh" "$ROOT_DIR/scripts/verify_package.sh" \
  "$ROOT_DIR/scripts/verify_elf.sh" "$FIXTURE/scripts/"
for file in module.prop config.json post-fs-data.sh service.sh action.sh customize.sh; do
  cp -- "$ROOT_DIR/module/$file" "$FIXTURE/module/$file"
done
cp -- "$FIXTURE/module/module.prop" "$WORK_DIR/original module.prop"

cat > "$WORK_DIR/native fixture.cpp" <<'CPP'
extern "C" __attribute__((visibility("default"))) void zygisk_module_entry() {}
CPP
"${CXX:-g++}" -shared -fPIC -fvisibility=hidden "$WORK_DIR/native fixture.cpp" \
  -o "$WORK_DIR/host library.so"
# This fixture tests ZIP structure and verify_elf's machine/export checks only.
# It is not an Android binary and does not replace a real NDK build.
python3 - "$WORK_DIR/host library.so" "$WORK_DIR/arm64 fixture.so" <<'PY'
import pathlib
import struct
import sys
data = bytearray(pathlib.Path(sys.argv[1]).read_bytes())
assert data[:6] == b"\x7fELF\x02\x01"
struct.pack_into("<H", data, 18, 183)
pathlib.Path(sys.argv[2]).write_bytes(data)
PY

fail() { echo "package test failed: $*" >&2; exit 1; }
expect_failure() {
  if "$@" >"$WORK_DIR/expected failure.log" 2>&1; then
    fail "command unexpectedly succeeded: $*"
  fi
}
no_staging_files() {
  local directory=$1
  local leftovers
  shopt -s nullglob
  leftovers=("$directory"/.yukari-package.*)
  shopt -u nullglob
  ((${#leftovers[@]} == 0)) || fail "staging files left in $directory"
}

OUTPUT="$WORK_DIR/output with spaces/custom package.zip"
bash "$FIXTURE/scripts/package.sh" --library "$WORK_DIR/arm64 fixture.so" --output "$OUTPUT"
bash "$FIXTURE/scripts/verify_package.sh" "$OUTPUT"
no_staging_files "$(dirname -- "$OUTPUT")"
cp -- "$OUTPUT" "$WORK_DIR/previous.zip"
expect_failure bash "$FIXTURE/scripts/package.sh" --library "$WORK_DIR/missing.so" --output "$OUTPUT"
cmp -- "$OUTPUT" "$WORK_DIR/previous.zip"
expect_failure bash "$FIXTURE/scripts/package.sh" --library "$WORK_DIR/host library.so" --output "$OUTPUT"
cmp -- "$OUTPUT" "$WORK_DIR/previous.zip"
mv -- "$FIXTURE/module/action.sh" "$FIXTURE/module/action.sh.saved"
expect_failure bash "$FIXTURE/scripts/package.sh" --library "$WORK_DIR/arm64 fixture.so" --output "$OUTPUT"
cmp -- "$OUTPUT" "$WORK_DIR/previous.zip"
mv -- "$FIXTURE/module/action.sh.saved" "$FIXTURE/module/action.sh"
no_staging_files "$(dirname -- "$OUTPUT")"
expect_failure bash "$FIXTURE/scripts/package.sh" --library
expect_failure bash "$FIXTURE/scripts/package.sh" --unknown

# The default search must select stripped before merged/CXX artifacts and must
# leave any pre-existing out/Yukari directory alone.
STRIPPED="$FIXTURE/module/build/intermediates/stripped_native_libs/release/stripReleaseDebugSymbols/out/lib/arm64-v8a"
MERGED="$FIXTURE/module/build/intermediates/merged_native_libs/release/mergeReleaseNativeLibs/out/lib/arm64-v8a"
CXX_DIR="$FIXTURE/module/build/intermediates/cxx/Release/fixture/obj/arm64-v8a"
mkdir -p "$STRIPPED" "$MERGED" "$CXX_DIR" "$FIXTURE/out/Yukari"
printf 'preserved\n' > "$FIXTURE/out/Yukari/keep"
cp -- "$WORK_DIR/arm64 fixture.so" "$STRIPPED/libyukari.so"
cp -- "$WORK_DIR/host library.so" "$MERGED/libyukari.so"
cp -- "$WORK_DIR/host library.so" "$CXX_DIR/libyukari.so"
(cd "$WORK_DIR" && bash "$FIXTURE/scripts/package.sh")
test -s "$FIXTURE/out/Yukari.zip"
test "$(cat "$FIXTURE/out/Yukari/keep")" = preserved
mv -- "$STRIPPED/libyukari.so" "$STRIPPED/yukari.so"
bash "$FIXTURE/scripts/package.sh"
rm -- "$STRIPPED/yukari.so"
cp -- "$WORK_DIR/arm64 fixture.so" "$MERGED/libyukari.so"
bash "$FIXTURE/scripts/package.sh"
no_staging_files "$FIXTURE/out"

YUKARI_MODULE_VERSION=1.2.3.test YUKARI_MODULE_VERSION_CODE=42 \
  bash "$FIXTURE/scripts/package.sh" --library "$WORK_DIR/arm64 fixture.so" --output "$OUTPUT"
cmp -- "$FIXTURE/module/module.prop" "$WORK_DIR/original module.prop"
python3 - "$OUTPUT" <<'PY'
import sys
import zipfile
with zipfile.ZipFile(sys.argv[1]) as archive:
    properties = archive.read("module.prop").decode("utf-8").splitlines()
    assert "version=1.2.3.test" in properties
    assert "versionCode=42" in properties
    for name in ("action.sh", "customize.sh", "post-fs-data.sh", "service.sh"):
        assert (archive.getinfo(name).external_attr >> 16) & 0o111
PY
cp -- "$OUTPUT" "$WORK_DIR/previous.zip"
for code in 0 -1 invalid ''; do
  expect_failure env YUKARI_MODULE_VERSION_CODE="$code" bash "$FIXTURE/scripts/package.sh" \
    --library "$WORK_DIR/arm64 fixture.so" --output "$OUTPUT"
  cmp -- "$OUTPUT" "$WORK_DIR/previous.zip"
done
for version in $'bad\nversion' $'bad\rversion' ''; do
  expect_failure env YUKARI_MODULE_VERSION="$version" bash "$FIXTURE/scripts/package.sh" \
    --library "$WORK_DIR/arm64 fixture.so" --output "$OUTPUT"
  cmp -- "$OUTPUT" "$WORK_DIR/previous.zip"
done
cmp -- "$FIXTURE/module/module.prop" "$WORK_DIR/original module.prop"
no_staging_files "$(dirname -- "$OUTPUT")"

python3 - "$OUTPUT" "$WORK_DIR" "$WORK_DIR/host library.so" <<'PY'
import pathlib
import stat
import struct
import sys
import warnings
import zipfile

root = pathlib.Path(sys.argv[2])
with zipfile.ZipFile(sys.argv[1]) as archive:
    files = {entry.filename: archive.read(entry) for entry in archive.infolist()}

def fixture(name, *, omit=None, extra=None, replace=None):
    with zipfile.ZipFile(root / (name + ".zip"), "w") as archive:
        for path, data in files.items():
            if path != omit:
                archive.writestr(path, replace[1] if replace and path == replace[0] else data)
        if extra:
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)
                archive.writestr(extra, b"unexpected")

fixture("missing", omit="service.sh")
fixture("duplicate", extra="module.prop")
fixture("parent", extra="../escape")
fixture("absolute", extra="/absolute")
fixture("backslash", extra="zygisk\\escape")
fixture("drive", extra="C:/escape")
fixture("double-slash", extra="zygisk//escape")
fixture("file-parent", extra="module.prop/child")
link = zipfile.ZipInfo("symlink")
link.external_attr = (stat.S_IFLNK | 0o777) << 16
fixture("symlink", extra=link)
fixture("bad-properties", replace=("module.prop", b"id=Yukari\nversionCode=0\n"))
properties = files["module.prop"].decode("utf-8")
fixture("duplicate-properties", replace=("module.prop", (properties + "\nid=Other\n").encode()))
lines = properties.splitlines()
bad_code = "\n".join("versionCode=0" if line.startswith("versionCode=") else line for line in lines)
fixture("bad-version-code", replace=("module.prop", bad_code.encode()))
fixture("wrong-abi", replace=("zygisk/arm64-v8a.so", pathlib.Path(sys.argv[3]).read_bytes()))
fixture("bad-crc")
corrupt_path = root / "bad-crc.zip"
with zipfile.ZipFile(corrupt_path) as archive:
    offset = archive.getinfo("config.json").header_offset
corrupt = bytearray(corrupt_path.read_bytes())
name_size, extra_size = struct.unpack_from("<HH", corrupt, offset + 26)
corrupt[offset + 30 + name_size + extra_size] ^= 1
corrupt_path.write_bytes(corrupt)
PY
for name in missing duplicate parent absolute backslash drive double-slash file-parent symlink \
  bad-properties duplicate-properties bad-version-code wrong-abi bad-crc; do
  expect_failure bash "$FIXTURE/scripts/verify_package.sh" "$WORK_DIR/$name.zip"
done
printf 'Package structure and atomic-output tests passed (host ELF fixture; not an NDK ABI test).\n'
