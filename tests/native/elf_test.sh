#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=$(mktemp -d)
trap 'rm -rf -- "$build_dir"' EXIT
cat > "$build_dir/exports.cpp" <<'EOF'
extern "C" {
    void zygisk_module_entry() {}
    void private_helper() {}
    int private_value = 42;
}
EOF
"${CXX:-g++}" -shared -fPIC "$build_dir/exports.cpp" \
    -Wl,--version-script="$root/module/src/main/cpp/exports.map" \
    -o "$build_dir/valid.so"
bash "$root/scripts/verify_elf.sh" "$build_dir/valid.so"
"${CXX:-g++}" -shared -fPIC "$build_dir/exports.cpp" -o "$build_dir/leaking.so"
if bash "$root/scripts/verify_elf.sh" "$build_dir/leaking.so" > /dev/null 2>&1; then
    echo 'ELF check accepted private exports' >&2
    exit 1
fi
echo 'ELF regression tests passed'
