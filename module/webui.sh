#!/system/bin/sh
# Yukari WebUI backend.
#
# The WebUI never parses or rewrites JSON itself: it asks this script for a
# plain key=value snapshot and hands a complete config back through `save`.
# Every subcommand is read-only except `save` and `force-stop`.

MODDIR=${0%/*}
CONFIG="$MODDIR/config.json"
PROP="$MODDIR/module.prop"

say() {
    printf '%s\n' "$1"
}

fail() {
    printf 'error: %s\n' "$1" >&2
    exit 1
}

bool_of() {
    # Prints false only for an explicit `"key": false`, matching the loader.
    if [ -f "$CONFIG" ] && grep -q "\"$1\"[[:space:]]*:[[:space:]]*false" "$CONFIG" 2>/dev/null; then
        say false
    else
        say true
    fi
}

prop_of() {
    [ -f "$PROP" ] || return 0
    sed -n "s/^$1=//p" "$PROP" | head -n 1
}

list_targets() {
    [ -f "$CONFIG" ] || return 0
    # Works for both pretty-printed and single-line configs: flatten, take the
    # targets array, then emit one quoted entry per line.
    tr '\n' ' ' < "$CONFIG" |
        sed -n 's/.*"targets"[[:space:]]*:[[:space:]]*\[\([^]]*\)\].*/\1/p' |
        tr ',' '\n' |
        sed -n 's/^[[:space:]]*"\([^"]*\)".*/\1/p'
}

current_user() {
    local user
    user=$(am get-current-user 2>/dev/null)
    [ -n "$user" ] || user=0
    say "$user"
}

case "${1:-info}" in
    info)
        if [ -f "$CONFIG" ]; then say "config=ready"; else say "config=missing"; fi
        say "enabled=$(bool_of enabled)"
        say "hide_features=$(bool_of hide_features)"
        say "hide_resources=$(bool_of hide_resources)"
        say "targets=$(list_targets | grep -c .)"
        say "user=$(current_user)"
        say "version=$(prop_of version)"
        say "code=$(prop_of versionCode)"
        say "author=$(prop_of author)"
        say "path=$CONFIG"
        ;;
    targets)
        list_targets
        ;;
    apps)
        # Third-party packages only: system app IDs are excluded by the hook.
        pm list packages -3 --user "$(current_user)" 2>/dev/null |
            sed -n 's/^package://p' |
            sort -u
        ;;
    save)
        payload=${2:-}
        case "$payload" in
            \{*) ;;
            *) fail "payload must be a JSON object" ;;
        esac
        case "$payload" in
            *"'"*) fail "payload must not contain single quotes" ;;
        esac
        printf '%s' "$payload" | grep -q '"targets"' || fail "payload requires a targets array"
        printf '%s\n' "$payload" > "$CONFIG.tmp" || fail "cannot write $CONFIG.tmp"
        chmod 0644 "$CONFIG.tmp" || fail "cannot chmod $CONFIG.tmp"
        chown 0:0 "$CONFIG.tmp" 2>/dev/null
        chcon u:object_r:system_file:s0 "$CONFIG.tmp" 2>/dev/null
        mv -f "$CONFIG.tmp" "$CONFIG" || fail "cannot replace $CONFIG"
        say "saved"
        ;;
    force-stop)
        # Package names never contain whitespace, so field splitting is safe.
        stopped=0
        for target in $(list_targets); do
            case "$target" in ''|*[!A-Za-z0-9._:]*) continue ;; esac
            if am force-stop "$target" >/dev/null 2>&1; then
                stopped=$((stopped + 1))
            fi
        done
        say "stopped=$stopped"
        ;;
    *)
        fail "unknown command: $1"
        ;;
esac
