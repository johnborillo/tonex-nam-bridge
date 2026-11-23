#!/bin/bash
# Builds the NAM Bridge menu-bar app into build/NAM Bridge.app
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/build/NAM Bridge.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
swiftc -O -parse-as-library -target "$(uname -m)-apple-macos14.0" \
  "$ROOT"/app/Sources/*.swift -o "$APP/Contents/MacOS/NAMBridge" \
  -framework CoreAudio -framework SwiftUI -framework AppKit
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>NAMBridge</string>
  <key>CFBundleIdentifier</key><string>local.tonex-nam-bridge.app</string>
  <key>CFBundleName</key><string>NAM Bridge</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>LSUIElement</key><true/>
</dict></plist>
PLIST
codesign --force --sign - "$APP"
echo "Built $APP"
