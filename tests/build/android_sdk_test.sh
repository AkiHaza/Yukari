#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
project="$temporary/project with spaces"
sdk="$temporary/SDK with spaces"
mkdir -p "$project/scripts" "$sdk/cmdline-tools/latest/bin"
cp "$root/scripts/android-sdk.sh" "$project/scripts/android-sdk.sh"
script="$project/scripts/android-sdk.sh"
cat >"$project/gradle.properties" <<'EOF'
# Test fixture deliberately lives outside the invoking directory.
yukari.compileSdk=36
yukari.buildToolsVersion=35.0.0
yukari.ndkVersion=27.2.12479018
yukari.cmakeVersion=3.22.1
EOF
export FAKE_SDK_LOG="$temporary/sdkmanager calls"
export FAKE_METADATA_WRITER="$temporary/write SDK metadata.py"
output="$temporary/script output"
ci_env="$temporary/github env"
ci_path="$temporary/github path"
environment=(env -u ANDROID_HOME -u ANDROID_SDK_ROOT -u GITHUB_ENV -u GITHUB_PATH
    -u FAKE_LICENSE_STATUS -u FAKE_INSTALL_STATUS -u FAKE_INSTALL_SKIP_METADATA)
expected_packages=(platform-tools 'platforms;android-36' 'build-tools;35.0.0'
    'ndk;27.2.12479018' 'cmake;3.22.1')

fail() { printf 'SDK test failed: %s\n' "$1" >&2; cat "$output" >&2; exit 1; }
contains() { grep -Fqx -- "$2" "$1" || fail "missing line '$2' in $1"; }
absent() { if grep -Fq -- "$2" "$1"; then fail "unexpected '$2' in $1"; fi; }
expect_status() {
    local expected=$1 status
    shift
    set +e
    "$@" >"$output" 2>&1
    status=$?
    set -e
    [[ $status == "$expected" ]] || fail "expected exit $expected, got $status"
}
with_ignored_sigpipe() {
    # An ignored disposition survives exec, matching the GitHub runner that
    # makes GNU yes report EPIPE/exit 1 instead of dying with status 141.
    bash -c 'trap "" PIPE; exec "$@"' _ "$@"
}
reset_fake() {
    : >"$FAKE_SDK_LOG"
    : >"$ci_env"
    : >"$ci_path"
    python3 "$FAKE_METADATA_WRITER" "$sdk" --clear
}
installed() {
    python3 "$FAKE_METADATA_WRITER" "$sdk" "${expected_packages[@]}"
}

# The fake installer writes the same machine metadata/layout used by both
# legacy sdkmanager and CLI 23, rather than inventing a human-readable table.
cat >"$FAKE_METADATA_WRITER" <<'PY'
from pathlib import Path
import shutil
import sys
import xml.etree.ElementTree as ET

root = Path(sys.argv[1])
packages = sys.argv[2:]
if packages == ['--clear']:
    for name in ('platform-tools', 'platforms', 'build-tools', 'ndk', 'cmake'):
        shutil.rmtree(root / name, ignore_errors=True)
    sys.exit(0)
