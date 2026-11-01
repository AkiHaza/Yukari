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
