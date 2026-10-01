#!/system/bin/sh

# POSIX sh only: compatible with Android mksh/toybox.
MODULE_DIR="/data/adb/modules/Yukari"
umask 077
CONFIG="$MODULE_DIR/config.json"
PID="$$"
TEMP_CONFIG="$MODULE_DIR/.config.json.tmp.$PID"
PACKAGE_LIST="$MODULE_DIR/.targets.$PID"
USER_LIST="$MODULE_DIR/.users.$PID"
SELECTED_LIST="$MODULE_DIR/.selected.$PID"
EXISTING_LIST="$MODULE_DIR/.existing.$PID"
EXISTING_RAW="$MODULE_DIR/.existing.raw.$PID"
KEY_EVENT_FILE="$MODULE_DIR/.key-event.$PID"

cleanup() {
    if [ -n "${EVENT_PID:-}" ]; then
        kill "$EVENT_PID" 2>/dev/null || :
        wait "$EVENT_PID" 2>/dev/null || :
    fi
    rm -f "$TEMP_CONFIG" "$PACKAGE_LIST" "$USER_LIST" "$SELECTED_LIST" "$EXISTING_LIST" "$EXISTING_RAW" "$KEY_EVENT_FILE" \
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

ENABLED="true"
FORCE_DENYLIST_UNMOUNT="true"
: > "$EXISTING_LIST" || fail "cannot create temporary files"
if [ -f "$CONFIG" ]; then
    if ! awk '
        function skip_space( character) {
            while (position <= length(document)) {
                character = substr(document, position, 1)
                if (character != " " && character != "\t" && character != "\r" && character != "\n") break
                position++
            }
        }
        function consume(expected) {
            if (substr(document, position, length(expected)) != expected) return 0
            position += length(expected)
            return 1
        }
        function read_string( character) {
            if (!consume("\"")) return 0
            parsed = ""
            while (position <= length(document)) {
                character = substr(document, position, 1)
                position++
                if (character == "\"") return 1
                if (character == "\\" || character ~ /[[:cntrl:]]/) return 0
                parsed = parsed character
            }
            return 0
        }
        function read_boolean() {
            if (consume("true")) { parsed = "true"; return 1 }
            if (consume("false")) { parsed = "false"; return 1 }
            return 0
        }
        function read_targets( target) {
            if (!consume("[")) return 0
            skip_space()
            if (consume("]")) return 1
            while (1) {
                if (!read_string()) return 0
                target = parsed
                if (target !~ /^[A-Za-z][A-Za-z0-9._-]*$/) return 0
                print "target=" target
                skip_space()
                if (consume("]")) return 1
                if (!consume(",")) return 0
                skip_space()
            }
        }
        function read_object( key) {
            skip_space()
            if (!consume("{")) return 0
            skip_space()
            if (consume("}")) return 0
            while (1) {
                if (!read_string()) return 0
                key = parsed
                if (seen[key]++) return 0
                skip_space()
                if (!consume(":")) return 0
                skip_space()
                if (key == "enabled") {
                    if (!read_boolean()) return 0
                    enabled = parsed
                } else if (key == "force_denylist_unmount") {
                    if (!read_boolean()) return 0
                    force_denylist_unmount = parsed
                } else if (key == "targets") {
                    if (!read_targets()) return 0
                } else return 0
                skip_space()
                if (consume("}")) break
                if (!consume(",")) return 0
                skip_space()
            }
            skip_space()
            return seen["targets"] && position > length(document)
        }
        { document = document $0 "\n" }
        END {
            position = 1
            enabled = "true"
            force_denylist_unmount = "true"
            if (!read_object()) exit 1
            print "enabled=" enabled
            print "force_denylist_unmount=" force_denylist_unmount
        }
    ' "$CONFIG" > "$EXISTING_RAW"; then
        fail "existing config is invalid or uses unsupported JSON; keeping it unchanged"
    fi
    ENABLED="$(sed -n 's/^enabled=//p' "$EXISTING_RAW")"
    FORCE_DENYLIST_UNMOUNT="$(sed -n 's/^force_denylist_unmount=//p' "$EXISTING_RAW")"
    sed -n 's/^target=//p' "$EXISTING_RAW" > "$EXISTING_LIST" || fail "cannot read existing targets"
    sort_unique "$EXISTING_LIST" || fail "cannot sort existing targets"
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
    pm list packages -3 --user "$USER_ID" 2>/dev/null | sed -n 's/^package:\([A-Za-z][A-Za-z0-9._-]*\)$/\1/p' >> "$PACKAGE_LIST"
done < "$USER_LIST"
sort_unique "$PACKAGE_LIST" || fail "cannot sort packages"
PACKAGE_COUNT="$(wc -l < "$PACKAGE_LIST" | tr -d '[:space:]')"
case "$PACKAGE_COUNT" in ''|*[!0-9]*) PACKAGE_COUNT=0 ;; esac