namespace = 'urn:android-sdk-fixture'
ET.register_namespace('sdk', namespace)
for package in packages:
    parts = package.split(';')
    family = parts[0]
    directory = root.joinpath(*parts)
    directory.mkdir(parents=True, exist_ok=True)
    version = '37.0.1' if family == 'platform-tools' else '2' if family == 'platforms' else parts[1]
    source = 'Pkg.Revision = ' + version + '\nPkg.Path = ' + package + '\n'
    if family == 'platforms':
        source += 'AndroidVersion.ApiLevel=' + parts[1][len('android-'):] + '\n'
    (directory / 'source.properties').write_text(source)
    repository = ET.Element('{' + namespace + '}repository')
    node = ET.SubElement(repository, '{' + namespace + '}localPackage', path=package)
    revision = ET.SubElement(node, '{' + namespace + '}revision')
    for name, value in zip(('major', 'minor', 'micro'), version.split('.')):
        ET.SubElement(revision, '{' + namespace + '}' + name).text = value
    if family == 'platforms':
        details = ET.SubElement(node, '{' + namespace + '}type-details')
        ET.SubElement(details, '{' + namespace + '}api-level').text = parts[1][len('android-'):]
    ET.ElementTree(repository).write(directory / 'package.xml', encoding='utf-8', xml_declaration=True)
    files = []
    if family == 'platform-tools':
        files = [('adb', True)]
    elif family == 'platforms':
        files = [('android.jar', False)]
    elif family == 'build-tools':
        files = [('aapt2', True), ('zipalign', True), ('lib/d8.jar', False)]
    elif family == 'ndk':
        files = [('build/cmake/android.toolchain.cmake', False)]
        host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
        files += [('toolchains/llvm/prebuilt/' + host + '/bin/' + tool, True)
                  for tool in ('clang++', 'llvm-readelf', 'llvm-strip')]
    elif family == 'cmake':
        files = [('bin/cmake', True), ('bin/ninja', True)]
    for filename, executable in files:
        path = directory / filename
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('fixture-tool\n')
        path.chmod(0o755 if executable else 0o644)
PY

cat >"$sdk/cmdline-tools/latest/bin/sdkmanager" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'SDKMANAGER=%s\n' "$0" >>"$FAKE_SDK_LOG"
printf '%s\n' "$@" >>"$FAKE_SDK_LOG"
[[ ${1:-} == "--sdk_root=$ANDROID_SDK_ROOT" ]] || exit 91
[[ $ANDROID_SDK_ROOT == "$ANDROID_HOME" ]] || exit 92
shift
case "${1:-}" in
    --licenses)
        printf 'PRIVATE_LICENSE_BODY\n'
        read -r answer
        [[ $answer == y ]] || exit 93
        exit "${FAKE_LICENSE_STATUS:-0}"
        ;;
    --list_installed)
        printf 'Human-readable SDK listings must not be parsed.\n' >&2
        exit 94
        ;;
    *)
        printf 'PRIVATE_INSTALL_PROGRESS\n'
        if [[ ${FAKE_INSTALL_STATUS:-0} != 0 ]]; then
            printf 'INSTALL_FAILURE_DETAILS\n' >&2
            exit "$FAKE_INSTALL_STATUS"
        fi
        if [[ ${FAKE_INSTALL_SKIP_METADATA:-0} != 1 ]]; then
            python3 "$FAKE_METADATA_WRITER" "$ANDROID_SDK_ROOT" "$@"
        fi
        ;;
esac
EOF
chmod +x "$sdk/cmdline-tools/latest/bin/sdkmanager"
reset_fake
expect_status 0 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" ANDROID_HOME="$sdk" \
    GITHUB_ENV="$ci_env" GITHUB_PATH="$ci_path" bash "$script"
for package in "${expected_packages[@]}"; do contains "$FAKE_SDK_LOG" "$package"; done
contains "$FAKE_SDK_LOG" "--sdk_root=$sdk"
contains "$FAKE_SDK_LOG" --licenses
absent "$FAKE_SDK_LOG" --list_installed
if grep -Fqx -- tools "$FAKE_SDK_LOG"; then fail 'obsolete tools package was requested'; fi
absent "$output" PRIVATE_LICENSE_BODY
absent "$output" PRIVATE_INSTALL_PROGRESS
contains "$ci_env" "ANDROID_HOME=$sdk"
contains "$ci_env" "ANDROID_SDK_ROOT=$sdk"
contains "$ci_path" "$sdk/cmdline-tools/latest/bin"
contains "$ci_path" "$sdk/platform-tools"
contains "$ci_path" "$sdk/cmake/3.22.1/bin"

# --check is read-only, and either root variable is sufficient.
: >"$FAKE_SDK_LOG"
expect_status 0 "${environment[@]}" ANDROID_HOME="$sdk" bash "$script" --check
[[ ! -s $FAKE_SDK_LOG ]] || fail '--check invoked sdkmanager'
absent "$FAKE_SDK_LOG" --licenses
if grep -Fqx -- platform-tools "$FAKE_SDK_LOG"; then fail '--check tried to install'; fi
grep -Fq 'Local shell: export ANDROID_HOME=' "$output" || fail 'missing local environment help'
grep -Fq 'Local shell: export PATH=' "$output" || fail 'missing local PATH help'

