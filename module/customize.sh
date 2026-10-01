#!/system/bin/sh
# Yukari Guard - Installation Script

ui_print "==============================="
ui_print " Yukari Guard Installer"
ui_print "==============================="

OLD_CONFIG="/data/adb/modules/Yukari/config.json"
# MODPATH is supplied by Magisk and is writable throughout installation;
# keeping the backup beside the staged module avoids relying on TMPDIR being
# exported by a recovery shell.
BACKUP_CONFIG="$MODPATH/.yukari_config_backup.$$"
KEEP_CONFIG=0

# Check for previous installation
if [ -f "$OLD_CONFIG" ]; then
    ui_print "- Previous Yukari installation detected"
    ui_print ""
    ui_print "  Keep existing config?"
    ui_print "  [Volume +] = Yes"
    ui_print "  [Volume -] = No (default)"
    ui_print "  Waiting up to 20 seconds..."
    ui_print ""

    # Volume key detection with 20s timeout
    (
        while true; do
            getevent -qlc 1 2>/dev/null | grep -q "KEY_VOLUMEUP" && exit 1
            getevent -qlc 1 2>/dev/null | grep -q "KEY_VOLUMEDOWN" && exit 2
        done
    ) &
    MONITOR_PID=$!

    elapsed=0
    while [ "$elapsed" -lt 20 ]; do
        if ! kill -0 "$MONITOR_PID" 2>/dev/null; then
            break
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done

    kill "$MONITOR_PID" 2>/dev/null
    wait "$MONITOR_PID" 2>/dev/null
    KEY_RESULT=$?

    case $KEY_RESULT in
        1)
            KEEP_CONFIG=1
            if cp "$OLD_CONFIG" "$BACKUP_CONFIG"; then
                ui_print "- Config will be preserved"
            else
                abort "- Could not back up config; installation stopped to preserve it"
            fi
            ;;
        2|*)
            ui_print "- Config will not be preserved"
            ;;
    esac
else
    ui_print "- First installation"
fi

# Restore config if user chose to keep
if [ "$KEEP_CONFIG" -eq 1 ] && [ -f "$BACKUP_CONFIG" ]; then
    if ! chmod 0644 "$BACKUP_CONFIG" || ! mv -f "$BACKUP_CONFIG" "$MODPATH/config.json"; then
        abort "- Could not restore config; installation stopped"
    fi
    ui_print "- Previous config restored"
fi

# Set permissions
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/action.sh" 0 0 0755
set_perm "$MODPATH/customize.sh" 0 0 0755
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/config.json" 0 0 0644

ui_print "- Installation complete"
ui_print "- Run action.sh to configure target apps"
ui_print "- Or manually edit:"
ui_print "  /data/adb/modules/Yukari/config.json"