append_numbers() {
    NUMBERS="$(printf '%s' "$1" | tr ',' ' ')"
    [ -n "$NUMBERS" ] || return 1
    for NUMBER in $NUMBERS; do
        case "$NUMBER" in ''|*[!0-9]*) return 1 ;; esac
        [ "$NUMBER" -ge 1 ] 2>/dev/null && [ "$NUMBER" -le "$PACKAGE_COUNT" ] 2>/dev/null || return 1
        sed -n "${NUMBER}p" "$PACKAGE_LIST" >> "$SELECTED_LIST" || return 1
    done
}

wait_volume_key() {
    ELAPSED=0
    while [ "$ELAPSED" -lt 20 ]; do
        : > "$KEY_EVENT_FILE" || return 1
        getevent -qlc 1 > "$KEY_EVENT_FILE" 2>/dev/null &
        EVENT_PID=$!
        while kill -0 "$EVENT_PID" 2>/dev/null && [ "$ELAPSED" -lt 20 ]; do
            sleep 1
            ELAPSED=$((ELAPSED+1))
        done
        if kill -0 "$EVENT_PID" 2>/dev/null; then kill "$EVENT_PID" 2>/dev/null || :; fi
        EVENT_STATUS=0
        wait "$EVENT_PID" 2>/dev/null || EVENT_STATUS=$?
        EVENT_PID=""
        [ "$EVENT_STATUS" -eq 0 ] || return 1
        KEY_RESULT="$(awk '
            /KEY_VOLUMEUP/ && ($NF == "DOWN" || $NF == "00000001") { print "up"; exit }
            /KEY_VOLUMEDOWN/ && ($NF == "DOWN" || $NF == "00000001") { print "down"; exit }
        ' "$KEY_EVENT_FILE")"
        case "$KEY_RESULT" in up|down) return 0 ;; esac
        sleep 1
        ELAPSED=$((ELAPSED+1))
    done
    return 1
}

: > "$SELECTED_LIST" || fail "cannot create selection list"
INTERACTIVE=0
if ( : < /dev/tty ) 2>/dev/null; then INTERACTIVE=1; fi
if [ "$INTERACTIVE" -eq 1 ]; then
    echo "Yukari target selector (users: $(tr '\n' ' ' < "$USER_LIST"))" >&2
    echo "Third-party packages discovered: $PACKAGE_COUNT" >&2
    INDEX=0
    while IFS= read -r PACKAGE_NAME; do [ -n "$PACKAGE_NAME" ] || continue; INDEX=$((INDEX+1)); printf '  %s) %s\n' "$INDEX" "$PACKAGE_NAME" >&2; done < "$PACKAGE_LIST"
    [ "$PACKAGE_COUNT" -gt 0 ] || echo "  (none)" >&2
    echo "Choose: a=all (merge), s=select (merge), r=replace, k=keep, q=cancel" >&2
    printf '> ' >&2; IFS= read -r CHOICE < /dev/tty || CHOICE=q
