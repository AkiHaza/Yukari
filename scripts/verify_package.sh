#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if (($# != 1)); then
  echo 'Usage: bash scripts/verify_package.sh package.zip' >&2
  exit 1
fi
PACKAGE=$1
if [[ ! -s $PACKAGE ]]; then
  echo "error: package is missing or empty: $PACKAGE" >&2
  exit 1
fi
WORK_DIR="$(mktemp -d)"
trap 'rm -rf -- "$WORK_DIR"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

python3 - "$PACKAGE" "$WORK_DIR/arm64-v8a.so" <<'PY'
import pathlib
import re
import stat
import sys
import zipfile
import zlib

required = {
    "module.prop", "config.json", "post-fs-data.sh", "service.sh",
    "action.sh", "customize.sh", "zygisk/arm64-v8a.so",
}

def fail(message):
    raise ValueError(message)

try:
    with zipfile.ZipFile(sys.argv[1]) as archive:
        entries = {}
        paths = {}
        for entry in archive.infolist():
            name = entry.orig_filename
            if name in entries:
                fail(f"duplicate ZIP path: {name!r}")
            path = name[:-1] if entry.is_dir() else name
            parts = path.split("/")
            if (not name or name.startswith("/") or "\\" in name or ":" in name
                    or any(part in ("", ".", "..") for part in parts)
                    or any(ord(char) < 32 or ord(char) == 127 for char in name)):
                fail(f"unsafe ZIP path: {name!r}")
            kind = stat.S_IFMT(entry.external_attr >> 16)
            if kind not in (0, stat.S_IFREG, stat.S_IFDIR):
                fail(f"unsupported ZIP entry type: {name!r}")
            if kind and (kind == stat.S_IFDIR) != entry.is_dir():
                fail(f"inconsistent ZIP entry type: {name!r}")
            if path in paths:
                fail(f"conflicting ZIP paths: {name!r}")
            if entry.flag_bits & 1:
                fail(f"encrypted ZIP entry: {name!r}")
            entries[name] = entry
            paths[path] = entry.is_dir()
        for path in paths:
            parts = path.split("/")
            for depth in range(1, len(parts)):
                parent = "/".join(parts[:depth])
                if parent in paths and not paths[parent]:
                    fail(f"ZIP file used as a parent directory: {parent!r}")
        missing = sorted(required - entries.keys())
        if missing:
            fail("missing required ZIP files: " + ", ".join(missing))
        for name in sorted(required):
            if entries[name].is_dir() or entries[name].file_size == 0:
                fail(f"required ZIP file is empty or a directory: {name}")
        corrupt = archive.testzip()
        if corrupt is not None:
            fail(f"ZIP CRC mismatch: {corrupt!r}")

        properties = {}
        for line in archive.read("module.prop").decode("utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            key, separator, value = line.partition("=")
            key, value = key.strip(), value.strip()
            if not separator or not key or key in properties:
                fail(f"invalid or duplicate module.prop field: {key!r}")
            properties[key] = value
        for key in ("id", "name", "version", "versionCode", "author", "description"):
            if not properties.get(key):
                fail(f"module.prop requires nonempty {key}")
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9._-]*", properties["id"]):
            fail("invalid module.prop id")
        if not re.fullmatch(r"[0-9]+", properties["versionCode"]) or int(properties["versionCode"]) < 1:
            fail("module.prop versionCode must be a positive integer")
        pathlib.Path(sys.argv[2]).write_bytes(archive.read("zygisk/arm64-v8a.so"))
except (OSError, ValueError, RuntimeError, zipfile.BadZipFile, NotImplementedError, zlib.error) as error:
    print(f"error: invalid package: {error}", file=sys.stderr)
    sys.exit(1)
PY

bash "$ROOT_DIR/scripts/verify_elf.sh" "$WORK_DIR/arm64-v8a.so" --android
echo "Package checks passed: $PACKAGE"
