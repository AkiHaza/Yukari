#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=$(mktemp -d)
trap 'rm -rf -- "$build_dir"' EXIT
jni_home=${JAVA_HOME:?set JAVA_HOME to a Linux JDK}
mkdir -p "$build_dir/shim/include/android"
cat > "$build_dir/shim/include/android/log.h" <<'EOF'
#pragma once
#include <stdarg.h>
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_ERROR 6
extern "C" int __android_log_vprint(int, const char *, const char *, va_list);
EOF
cat > "$build_dir/shim/log.cpp" <<'EOF'
#include <stdarg.h>
extern "C" int __android_log_vprint(int, const char *, const char *, va_list) { return 0; }
EOF
"${CXX:-g++}" -shared -fPIC "$build_dir/shim/log.cpp" -o "$build_dir/shim/liblog.so"
if ! cmake -S "$root/module/src/main/cpp" -B "$build_dir/build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="${CXX:-g++}" \
    "-DCMAKE_CXX_FLAGS=-Wall -Wextra -Werror -I\"$jni_home/include\" -I\"$jni_home/include/linux\" -I\"$build_dir/shim/include\"" \
    "-DCMAKE_SHARED_LINKER_FLAGS=-L\"$build_dir/shim\"" > "$build_dir/configure.log" 2>&1; then
    cat "$build_dir/configure.log" >&2
    exit 1
fi
if ! cmake --build "$build_dir/build" --parallel 2 > "$build_dir/build.log" 2>&1; then
    cat "$build_dir/build.log" >&2
    exit 1
fi
bash "$root/scripts/verify_elf.sh" "$build_dir/build/libyukari.so"
if readelf --wide --sections "$build_dir/build/libyukari.so" | grep -q '\.symtab'; then
    echo 'Release library retains the private symbol table' >&2
    exit 1
fi
echo 'Production CMake/JNI host build passed (Android log shim; not an NDK/device test)'