else
    if ! command -v getevent >/dev/null 2>&1; then
        echo "Yukari: no terminal or volume-key input; config unchanged." >&2
        exit 0
    fi
    echo "Volume +: add all discovered packages; Volume -: select packages one by one." >&2
    echo "Wait 20 seconds to cancel without changing the config." >&2
    if ! wait_volume_key; then
        echo "Yukari: timed out; config unchanged." >&2
        exit 0
    fi
    case "$KEY_RESULT" in up) CHOICE=a ;; down) CHOICE=v ;; esac
fi

case "$CHOICE" in
    q|Q) echo "Yukari: cancelled; existing config was not changed." >&2; exit 0 ;;
    ''|k|K) echo "Yukari: existing config was not changed." >&2; exit 0 ;;
    a|A|all|ALL) cp "$PACKAGE_LIST" "$SELECTED_LIST" || fail "cannot select all"; cat "$EXISTING_LIST" >> "$SELECTED_LIST" || fail "cannot merge targets" ;;
    s|S|r|R)
        NUMBERS=""; [ "$INTERACTIVE" -eq 1 ] && { printf 'Enter numbers (space/comma separated): ' >&2; IFS= read -r NUMBERS < /dev/tty || NUMBERS=""; }
        [ -n "$NUMBERS" ] || { echo "Yukari: no packages selected; config unchanged." >&2; exit 0; }
        : > "$SELECTED_LIST"; append_numbers "$NUMBERS" || fail "invalid package selection"
        case "$CHOICE" in s|S) cat "$EXISTING_LIST" >> "$SELECTED_LIST" || fail "cannot merge targets" ;; esac ;;
    v)
        while IFS= read -r PACKAGE_NAME; do
            [ -n "$PACKAGE_NAME" ] || continue
            printf 'Include %s? Volume + yes, Volume - no (20 seconds to cancel):\n' "$PACKAGE_NAME" >&2
            if ! wait_volume_key; then
                echo "Yukari: timed out; config unchanged." >&2
                exit 0
            fi
            if [ "$KEY_RESULT" = up ]; then
                printf '%s\n' "$PACKAGE_NAME" >> "$SELECTED_LIST" || fail "cannot record package selection"
            fi
        done < "$PACKAGE_LIST"
        [ -s "$SELECTED_LIST" ] || { echo "Yukari: no packages selected; config unchanged." >&2; exit 0; }
        cat "$EXISTING_LIST" >> "$SELECTED_LIST" || fail "cannot merge targets" ;;
    *) : > "$SELECTED_LIST"; append_numbers "$CHOICE" || fail "invalid choice"; cat "$EXISTING_LIST" >> "$SELECTED_LIST" || fail "cannot merge targets" ;;
esac
sort_unique "$SELECTED_LIST" || fail "cannot sort targets"
TARGET_COUNT="$(grep -c '^[A-Za-z][A-Za-z0-9._-]*$' "$SELECTED_LIST" 2>/dev/null || true)"
case "$TARGET_COUNT" in ''|*[!0-9]*) TARGET_COUNT=0 ;; esac

{
    printf '{\n  "enabled": %s,\n  "force_denylist_unmount": %s,\n  "targets": [\n' "$ENABLED" "$FORCE_DENYLIST_UNMOUNT" || fail "cannot write config header"
    INDEX=0
    while IFS= read -r PACKAGE_NAME; do
        case "$PACKAGE_NAME" in ''|*[!A-Za-z0-9._-]*) continue ;; esac
        INDEX=$((INDEX+1))
        if [ "$INDEX" -lt "$TARGET_COUNT" ]; then
            printf '    "%s",\n' "$PACKAGE_NAME" || fail "cannot write config target"
        else
            printf '    "%s"\n' "$PACKAGE_NAME" || fail "cannot write config target"
        fi
    done < "$SELECTED_LIST"
    printf '  ]\n}\n' || fail "cannot complete config"
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
