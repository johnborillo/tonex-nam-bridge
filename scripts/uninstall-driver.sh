#!/bin/bash
# Removes NAM Bridge from the system audio server.
set -euo pipefail
sudo rm -rf "/Library/Audio/Plug-Ins/HAL/NAMBridge.driver"
sudo killall coreaudiod || true
echo "NAM Bridge removed."
