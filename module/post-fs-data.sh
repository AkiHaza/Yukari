#!/system/bin/sh
MODDIR=${0%/*}
CONFIG="$MODDIR/config.json"

if [ ! -f "$CONFIG" ]; then
  cat > "$CONFIG" <<'EOF'
{
  "enabled": true,
  "hide_features": false,
  "hide_resources": false,
  "targets": []
}
EOF
  chmod 0644 "$CONFIG"
fi
