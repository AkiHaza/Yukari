#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT
mkdir -p "$fixture/project/scripts" "$fixture/project/tests/native" \
    "$fixture/project/tests/build" "$fixture/project/module" "$fixture/jdk/bin" "$fixture/sdk"
cp "$root/scripts/build.sh" "$fixture/project/scripts/build.sh"
cat > "$fixture/jdk/bin/java" <<'EOF'
#!/usr/bin/env bash
echo "    java.specification.version = ${FAKE_JAVA_VERSION:-17}" >&2
EOF
chmod +x "$fixture/jdk/bin/java"
cat > "$fixture/project/tests/native/run.sh" <<'EOF'
echo native >> "$CAPTURE"
exit "${FAIL_NATIVE:-0}"
EOF
cat > "$fixture/project/tests/build/run.sh" <<'EOF'
echo infra >> "$CAPTURE"
EOF
cat > "$fixture/project/scripts/android-sdk.sh" <<'EOF'
echo sdk >> "$CAPTURE"
exit "${FAIL_SDK:-0}"
EOF
cat > "$fixture/project/gradlew" <<'EOF'
echo gradle >> "$CAPTURE"
[[ $* == *:module:assembleRelease* ]] || exit 91
exit "${FAIL_GRADLE:-0}"
EOF
cat > "$fixture/project/scripts/package.sh" <<'EOF'
echo package >> "$CAPTURE"
printf '%s\n%s\n' "${YUKARI_MODULE_VERSION:-original}" "${YUKARI_MODULE_VERSION_CODE:-original}" > "$PACKAGE_CAPTURE"
EOF
printf 'version=1.2.3\nversionCode=7\n' > "$fixture/project/module/module.prop"
git init -q -b main "$fixture/project"
git -C "$fixture/project" config user.name 'Build test'
git -C "$fixture/project" config user.email 'build-test@example.invalid'
git -C "$fixture/project" config commit.gpgsign false
git -C "$fixture/project" config core.hooksPath /dev/null
git -C "$fixture/project" add .
git -C "$fixture/project" commit -qm fixture
sha=$(git -C "$fixture/project" rev-parse HEAD)

invoke() {
    : > "$fixture/calls"
    env -u GITHUB_ACTIONS -u GITHUB_SHA -u GITHUB_REF -u GITHUB_REF_TYPE -u GITHUB_REF_NAME \
        -u GITHUB_RUN_NUMBER -u YUKARI_MODULE_VERSION -u YUKARI_MODULE_VERSION_CODE \
        -u FAIL_NATIVE -u FAIL_SDK -u FAIL_GRADLE -u FAKE_JAVA_VERSION \
        JAVA_HOME="$fixture/jdk" ANDROID_HOME="$fixture/sdk" ANDROID_SDK_ROOT="$fixture/sdk" \
        CAPTURE="$fixture/calls" PACKAGE_CAPTURE="$fixture/package" \
        "$@" > "$fixture/output" 2>&1
}
expect_calls() {
    local actual
    actual=$(tr '\n' ' ' < "$fixture/calls")
    [[ $actual == "$1" ]] || { cat "$fixture/output" >&2; echo "Unexpected phases: $actual" >&2; exit 1; }
}
expect_failure() {
    local expected=$1
    shift
    local result=0
    invoke "$@" || result=$?
    [[ $result == "$expected" ]] || { cat "$fixture/output" >&2; echo "Expected exit $expected, got $result" >&2; exit 1; }
}

invoke bash "$fixture/project/scripts/build.sh" test
expect_calls 'native infra '
invoke bash "$fixture/project/scripts/build.sh" android
expect_calls 'sdk gradle package '
invoke bash "$fixture/project/scripts/build.sh" all
expect_calls 'native infra sdk gradle package '
expect_failure 23 FAIL_NATIVE=23 bash "$fixture/project/scripts/build.sh" all
expect_calls 'native '
expect_failure 24 FAIL_SDK=24 bash "$fixture/project/scripts/build.sh" android
expect_calls 'sdk '
expect_failure 25 FAIL_GRADLE=25 bash "$fixture/project/scripts/build.sh" android
expect_calls 'sdk gradle '
expect_failure 1 FAKE_JAVA_VERSION=25 bash "$fixture/project/scripts/build.sh" android
expect_calls ''
expect_failure 1 GITHUB_SHA=0000000000000000000000000000000000000000 bash "$fixture/project/scripts/build.sh" test
expect_calls ''
invoke GITHUB_ACTIONS=true GITHUB_SHA="$sha" GITHUB_REF_NAME=dev GITHUB_RUN_NUMBER=42 \
    bash "$fixture/project/scripts/build.sh" package
expect_calls 'package '
[[ $(head -n 1 "$fixture/package") == "1.2.3-dev (42-${sha:0:7})" ]]
[[ $(tail -n 1 "$fixture/package") == 42 ]]
[[ $(cat "$fixture/project/module/module.prop") == $'version=1.2.3\nversionCode=7' ]]
expect_failure 2 bash "$fixture/project/scripts/build.sh" unknown
mkdir -p "$fixture/not-a-repository/scripts"
cp "$root/scripts/build.sh" "$fixture/not-a-repository/scripts/build.sh"
expect_failure 128 bash "$fixture/not-a-repository/scripts/build.sh" test
expect_calls ''
echo 'Build coordinator tests passed'
