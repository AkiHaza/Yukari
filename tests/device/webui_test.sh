#!/system/bin/sh
# Run with: adb shell sh /data/local/tmp/webui_test.sh /data/adb/modules/Yukari
# Only the temporary fixture is modified; the installed config is not touched.
set -eu

module=${1:-/data/adb/modules/Yukari}
fixture=$(mktemp -d /data/local/tmp/yukari-webui-test.XXXXXX)
trap 'rm -rf "$fixture"' EXIT
cp "$module/webui.sh" "$fixture/webui.sh"
cp "$module/module.prop" "$fixture/module.prop"
mkdir "$fixture/bin"
cat > "$fixture/bin/am" <<'EOF'
#!/system/bin/sh
[ "$1" = get-current-user ] && echo 0
EOF
cat > "$fixture/bin/pm" <<'EOF'
#!/system/bin/sh
[ "${YUKARI_TEST_PM_FAIL:-0}" = 0 ] || exit 23
printf 'package:com.example.app\npackage:com.other.app\n'
EOF
chmod 0755 "$fixture/bin/am" "$fixture/bin/pm"
export PATH="$fixture/bin:$PATH"

fail() { echo "FAIL: $*" >&2; exit 1; }
backend() { sh "$fixture/webui.sh" "$@"; }
expect_failure() {
    if backend "$@" > "$fixture/stdout" 2> "$fixture/stderr"; then
        fail "command unexpectedly succeeded: $1"
    fi
}

expect_failure info
expect_failure targets
cat > "$fixture/config.json" <<'EOF'
{
  "enabled": false,
  "hide_features": true,
  "hide_resources": false,
  "targets": [
    "com.example.app",
    "com.other.app:worker"
  ]
}
EOF
backend info > "$fixture/info"
grep -qx 'config=ready' "$fixture/info" || fail 'config status'
grep -qx 'enabled=false' "$fixture/info" || fail 'enabled flag'
grep -qx 'hide_features=true' "$fixture/info" || fail 'feature flag'
grep -qx 'hide_resources=false' "$fixture/info" || fail 'resource flag'
grep -qx 'targets=2' "$fixture/info" || fail 'target count'
backend targets > "$fixture/targets"
grep -qx 'com.other.app:worker' "$fixture/targets" || fail 'process target'
[ "$(backend apps | wc -l)" -eq 2 ] || fail 'installed apps'
export YUKARI_TEST_PM_FAIL=1
expect_failure apps
unset YUKARI_TEST_PM_FAIL

payload='{"enabled":true,"hide_features":false,"hide_resources":true,"targets":[]}'
[ "$(backend save "$payload")" = saved ] || fail 'save response'
[ "$(cat "$fixture/config.json")" = "$payload" ] || fail 'save round trip'
[ "$(stat -c %a "$fixture/config.json")" = 644 ] || fail 'saved permissions'
[ -z "$(backend targets)" ] || fail 'empty targets'
expect_failure save 'not JSON'
expect_failure save '{}'
[ "$(cat "$fixture/config.json")" = "$payload" ] || fail 'rejected save changed config'
expect_failure unknown
echo 'WebUI Android backend tests passed'
