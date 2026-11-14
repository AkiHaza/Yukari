#!/system/bin/sh
# Yukari action script.
#
# Magisk has no built-in WebUI, so the action button first tries to open the
# module's webroot in an installed WebUI host. Without a host it falls back to
# the original terminal flow, which rewrites config.json with every installed
# third-party package enabled.

MODULE_ID="Yukari"
MODULE_DIR="/data/adb/modules/$MODULE_ID"
CONFIG="$MODULE_DIR/config.json"
TEMP_CONFIG="$MODULE_DIR/config.json.tmp"
PACKAGE_LIST="$MODULE_DIR/targets.tmp"

if [ "$(id -u)" != "0" ]; then
    echo "Error: Root required"
    exit 1
fi

if [ ! -d "$MODULE_DIR" ]; then
    echo "Error: Yukari module not found at $MODULE_DIR"
    exit 1
fi

launch_webui() {
    if pm path io.github.a13e300.ksuwebui > /dev/null 2>&1; then
        echo "Launching WebUI in KSU WebUI Standalone..."
        am start -n "io.github.a13e300.ksuwebui/.WebUIActivity" -e id "$MODULE_ID" > /dev/null 2>&1 && return 0
        echo "! Failed to start io.github.a13e300.ksuwebui"
    fi
    if pm path com.dergoogler.mmrl.wx > /dev/null 2>&1; then
        echo "Launching WebUI in WebUI X..."
        am start -n "com.dergoogler.mmrl.wx/.ui.activity.webui.WebUIActivity" -e MOD_ID "$MODULE_ID" > /dev/null 2>&1 && return 0
        echo "! Failed to start com.dergoogler.mmrl.wx"
    fi
    return 1
}

# Preserve existing settings for the terminal fallback.
ENABLED="true"
HIDE_FEATURES="true"
HIDE_RESOURCES="true"
if [ -f "$CONFIG" ]; then
    grep -q '"enabled"[[:space:]]*:[[:space:]]*false' "$CONFIG" && ENABLED="false"
    grep -q '"hide_features"[[:space:]]*:[[:space:]]*false' "$CONFIG" && HIDE_FEATURES="false"
    grep -q '"hide_resources"[[:space:]]*:[[:space:]]*false' "$CONFIG" && HIDE_RESOURCES="false"
fi

if launch_webui; then
    echo "Done. Configure targets in the WebUI."
    exit 0
fi

echo "No WebUI host found."
echo "  Install KSU WebUI Standalone to manage targets graphically:"
echo "  https://github.com/KOWX712/KsuWebUIStandalone/releases"
echo
echo "  Falling back to the terminal flow (all third-party apps become targets)."
echo

CURRENT_USER="$(am get-current-user 2>/dev/null)"
[ -z "$CURRENT_USER" ] && CURRENT_USER="0"

pm list packages -3 --user "$CURRENT_USER" 2>/dev/null |
    sed -n 's/^package://p' |
    sort -u > "$PACKAGE_LIST"

PACKAGE_COUNT="$(wc -l < "$PACKAGE_LIST" | tr -d ' ')"

{
    echo "{"
    echo "  \"enabled\": $ENABLED,"
    echo "  \"hide_features\": $HIDE_FEATURES,"
    echo "  \"hide_resources\": $HIDE_RESOURCES,"
    echo "  \"targets\": ["

    INDEX=0
    while IFS= read -r PACKAGE_NAME; do
        [ -z "$PACKAGE_NAME" ] && continue
        INDEX=$((INDEX + 1))
        if [ "$INDEX" -lt "$PACKAGE_COUNT" ]; then
            printf '    "%s",\n' "$PACKAGE_NAME"
        else
            printf '    "%s"\n' "$PACKAGE_NAME"
        fi
    done < "$PACKAGE_LIST"

    echo "  ]"
    echo "}"
} > "$TEMP_CONFIG"

chmod 0644 "$TEMP_CONFIG"
mv -f "$TEMP_CONFIG" "$CONFIG"
rm -f "$PACKAGE_LIST"

echo "Yukari config updated."
echo "  enabled: $ENABLED"
echo "  user: $CURRENT_USER"
echo "  targets: $PACKAGE_COUNT app(s)"
echo "  path: $CONFIG"
echo
echo "Force-stop target apps or reboot to apply."
