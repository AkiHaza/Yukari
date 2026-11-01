#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ZIP_PATH="$ROOT_DIR/out/Yukari.zip"
NATIVE_LIBRARY=""

usage() {
  echo "Usage: bash scripts/package.sh [--library library.so] [--output package.zip]"
}

while (($#)); do
  case "$1" in
    --library|--output)
      if (($# < 2)) || [[ -z $2 ]]; then
        echo "error: $1 requires a path" >&2
        exit 1
      fi
      if [[ $1 == --library ]]; then NATIVE_LIBRARY=$2; else ZIP_PATH=$2; fi
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    *) echo "error: unknown argument: $1" >&2; usage >&2; exit 1 ;;
  esac
done

find_native_library() {
  local candidates=(
    "$ROOT_DIR/module/build/intermediates/stripped_native_libs/release/stripReleaseDebugSymbols/out/lib/arm64-v8a/libyukari.so"
    "$ROOT_DIR/module/build/intermediates/stripped_native_libs/release/stripReleaseDebugSymbols/out/lib/arm64-v8a/yukari.so"
    "$ROOT_DIR/module/build/intermediates/merged_native_libs/release/mergeReleaseNativeLibs/out/lib/arm64-v8a/libyukari.so"
    "$ROOT_DIR/module/build/intermediates/cxx/Release"/*/obj/arm64-v8a/libyukari.so
    "$ROOT_DIR/module/build/intermediates/cxx/Release"/*/obj/arm64-v8a/yukari.so
  )

  local candidate
  for candidate in "${candidates[@]}"; do
    if [ -f "$candidate" ]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

if [[ -z $NATIVE_LIBRARY ]]; then NATIVE_LIBRARY="$(find_native_library || true)"; fi
if [[ -z $NATIVE_LIBRARY ]]; then
  echo "error: arm64-v8a libyukari.so was not produced" >&2
  if [[ -d $ROOT_DIR/module/build ]]; then
    find "$ROOT_DIR/module/build" -type f \( -name 'libyukari.so' -o -name 'yukari.so' \) -print >&2 || true
  fi
  exit 1
fi
if [[ ! -s $NATIVE_LIBRARY ]]; then
  echo "error: native library is missing or empty: $NATIVE_LIBRARY" >&2
  exit 1
fi
if [[ -d $ZIP_PATH || $ZIP_PATH == */ ]]; then
  echo "error: output must name a ZIP file: $ZIP_PATH" >&2
  exit 1
fi

OUT_DIR="$(dirname -- "$ZIP_PATH")"
mkdir -p -- "$OUT_DIR"
OUT_DIR="$(cd -- "$OUT_DIR" && pwd)"
ZIP_PATH="$OUT_DIR/$(basename -- "$ZIP_PATH")"
# Keep temporary files beside the destination so the final rename is atomic.
WORK_DIR="$(mktemp -d "$OUT_DIR/.yukari-package.XXXXXXXX")"
trap 'rm -rf -- "$WORK_DIR"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
STAGE="$WORK_DIR/stage"
TEMP_ZIP="$WORK_DIR/Yukari.zip"
mkdir -p -- "$STAGE/zygisk"
for file in module.prop config.json post-fs-data.sh service.sh action.sh customize.sh; do
  cp -- "$ROOT_DIR/module/$file" "$STAGE/$file"
done
chmod 0755 "$STAGE/post-fs-data.sh" "$STAGE/service.sh" "$STAGE/action.sh" "$STAGE/customize.sh"
cp -- "$NATIVE_LIBRARY" "$STAGE/zygisk/arm64-v8a.so"

python3 - "$STAGE/module.prop" <<'PY'
import os
import pathlib
import re
import sys

overrides = {}
for variable, key in (("YUKARI_MODULE_VERSION", "version"),
                      ("YUKARI_MODULE_VERSION_CODE", "versionCode")):
    if variable not in os.environ:
        continue
    value = os.environ[variable]
    if not value or "\n" in value or "\r" in value:
        sys.exit(f"error: {variable} must be nonempty and contain no line breaks")
    if key == "versionCode" and (not re.fullmatch(r"[0-9]+", value) or int(value) < 1):
        sys.exit("error: YUKARI_MODULE_VERSION_CODE must be a positive integer")
    overrides[key] = value
if overrides:
    path = pathlib.Path(sys.argv[1])
    lines = path.read_text(encoding="utf-8").splitlines()
    replaced = set()
    for index, line in enumerate(lines):
        key, separator, _ = line.partition("=")
        key = key.strip()
        if separator and key in overrides:
            lines[index] = f"{key}={overrides[key]}"
            replaced.add(key)
    lines.extend(f"{key}={value}" for key, value in overrides.items() if key not in replaced)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
PY

python3 - "$STAGE" "$TEMP_ZIP" <<'PY'
import pathlib
import sys
import zipfile

stage = pathlib.Path(sys.argv[1])
with zipfile.ZipFile(sys.argv[2], "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for path in sorted(stage.rglob("*")):
        if path.is_file():
            archive.write(path, path.relative_to(stage).as_posix())
PY

bash "$ROOT_DIR/scripts/verify_package.sh" "$TEMP_ZIP"
mv -fT -- "$TEMP_ZIP" "$ZIP_PATH"
echo "Packaged $ZIP_PATH from $NATIVE_LIBRARY"
