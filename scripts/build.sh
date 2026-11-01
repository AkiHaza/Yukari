#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mode=${1:-all}
if [[ $# -gt 1 || ! $mode =~ ^(test|android|package|all)$ ]]; then
    echo 'usage: bash scripts/build.sh [test|android|package|all]' >&2
    exit 2
fi
cd -- "$root"
mkdir -p -- "$root/out/diagnostics"
diagnostics=$(mktemp -d "$root/out/diagnostics/$mode.XXXXXXXX")
trap 'status=$?; if (( status != 0 )); then printf "Build stopped (exit %s); logs: %s\n" "$status" "$diagnostics" >&2; fi' EXIT

phase() {
    local name=$1
    shift
    printf '\n[%s]\n' "$name"
    if "$@" 2>&1 | tee "$diagnostics/$name.log"; then return 0; else return $?; fi
}

source_info() {
    local actual changes
    actual=$(git rev-parse HEAD) || return $?
    git log -1 --format='commit=%H%nsubject=%s' || return $?
    printf 'requested_ref=%s\nrequested_sha=%s\n' "${GITHUB_REF:-local}" "${GITHUB_SHA:-local}"
    if [[ -n ${GITHUB_SHA:-} && $actual != "$GITHUB_SHA" ]]; then
        echo 'Checkout does not match the requested GitHub commit.' >&2
        return 1
    fi
    changes=$(git status --porcelain) || return $?
    if [[ -n $changes ]]; then echo 'working_tree=modified'; else echo 'working_tree=clean'; fi
}

test_host() {
    phase native-tests bash "$root/tests/native/run.sh"
    phase build-tests bash "$root/tests/build/run.sh"
}

check_java() {
    if [[ -n ${JAVA_HOME:-} ]]; then
        [[ -x $JAVA_HOME/bin/java ]] || { echo 'JAVA_HOME must contain a JDK.' >&2; return 1; }
        export PATH="$JAVA_HOME/bin:$PATH"
    fi
    command -v java > /dev/null || { echo 'Install JDK 17 and set JAVA_HOME.' >&2; return 1; }
    local major
    major=$(java -XshowSettings:properties -version 2>&1 | awk '$1 == "java.specification.version" {print $3}')
    [[ $major == 17 ]] || { echo "Android builds require JDK 17; found $major. Set JAVA_HOME." >&2; return 1; }
    java -version
}

package_module() {
    local overrides=()
    if [[ ${GITHUB_ACTIONS:-} == true ]]; then
        local base version short_sha
        base=$(awk -F= '$1 == "version" {print substr($0, index($0, "=") + 1)}' module/module.prop) || return $?
        short_sha=${GITHUB_SHA:0:7}
        if [[ ${GITHUB_REF_TYPE:-} == tag ]]; then
            version=${GITHUB_REF_NAME#v}
        elif [[ ${GITHUB_REF_NAME:-} == dev ]]; then
            version="$base-dev (${GITHUB_RUN_NUMBER}-$short_sha)"
        else
            version="$base (${GITHUB_RUN_NUMBER}-$short_sha)"
        fi
        overrides=("YUKARI_MODULE_VERSION=$version" "YUKARI_MODULE_VERSION_CODE=$GITHUB_RUN_NUMBER")
    fi
    env "${overrides[@]}" bash "$root/scripts/package.sh"
}

build_android() {
    phase java check_java
    phase android-sdk bash "$root/scripts/android-sdk.sh"
    # The preparation script validates conflicting roots before normalizing.
    export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$ANDROID_HOME}"
    export ANDROID_HOME="$ANDROID_SDK_ROOT"
    phase gradle bash "$root/gradlew" --no-daemon --max-workers=2 --stacktrace :module:assembleRelease
    phase package package_module
}

phase source source_info
case $mode in
    test) test_host ;;
    android) build_android ;;
    package) phase package package_module ;;
    all) test_host; build_android ;;
esac
printf 'Build completed; logs: %s\n' "$diagnostics"