# Failed license/install calls retain sdkmanager's exit code. Successful
# licenses intentionally close stdin early, exercising yes/SIGPIPE handling.
reset_fake
expect_status 23 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" FAKE_LICENSE_STATUS=23 bash "$script"
grep -Fq PRIVATE_LICENSE_BODY "$output" || fail 'license failure details were hidden'
absent "$FAKE_SDK_LOG" --list_installed
[[ ! -s $ci_env ]] || fail 'failed setup exported CI environment'
reset_fake
expect_status 29 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" FAKE_INSTALL_STATUS=29 bash "$script"
grep -Fq INSTALL_FAILURE_DETAILS "$output" || fail 'installation failure details were hidden'
absent "$FAKE_SDK_LOG" --list_installed
absent "$output" PRIVATE_LICENSE_BODY

# CI may inherit SIGPIPE=SIG_IGN. Prove the wrapper really reproduces GNU
# yes's exit 1, then require sdkmanager's status to remain authoritative.
expect_status 0 with_ignored_sigpipe bash -c '
    set +e
    yes 2>/dev/null | head -n 1 >/dev/null
    statuses=("${PIPESTATUS[@]}")
    [[ ${statuses[0]} == 1 && ${statuses[1]} == 0 ]]
'
reset_fake
expect_status 0 with_ignored_sigpipe "${environment[@]}" ANDROID_SDK_ROOT="$sdk" bash "$script"
contains "$FAKE_SDK_LOG" --licenses
for package in "${expected_packages[@]}"; do contains "$FAKE_SDK_LOG" "$package"; done
absent "$output" PRIVATE_LICENSE_BODY
absent "$output" 'license input failed'
reset_fake
expect_status 23 with_ignored_sigpipe "${environment[@]}" ANDROID_SDK_ROOT="$sdk" \
    FAKE_LICENSE_STATUS=23 bash "$script"
grep -Fq PRIVATE_LICENSE_BODY "$output" || fail 'ignored SIGPIPE swallowed license failure details'
if grep -Fqx -- platform-tools "$FAKE_SDK_LOG"; then fail 'license failure continued into installation'; fi
reset_fake
expect_status 29 with_ignored_sigpipe "${environment[@]}" ANDROID_SDK_ROOT="$sdk" \
    FAKE_INSTALL_STATUS=29 bash "$script"
grep -Fq INSTALL_FAILURE_DETAILS "$output" || fail 'ignored SIGPIPE swallowed installation failure details'
absent "$output" PRIVATE_LICENSE_BODY

installed
: >"$FAKE_SDK_LOG"
expect_status 0 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" FAKE_LICENSE_STATUS=23 FAKE_INSTALL_STATUS=29 bash "$script"
[[ ! -s $FAKE_SDK_LOG ]] || fail 'complete SDK unnecessarily reinstalled'

reset_fake
expect_status 1 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" bash "$script" --check
grep -Fq 'invalid required package ndk;27.2.12479018' "$output" || fail 'missing-package diagnostic absent'
absent "$FAKE_SDK_LOG" --licenses
expect_status 1 "${environment[@]}" bash "$script" --check
expect_status 1 "${environment[@]}" ANDROID_SDK_ROOT="$temporary/missing SDK" bash "$script" --check
mkdir -p "$temporary/other SDK"
expect_status 1 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" ANDROID_HOME="$temporary/other SDK" bash "$script" --check
grep -Fq 'different SDKs' "$output" || fail 'conflicting SDK roots were not diagnosed'
expect_status 1 "${environment[@]}" ANDROID_SDK_ROOT="$temporary/other SDK" bash "$script" --check
grep -Fq 'sdkmanager missing' "$output" || fail 'missing sdkmanager was not diagnosed'

