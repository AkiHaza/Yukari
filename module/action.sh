#!/system/bin/sh

# POSIX sh only: compatible with Android mksh/toybox.
MODULE_DIR="/data/adb/modules/Yukari"
CONFIG="$MODULE_DIR/config.json"
PID="$$"
TEMP_CONFIG="$MODULE_DIR/.config.json.tmp.$PID"
PACKAGE_LIST="$MODULE_DIR/.targets.$PID"
USER_LIST="$MODULE_DIR/.users.$PID"
SELECTED_LIST="$MODULE_DIR/.selected.$PID"
EXISTING_LIST="$MODULE_DIR/.existing.$PID"
EXISTING_RAW="$MODULE_DIR/.existing.raw.$PID"

cleanup() {
    rm -f "$TEMP_CONFIG" "$PACKAGE_LIST" "$USER_LIST" "$SELECTED_LIST" "$EXISTING_LIST" "$EXISTING_RAW" \
        "$USER_LIST.sorted" "$PACKAGE_LIST.sorted" "$SELECTED_LIST.sorted"
}
trap cleanup 0
trap 'cleanup; exit 1' 1 2 3 15
fail() { echo "Yukari: $*" >&2; exit 1; }
sort_unique() {
    SORTED="$1.sorted"
    sort -u "$1" > "$SORTED" || return 1
    mv -f "$SORTED" "$1"
}
sort_numeric_unique() {
    SORTED="$1.sorted"
    sort -n -u "$1" > "$SORTED" || return 1
    mv -f "$SORTED" "$1"
}
[ "$(id -u 2>/dev/null)" = 0 ] || fail "root is required"
[ -d "$MODULE_DIR" ] || fail "module not found at $MODULE_DIR"

# Whitespace-insensitive scalar parsing preserves hand-edited enabled state.
ENABLED="true"
FORCE_DENYLIST_UNMOUNT="true"
if [ -f "$CONFIG" ]; then
    COMPACT_CONFIG="$(tr -d '[:space:]' < "$CONFIG" 2>/dev/null)" || fail "cannot read existing config"
    ENABLED_VALUE="$(printf '%s\n' "$COMPACT_CONFIG" | awk '{ if ($0 ~ /"enabled":false/) {print "false"; exit} if ($0 ~ /"enabled":true/) {print "true"; exit} }')"
    case "$ENABLED_VALUE" in false|true) ENABLED="$ENABLED_VALUE" ;; esac
    FORCE_VALUE="$(printf '%s\n' "$COMPACT_CONFIG" | awk '{ if ($0 ~ /"force_denylist_unmount":false/) {print "false"; exit} if ($0 ~ /"force_denylist_unmount":true/) {print "true"; exit} }')"
    case "$FORCE_VALUE" in false|true) FORCE_DENYLIST_UNMOUNT="$FORCE_VALUE" ;; esac
fi

# Preserve existing targets, including manually added packages not installed now.
: > "$EXISTING_LIST" || fail "cannot create temporary files"
if [ -f "$CONFIG" ]; then
    if ! printf '%s\n' "$COMPACT_CONFIG" | awk '
      BEGIN { found=0; complete=0 }
      {
        marker="\"targets\":["
        start=index($0, marker)
        if (start == 0) exit 1
        found=1
        payload=substr($0, start + length(marker))
        finish=index(payload, "]")
        if (finish == 0) exit 1
        complete=1
        payload=substr(payload, 1, finish - 1)
        while (match(payload,/"[A-Za-z][A-Za-z0-9._-]*"/)) {
          print substr(payload, RSTART + 1, RLENGTH - 2)
          payload=substr(payload, RSTART + RLENGTH)
        }
        exit
      }
      END { if (!found || !complete) exit 1 }
    ' > "$EXISTING_RAW"; then
        fail "cannot parse existing targets"
    fi
    sort -u "$EXISTING_RAW" > "$EXISTING_LIST" || fail "cannot sort existing targets"
fi

# Enumerate all users (owner, secondary users and work profiles).
: > "$USER_LIST" || fail "cannot create user list"
if command -v cmd >/dev/null 2>&1; then cmd user list 2>/dev/null | sed -n 's/.*UserInfo{\([0-9][0-9]*\):.*/\1/p' >> "$USER_LIST"; fi
if command -v pm >/dev/null 2>&1; then pm list users 2>/dev/null | sed -n 's/.*UserInfo{\([0-9][0-9]*\):.*/\1/p' >> "$USER_LIST"; fi
CURRENT_USER="$(am get-current-user 2>/dev/null)"
case "$CURRENT_USER" in ''|*[!0-9]*) CURRENT_USER=0 ;; esac
echo "$CURRENT_USER" >> "$USER_LIST"
sort_numeric_unique "$USER_LIST" || fail "cannot sort users"

: > "$PACKAGE_LIST" || fail "cannot create package list"
while IFS= read -r USER_ID; do
    case "$USER_ID" in ''|*[!0-9]*) continue ;; esac
    pm list packages -3 --user "$USER_ID" 2>/dev/null | sed -n 's/^package://p' >> "$PACKAGE_LIST"
