#!/bin/bash
# Installs NAM Bridge into the system audio server. Needs admin rights (sudo).
# Restarting coreaudiod briefly interrupts all audio on this Mac.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/build/NAMBridge.driver"
DEST="/Library/Audio/Plug-Ins/HAL/NAMBridge.driver"

if [ ! -d "$SRC" ]; then
  echo "Build first: cmake -S \"$ROOT\" -B \"$ROOT/build\" && cmake --build \"$ROOT/build\" -j"
  exit 1
fi

echo "Installing $SRC -> $DEST"
sudo rm -rf "$DEST"
sudo cp -R "$SRC" "$DEST"
sudo chown -R root:wheel "$DEST"
echo "Restarting coreaudiod (audio will drop for a moment)..."
sudo killall coreaudiod || true
sleep 3
"$ROOT/build/nam-bridge" driver status || {
  echo "The device did not appear yet. Wait a few seconds and run: $ROOT/build/nam-bridge driver status"
  echo "Driver logs: log show --last 2m --predicate 'process CONTAINS \"coreaudiod\" OR process CONTAINS \"Core Audio Driver\"' | grep -i nam"
}
