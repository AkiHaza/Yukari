#!/system/bin/sh
MODDIR=${0%/*}
CONFIG="$MODDIR/config.json"
LOGDIR="$MODDIR/logs"

mkdir -p "$LOGDIR"
chmod 0755 "$LOGDIR"

if [ ! -f "$CONFIG" ]; then
  cat > "$CONFIG" <<'EOF'
{
  "enabled": true,
  "enhancedMode": false,
  "targets": []
}
EOF
  chmod 0644 "$CONFIG"
fi
