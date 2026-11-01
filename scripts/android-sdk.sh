#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
properties="$root/gradle.properties"

fail() {
    printf 'Android SDK: %s\n' "$1" >&2
    exit "${2:-1}"
}

usage() {
    cat <<'EOF'
Usage: bash scripts/android-sdk.sh [--check]

Default: accept Android SDK licenses and install the versions specified by
gradle.properties, then verify that every required package is installed.
--check: verify package metadata and tools only; do not run sdkmanager.

Set ANDROID_SDK_ROOT or ANDROID_HOME to an existing SDK with command-line
tools installed. If both are set they must resolve to the same directory.
Python 3 reads SDK package.xml/source.properties without parsing CLI tables.
GitHub Actions receives environment/path entries through GITHUB_ENV and
GITHUB_PATH. Local invocations print exports to apply in the calling shell.
EOF
}

check_only=false
case "${1:-}" in
    '') [[ $# == 0 ]] || fail 'unexpected empty argument' 2 ;;
    --check) [[ $# == 1 ]] || fail 'unexpected arguments after --check' 2; check_only=true ;;
    --help|-h) usage; exit 0 ;;
    *) usage >&2; fail "unknown argument: $1" 2 ;;
esac

property() {
    local value
    value=$(awk -v wanted="$1" '
        {
            sub(/\r$/, "")
            if ($0 ~ /^[[:space:]]*[#!]/) next
            separator = index($0, "=")
            if (!separator) next
            key = substr($0, 1, separator - 1)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", key)
            if (key != wanted) next
            value = substr($0, separator + 1)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
            count++
        }
        END { if (count != 1 || value == "") exit 1; print value }
    ' "$properties") || fail "expected one nonempty $1 in $properties"
    printf '%s' "$value"
}

[[ -f $properties ]] || fail "missing build properties: $properties"
compile_sdk=$(property yukari.compileSdk)
build_tools=$(property yukari.buildToolsVersion)
ndk=$(property yukari.ndkVersion)
cmake=$(property yukari.cmakeVersion)
[[ $compile_sdk =~ ^[0-9]+$ ]] || fail 'yukari.compileSdk must be numeric'
for version in "$build_tools" "$ndk" "$cmake"; do
    [[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "invalid SDK tool version: $version"
done
packages=(
    platform-tools
    "platforms;android-$compile_sdk"
    "build-tools;$build_tools"
    "ndk;$ndk"
    "cmake;$cmake"
)

sdk_root=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}
[[ -n $sdk_root ]] || fail 'set ANDROID_SDK_ROOT or ANDROID_HOME to an existing SDK'
[[ $sdk_root != *$'\n'* && $sdk_root != *$'\r'* ]] || fail 'SDK path cannot contain a newline'
[[ -d $sdk_root ]] || fail "SDK directory does not exist: $sdk_root"
sdk_root=$(cd -P -- "$sdk_root" && pwd -P)
if [[ -n ${ANDROID_HOME:-} && -n ${ANDROID_SDK_ROOT:-} ]]; then
    [[ -d $ANDROID_HOME ]] || fail "ANDROID_HOME does not exist: $ANDROID_HOME"
    sdk_home=$(cd -P -- "$ANDROID_HOME" && pwd -P)
    [[ $sdk_home == "$sdk_root" ]] || fail 'ANDROID_HOME and ANDROID_SDK_ROOT point to different SDKs'
fi
export ANDROID_HOME="$sdk_root" ANDROID_SDK_ROOT="$sdk_root"

sdkmanager="$sdk_root/cmdline-tools/latest/bin/sdkmanager"
if [[ ! -x $sdkmanager ]]; then
    candidates=()
    for candidate in "$sdk_root"/cmdline-tools/*/bin/sdkmanager; do
        [[ -x $candidate ]] && candidates+=("$candidate")
    done
    [[ ${#candidates[@]} != 0 ]] || fail "sdkmanager missing under $sdk_root/cmdline-tools; install Android command-line tools first"
    sdkmanager=$(printf '%s\n' "${candidates[@]}" | sort -V | tail -n 1)
fi

temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
command -v python3 >/dev/null 2>&1 || fail 'python3 is required to validate SDK metadata'

check_metadata() {
    python3 - "$sdk_root" "${packages[@]}" <<'PY'
import os
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

sdk = Path(sys.argv[1])
required = sys.argv[2:]
failures = []
host = {'linux': 'linux-x86_64', 'darwin': 'darwin-x86_64'}.get(sys.platform)

def local_name(element):
    return element.tag.rsplit('}', 1)[-1]

def canonical_id(value):
    return value.replace('/', ';')

def revision(value):
    if not re.fullmatch(r'[0-9]+(?:\.[0-9]+){0,2}', value):
        raise ValueError('invalid Pkg.Revision: ' + value)
    components = [int(part) for part in value.split('.')]
    return tuple(components + [0] * (3 - len(components)))

def properties(path):
    result = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        line = line.strip()
        if not line or line.startswith(('#', '!')):
            continue
        if '=' not in line:
            raise ValueError('malformed source.properties line')
        key, value = (part.strip() for part in line.split('=', 1))
        if key in result:
            raise ValueError('duplicate source.properties key: ' + key)
        result[key] = value
    return result

def require_file(directory, relative, executable=False):
    path = directory / relative
    if not path.is_file() or path.stat().st_size == 0:
        raise ValueError('missing required tool/file: ' + str(relative))
    if executable and not os.access(path, os.X_OK):
        raise ValueError('tool is not executable: ' + str(relative))

for package in required:
    directory = sdk.joinpath(*package.split(';'))
    try:
        if not directory.is_dir():
            raise ValueError('package directory is missing')
        source = properties(directory / 'source.properties')
        source_revision = revision(source.get('Pkg.Revision', ''))
        if 'Pkg.Path' in source and canonical_id(source['Pkg.Path']) != package:
            raise ValueError('source.properties Pkg.Path does not match ' + package)
        xml_path = directory / 'package.xml'
        if xml_path.exists():
            tree = ET.parse(xml_path)
            locals_ = [element for element in tree.iter() if local_name(element) == 'localPackage']
            if len(locals_) != 1 or canonical_id(locals_[0].get('path', '')) != package:
                raise ValueError('package.xml localPackage ID does not match ' + package)
            package_node = locals_[0]
            revisions = [element for element in package_node if local_name(element) == 'revision']
            if len(revisions) != 1:
                raise ValueError('package.xml must contain one revision')
            values = {local_name(item): (item.text or '').strip() for item in revisions[0]}
            if 'major' not in values or any(not value.isdigit() for value in values.values()):
                raise ValueError('package.xml contains an invalid revision')
            if int(values.get('preview', '0')) != 0:
                raise ValueError('preview package cannot satisfy a stable tool version')
            xml_revision = tuple(int(values.get(part, '0')) for part in ('major', 'minor', 'micro'))
            if xml_revision != source_revision:
                raise ValueError('package.xml revision disagrees with source.properties')
        # Legacy archives may lack package.xml. In that case the official
        # directory layout, exact revision/API metadata and tools establish ID.
        parts = package.split(';')
        family = parts[0]
        if family in ('build-tools', 'ndk', 'cmake') and source_revision != revision(parts[1]):
            raise ValueError('installed revision does not match ' + parts[1])
        if family == 'platforms':
            api = parts[1][len('android-'):]
            if source.get('AndroidVersion.ApiLevel') != api:
                raise ValueError('AndroidVersion.ApiLevel does not match ' + api)
            if xml_path.exists():
                api_nodes = [item for item in package_node.iter() if local_name(item) == 'api-level']
                if any((item.text or '').strip() != api for item in api_nodes):
                    raise ValueError('package.xml api-level disagrees with directory/API metadata')
            require_file(directory, 'android.jar')
        elif family == 'platform-tools':
            require_file(directory, 'adb', True)
        elif family == 'build-tools':
            for tool in ('aapt2', 'zipalign'):
                require_file(directory, tool, True)
            require_file(directory, 'lib/d8.jar')
        elif family == 'ndk':
            if host is None:
                raise ValueError('NDK host tool check supports Linux and macOS')
            require_file(directory, 'build/cmake/android.toolchain.cmake')
            for tool in ('clang++', 'llvm-readelf', 'llvm-strip'):
                require_file(directory, 'toolchains/llvm/prebuilt/' + host + '/bin/' + tool, True)
        elif family == 'cmake':
            for tool in ('cmake', 'ninja'):
                require_file(directory, 'bin/' + tool, True)
    except (OSError, ValueError, ET.ParseError) as error:
        failures.append('Android SDK: invalid required package ' + package + ': ' + str(error))

if failures:
    print('\n'.join(failures), file=sys.stderr)
    sys.exit(1)
print('Android SDK: verified metadata and tools for ' + str(len(required)) + ' required packages.')
PY
}

show_log() {
    # CLI 23 can print a progress line for every network chunk. Keep failures
    # readable while preserving their real sdkmanager exit status.
    tail -c 65536 "$1" | tr '\r' '\n' | tail -n 80 >&2
}

if check_metadata >"$temporary/metadata.log" 2>&1; then
    printf 'Required Android SDK packages already installed; skipping license/install calls.\n'
elif [[ $check_only == true ]]; then
    cat "$temporary/metadata.log" >&2
    fail 'SDK metadata/tool check failed; run without --check to install'
else
    printf 'Accepting Android SDK licenses (details shown only on failure).\n'
    # sdkmanager may close stdin as soon as it has enough answers. The yes
    # process then receives SIGPIPE; only sdkmanager decides license success.
    set +e
    yes | "$sdkmanager" "--sdk_root=$sdk_root" --licenses >"$temporary/licenses.log" 2>&1
    license_status=("${PIPESTATUS[@]}")
    set -e
    if [[ ${license_status[1]} != 0 ]]; then
        show_log "$temporary/licenses.log"
        fail "license acceptance failed (sdkmanager exit ${license_status[1]})" "${license_status[1]}"
    fi
    if [[ ${license_status[0]} != 0 && ${license_status[0]} != 141 ]]; then
        fail "license input failed (yes exit ${license_status[0]})" "${license_status[0]}"
    fi
    printf 'Installing explicit SDK packages:\n'
    printf '  %s\n' "${packages[@]}"
    if "$sdkmanager" "--sdk_root=$sdk_root" "${packages[@]}" >"$temporary/install.log" 2>&1; then
        printf 'Android SDK package installation completed.\n'
    else
        status=$?
        show_log "$temporary/install.log"
        fail "package installation failed (sdkmanager exit $status)" "$status"
    fi
    if ! check_metadata >"$temporary/metadata.log" 2>&1; then
        cat "$temporary/metadata.log" >&2
        fail 'SDK metadata/tool check failed after installation'
    fi
fi
cat "$temporary/metadata.log"

sdk_bin=$(dirname -- "$sdkmanager")
if [[ -n ${GITHUB_ENV:-} ]]; then
    printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\n' "$sdk_root" "$sdk_root" >>"$GITHUB_ENV"
fi
if [[ -n ${GITHUB_PATH:-} ]]; then
    printf '%s\n' "$sdk_bin" "$sdk_root/platform-tools" "$sdk_root/cmake/$cmake/bin" >>"$GITHUB_PATH"
fi
printf 'Android SDK package check passed: %s\n' "$sdk_root"
if [[ -z ${GITHUB_ENV:-} ]]; then
    printf 'Local shell: export ANDROID_HOME=%q ANDROID_SDK_ROOT=%q\n' "$sdk_root" "$sdk_root"
fi
if [[ -z ${GITHUB_PATH:-} ]]; then
    printf 'Local shell: export PATH=%q:%q:%q:"$PATH"\n' "$sdk_bin" "$sdk_root/platform-tools" "$sdk_root/cmake/$cmake/bin"
fi