# Prefer latest if present; otherwise compare installed versions numerically.
mkdir -p "$sdk/cmdline-tools/9.0/bin" "$sdk/cmdline-tools/10.0/bin"
cp "$sdk/cmdline-tools/latest/bin/sdkmanager" "$sdk/cmdline-tools/9.0/bin/sdkmanager"
cp "$sdk/cmdline-tools/latest/bin/sdkmanager" "$sdk/cmdline-tools/10.0/bin/sdkmanager"
installed
: >"$FAKE_SDK_LOG"
expect_status 0 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" ANDROID_HOME="$sdk/." GITHUB_PATH="$ci_path" bash "$script" --check
contains "$ci_path" "$sdk/cmdline-tools/latest/bin"
mv "$sdk/cmdline-tools/latest/bin/sdkmanager" "$temporary/sdkmanager saved"
: >"$FAKE_SDK_LOG"
: >"$ci_path"
expect_status 0 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" GITHUB_PATH="$ci_path" bash "$script" --check
contains "$ci_path" "$sdk/cmdline-tools/10.0/bin"
[[ ! -s $FAKE_SDK_LOG ]] || fail 'metadata check invoked sdkmanager'

# Missing/damaged/mismatched machine metadata and incomplete tools are rejected
# without invoking the installer in --check mode.
printf '<broken XML' >"$sdk/ndk/27.2.12479018/package.xml"
expect_status 1 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" bash "$script" --check
installed
sed -i 's/ndk;27.2.12479018/ndk;27.2.99999999/' "$sdk/ndk/27.2.12479018/package.xml"
expect_status 1 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" bash "$script" --check
grep -Fq 'localPackage ID does not match' "$output" || fail 'wrong package ID was accepted'
installed
sed -i 's/27.2.12479018/27.2.99999999/g' "$sdk/ndk/27.2.12479018/source.properties"
expect_status 1 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" bash "$script" --check
installed
printf 'malformed source properties\n' >"$sdk/cmake/3.22.1/source.properties"
expect_status 1 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" bash "$script" --check
installed
sed -i 's/AndroidVersion.ApiLevel=36/AndroidVersion.ApiLevel=37/' "$sdk/platforms/android-36/source.properties"
expect_status 1 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" bash "$script" --check
installed
rm -- "$sdk/cmake/3.22.1/bin/ninja"
expect_status 1 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" bash "$script" --check
grep -Fq 'missing required tool/file' "$output" || fail 'missing binary was accepted'
installed
chmod -x "$sdk/platform-tools/adb"
expect_status 1 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" bash "$script" --check
grep -Fq 'not executable' "$output" || fail 'nonexecutable binary was accepted'
[[ ! -s $FAKE_SDK_LOG ]] || fail 'damaged metadata triggered sdkmanager during --check'
installed
for package in "${expected_packages[@]}"; do
    directory="$sdk/${package//;/\/}"
    rm -- "$directory/package.xml"
    sed -i '/^Pkg.Path = /d' "$directory/source.properties"
done
expect_status 0 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" bash "$script" --check
reset_fake
expect_status 1 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" FAKE_INSTALL_SKIP_METADATA=1 bash "$script"
grep -Fq 'check failed after installation' "$output" || fail 'incomplete installation was accepted'

# Changing the fixture property changes the actual requested package.
sed -i 's/yukari.compileSdk=36/yukari.compileSdk=37/' "$project/gradle.properties"
reset_fake
expect_status 0 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" bash "$script"
contains "$FAKE_SDK_LOG" 'platforms;android-37'
if grep -Fqx -- 'platforms;android-36' "$FAKE_SDK_LOG"; then fail 'compileSdk was hardcoded'; fi
printf 'yukari.compileSdk=38\n' >>"$project/gradle.properties"
expect_status 1 "${environment[@]}" ANDROID_SDK_ROOT="$sdk" bash "$script" --check
expect_status 0 "${environment[@]}" bash "$script" --help
expect_status 2 "${environment[@]}" bash "$script" --unknown
printf 'Android SDK preparation tests passed.\n'
