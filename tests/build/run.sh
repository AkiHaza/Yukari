#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
bash "$root/tests/build/android_sdk_test.sh"
bash "$root/tests/build/package_test.sh"
bash "$root/tests/build/coordinator_test.sh"
bash -n "$root/scripts/build.sh" "$root/gradlew"
echo 'Build infrastructure tests passed'
