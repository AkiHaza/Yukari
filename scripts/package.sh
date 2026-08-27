#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
ZIP_PATH="$OUT_DIR/Yukari.zip"
STAGE="$OUT_DIR/Yukari"

find_native_library() {
  local candidates=(
    "$ROOT_DIR/module/build/intermediates/stripped_native_libs/release/out/lib/arm64-v8a/libyukari.so"
    "$ROOT_DIR/module/build/intermediates/stripped_native_libs/release/out/lib/arm64-v8a/yukari.so"
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

rm -rf "$STAGE" "$ZIP_PATH"
mkdir -p "$STAGE/zygisk" "$OUT_DIR"
cp "$ROOT_DIR/module/module.prop" "$STAGE/module.prop"
cp "$ROOT_DIR/module/config.json" "$STAGE/config.json"
cp "$ROOT_DIR/module/post-fs-data.sh" "$STAGE/post-fs-data.sh"
cp "$ROOT_DIR/module/service.sh" "$STAGE/service.sh"
cp "$ROOT_DIR/module/action.sh" "$STAGE/action.sh"
cp "$ROOT_DIR/module/customize.sh" "$STAGE/customize.sh"
chmod 0755 "$STAGE/post-fs-data.sh" "$STAGE/service.sh" "$STAGE/action.sh" "$STAGE/customize.sh"

NATIVE_LIBRARY="$(find_native_library || true)"
if [ -z "$NATIVE_LIBRARY" ]; then
  echo "error: arm64-v8a libyukari.so was not produced" >&2
  find "$ROOT_DIR/module/build" -type f \( -name 'libyukari.so' -o -name 'yukari.so' \) -print >&2 || true
  exit 1
fi

cp "$NATIVE_LIBRARY" "$STAGE/zygisk/arm64-v8a.so"

(
  cd "$STAGE"
  zip -9 -r "$ZIP_PATH" .
)

test -s "$ZIP_PATH"
unzip -l "$ZIP_PATH" | grep -q 'module.prop'
unzip -l "$ZIP_PATH" | grep -q 'zygisk/arm64-v8a.so'
unzip -l "$ZIP_PATH" | grep -q 'action.sh'
unzip -l "$ZIP_PATH" | grep -q 'customize.sh'
echo "Packaged $ZIP_PATH from $NATIVE_LIBRARY"
