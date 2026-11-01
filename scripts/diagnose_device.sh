#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo 'usage: diagnose_device.sh package.name [adb-serial]' >&2
    exit 2
fi
package=$1
if [[ ! $package =~ ^[a-zA-Z0-9_]+(.[a-zA-Z0-9_]+)+$ ]]; then
    echo 'Expected an Android package name, without a process suffix.' >&2
    exit 2
fi
adb=${ADB:-adb}
device=()
if [[ $# == 2 ]]; then device=(-s "$2"); fi
"$adb" "${device[@]}" get-state > /dev/null
echo '[device] Android API / ABI'
"$adb" "${device[@]}" shell getprop ro.build.version.sdk
"$adb" "${device[@]}" shell getprop ro.product.cpu.abi
echo '[module] Installed configuration (requires root)'
"$adb" "${device[@]}" shell "su -c 'cat /data/adb/modules/Yukari/config.json'"
echo '[processes] Target and private processes'
processes=$("$adb" "${device[@]}" shell ps -A -o UID,PID,NAME | tr -d '\r' |
    awk -v package="$package" '$3 == package || index($3, package ":") == 1')
if [[ -z $processes ]]; then
    echo 'Target is not running. Launch it, then run this script again.' >&2
    exit 1
fi
printf '%s\n' "$processes"
while read -r uid pid name; do
    [[ $pid =~ ^[0-9]+$ ]] || continue
    printf '[logs] %s (pid %s, uid %s)\n' "$name" "$pid" "$uid"
    "$adb" "${device[@]}" logcat -d -v brief --pid="$pid" -s Yukari:I '*:S' | tail -n 80
done <<< "$processes"
echo 'These are loading diagnostics. Verify API behavior inside the target app using tests/device/README.md.'