done < "$USER_LIST"
sort_unique "$PACKAGE_LIST" || fail "cannot sort packages"
PACKAGE_COUNT="$(wc -l < "$PACKAGE_LIST" | tr -d '[:space:]')"
case "$PACKAGE_COUNT" in ''|*[!0-9]*) PACKAGE_COUNT=0 ;; esac

append_numbers() {
    NUMBERS="$(printf '%s' "$1" | tr ',' ' ')"
    for NUMBER in $NUMBERS; do
        case "$NUMBER" in ''|*[!0-9]*) return 1 ;; esac
        [ "$NUMBER" -ge 1 ] 2>/dev/null && [ "$NUMBER" -le "$PACKAGE_COUNT" ] 2>/dev/null || return 1
        sed -n "${NUMBER}p" "$PACKAGE_LIST" >> "$SELECTED_LIST"
    done
}

: > "$SELECTED_LIST" || fail "cannot create selection list"
INTERACTIVE=0; [ -r /dev/tty ] && [ -w /dev/tty ] && INTERACTIVE=1
if [ "$INTERACTIVE" -eq 1 ]; then
    echo "Yukari target selector (users: $(tr '\n' ' ' < "$USER_LIST"))" >&2
    echo "Third-party packages discovered: $PACKAGE_COUNT" >&2
    INDEX=0
    while IFS= read -r PACKAGE_NAME; do [ -n "$PACKAGE_NAME" ] || continue; INDEX=$((INDEX+1)); printf '  %s) %s\n' "$INDEX" "$PACKAGE_NAME" >&2; done < "$PACKAGE_LIST"
    [ "$PACKAGE_COUNT" -gt 0 ] || echo "  (none)" >&2
    echo "Choose: a=all (merge), s=select (merge), r=replace, k=keep, q=cancel" >&2
    printf '> ' >&2; IFS= read -r CHOICE < /dev/tty || CHOICE=""
else
    echo "No interactive terminal; preserving existing targets." >&2; CHOICE=k
fi

case "$CHOICE" in
    q|Q) echo "Yukari: cancelled; existing config was not changed." >&2; exit 0 ;;
    ''|k|K) cp "$EXISTING_LIST" "$SELECTED_LIST" || fail "cannot preserve targets" ;;
    a|A|all|ALL) cp "$PACKAGE_LIST" "$SELECTED_LIST" || fail "cannot select all"; cat "$EXISTING_LIST" >> "$SELECTED_LIST" || fail "cannot merge targets" ;;
    s|S|r|R)
        NUMBERS=""; [ "$INTERACTIVE" -eq 1 ] && { printf 'Enter numbers (space/comma separated): ' >&2; IFS= read -r NUMBERS < /dev/tty || NUMBERS=""; }
        : > "$SELECTED_LIST"; append_numbers "$NUMBERS" || fail "invalid package selection"
        case "$CHOICE" in s|S) cat "$EXISTING_LIST" >> "$SELECTED_LIST" || fail "cannot merge targets" ;; esac ;;
    *) : > "$SELECTED_LIST"; append_numbers "$CHOICE" || fail "invalid choice"; cat "$EXISTING_LIST" >> "$SELECTED_LIST" || fail "cannot merge targets" ;;
esac
sort_unique "$SELECTED_LIST" || fail "cannot sort targets"
TARGET_COUNT="$(grep -c '^[A-Za-z][A-Za-z0-9._-]*$' "$SELECTED_LIST" 2>/dev/null || true)"
case "$TARGET_COUNT" in ''|*[!0-9]*) TARGET_COUNT=0 ;; esac

{
    printf '{\n  "enabled": %s,\n  "force_denylist_unmount": %s,\n  "targets": [\n' "$ENABLED" "$FORCE_DENYLIST_UNMOUNT"
    INDEX=0
    while IFS= read -r PACKAGE_NAME; do
        case "$PACKAGE_NAME" in ''|*[!A-Za-z0-9._-]*) continue ;; esac
        INDEX=$((INDEX+1)); if [ "$INDEX" -lt "$TARGET_COUNT" ]; then printf '    "%s",\n' "$PACKAGE_NAME"; else printf '    "%s"\n' "$PACKAGE_NAME"; fi
    done < "$SELECTED_LIST"
    printf '  ]\n}\n'
} > "$TEMP_CONFIG" || fail "cannot write temporary config"
[ -s "$TEMP_CONFIG" ] || fail "temporary config is empty"
grep -q '^[[:space:]]*"enabled":' "$TEMP_CONFIG" || fail "temporary config validation failed"
grep -q '^[[:space:]]*"force_denylist_unmount":' "$TEMP_CONFIG" || fail "temporary config validation failed"
grep -q '^[[:space:]]*"targets":' "$TEMP_CONFIG" || fail "temporary config validation failed"
chmod 0644 "$TEMP_CONFIG" || fail "cannot set config permissions"
mv -f "$TEMP_CONFIG" "$CONFIG" || fail "cannot atomically replace config"

echo "Yukari config updated."
echo "  enabled: $ENABLED"
echo "  force_denylist_unmount: $FORCE_DENYLIST_UNMOUNT"
echo "  users: $(tr '\n' ' ' < "$USER_LIST")"
echo "  discovered packages: $PACKAGE_COUNT"
echo "  targets: $TARGET_COUNT"
echo "  path: $CONFIG"
echo
echo "Force-stop target apps or reboot to apply."
