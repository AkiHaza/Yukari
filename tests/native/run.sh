#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=$(mktemp -d)
trap 'rm -rf -- "$build_dir"' EXIT
read -r -a extra_cxxflags <<< "${CXXFLAGS:-}"
read -r -a extra_ldflags <<< "${LDFLAGS:-}"

"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -Werror -pthread \
    "${extra_cxxflags[@]}" \
    -I"$root/tests/native/include" -I"$root/module/src/main/cpp" \
    "$root/tests/native/binder_test.cpp" \
    "$root/module/src/main/cpp/service_match.cpp" \
    -Wl,--wrap=malloc -Wl,--wrap=realloc -Wl,--wrap=free \
    "${extra_ldflags[@]}" \
    -o "$build_dir/binder_test"
"$build_dir/binder_test"

"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -Werror \
    "${extra_cxxflags[@]}" -I"$root/module/src/main/cpp" \
    "$root/tests/native/config_test.cpp" "$root/module/src/main/cpp/config.cpp" \
    "${extra_ldflags[@]}" -o "$build_dir/config_test"
"$build_dir/config_test"

bash "$root/tests/native/elf_test.sh"

"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -Werror \
    "${extra_cxxflags[@]}" -I"$root/tests/native/include" -I"$root/module/src/main/cpp" \
    "$root/tests/native/cache_test.cpp" "$root/module/src/main/cpp/service_match.cpp" \
    "${extra_ldflags[@]}" -o "$build_dir/cache_test"
"$build_dir/cache_test"

"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -Werror \
    "${extra_cxxflags[@]}" -I"$root/module/src/main/cpp" \
    "$root/tests/native/feature_test.cpp" "${extra_ldflags[@]}" \
    -o "$build_dir/feature_test"
"$build_dir/feature_test"

jni_home=${JAVA_HOME:-}
if [[ ! -f "$jni_home/include/jni.h" ]]; then
    if command -v javac > /dev/null 2>&1; then
        jni_home=$(dirname -- "$(dirname -- "$(readlink -f -- "$(command -v javac)")")")
    fi
fi
if [[ ! -f "$jni_home/include/jni.h" ]]; then
    echo 'A JDK with JNI headers is required; set JAVA_HOME.' >&2
    exit 1
fi
"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -Werror -pthread \
    "${extra_cxxflags[@]}" -DYUKARI_FEATURE_JNI_TEST \
    -I"$jni_home/include" -I"$jni_home/include/linux" -I"$root/module/src/main/cpp" \
    "$root/tests/native/feature_test.cpp" "${extra_ldflags[@]}" \
    -o "$build_dir/feature_jni_test"
"$build_dir/feature_jni_test"

"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -Werror \
    "${extra_cxxflags[@]}" -I"$root/tests/native/include" -I"$root/module/src/main/cpp" \
    "$root/tests/native/resource_test.cpp" "${extra_ldflags[@]}" \
    -o "$build_dir/resource_test"
"$build_dir/resource_test"

JAVA_HOME="$jni_home" bash "$root/tests/native/production_build.sh"
bash -n "$root/module/action.sh" "$root/module/post-fs-data.sh" \
    "$root/scripts/diagnose_device.sh" "$root/scripts/package.sh"
