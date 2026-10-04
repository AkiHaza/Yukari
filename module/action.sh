#!/system/bin/sh

MODULE_DIR="/data/adb/modules/Yukari"
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

# Preserve existing settings
ENABLED="true"
ENHANCED="false"
if [ -f "$CONFIG" ]; then
    grep -q '"enabled"[[:space:]]*:[[:space:]]*false' "$CONFIG" && ENABLED="false"
    grep -q '"force_denylist_unmount"[[:space:]]*:[[:space:]]*true' "$CONFIG" && ENHANCED="true"
fi

CURRENT_USER="$(am get-current-user 2>/dev/null)"
[ -z "$CURRENT_USER" ] && CURRENT_USER="0"

pm list packages -3 --user "$CURRENT_USER" 2>/dev/null |
    sed -n 's/^package://p' |
    sort -u > "$PACKAGE_LIST"

PACKAGE_COUNT="$(wc -l < "$PACKAGE_LIST" | tr -d ' ')"

{
    echo "{"
    echo "  \"enabled\": $ENABLED,"
    echo "  \"force_denylist_unmount\": $ENHANCED,"
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
echo "  force_denylist_unmount: $ENHANCED"
echo "  user: $CURRENT_USER"
echo "  targets: $PACKAGE_COUNT app(s)"
echo "  path: $CONFIG"
echo
echo "Force-stop target apps or reboot to apply."
