#!/usr/bin/env sh
# Lightweight repository wrapper. CI installs a pinned Gradle distribution via
# gradle/actions/setup-gradle; local users may provide `gradle` on PATH.
set -eu

if command -v gradle >/dev/null 2>&1; then
    exec gradle "$@"
fi

echo "gradlew: Gradle was not found on PATH." >&2
echo "Install Gradle 8.11.1 or run this project in CI with setup-gradle." >&2
exit 127
