#!/bin/sh
# install.sh — installs the Mac heavy worker (heavy_mac_worker.py + job.sb) into ~/heavy-worker and starts it as
# a launchd agent. The worker runs from that copy, so later pulls of this checkout (which Fedora can push to)
# never change what runs; rerun this script to take a reviewed update. `install.sh stop` stops and unloads it.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root="$HOME/heavy-worker"
label=com.psxport.heavy-worker
plist="$HOME/Library/LaunchAgents/$label.plist"

launchctl bootout "gui/$(id -u)/$label" 2>/dev/null || true
if [ "${1:-}" = stop ]; then
  rm -f "$plist"
  echo "heavy-mac: stopped"
  exit 0
fi

mkdir -p "$root/bin" "$root/logs"
install -m 0644 "$here/heavy_mac_worker.py" "$here/job.sb" "$root/"
install -m 0755 "$(command -v uv)" "$root/bin/uv"
[ -x "$(command -v uvx)" ] && install -m 0755 "$(command -v uvx)" "$root/bin/uvx"

cat > "$plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$label</string>
  <key>ProgramArguments</key>
  <array><string>/usr/bin/python3</string><string>-I</string><string>$root/heavy_mac_worker.py</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>30</integer>
  <key>ProcessType</key><string>Background</string>
  <key>StandardOutPath</key><string>$root/logs/worker.log</string>
  <key>StandardErrorPath</key><string>$root/logs/worker.log</string>
</dict>
</plist>
EOF
launchctl bootstrap "gui/$(id -u)" "$plist"
echo "heavy-mac: serving from $root (log: $root/logs/worker.log)"
