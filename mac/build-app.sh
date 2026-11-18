#!/bin/bash
# Builds a double-clickable StorageScope.app (universal: Apple Silicon + Intel) into ./build.
# Needs Xcode or the Xcode Command Line Tools (xcode-select --install).
set -euo pipefail
cd "$(dirname "$0")"

swift build -c release --arch arm64 --arch x86_64
BIN="$(swift build -c release --arch arm64 --arch x86_64 --show-bin-path)/StorageScope"

APP="build/StorageScope.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/StorageScope"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>StorageScope</string>
  <key>CFBundleDisplayName</key><string>StorageScope</string>
  <key>CFBundleIdentifier</key><string>com.storagescope.mac</string>
  <key>CFBundleExecutable</key><string>StorageScope</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString ${VERSION:-0.1.0}" "$APP/Contents/Info.plist"

# Ad-hoc signature so the app runs on this Mac. To share it, sign with a Developer ID and notarize (see README).
codesign --force --deep --sign - "$APP"
# Disk image: the app plus an Applications shortcut to drag it onto.
rm -rf build/dmg build/StorageScope-mac.dmg
mkdir build/dmg
cp -R "$APP" build/dmg/
ln -s /Applications build/dmg/Applications
hdiutil create -volname StorageScope -srcfolder build/dmg -ov -format UDZO build/StorageScope-mac.dmg
rm -rf build/dmg
(cd build && shasum -a 256 StorageScope-mac.dmg > StorageScope-mac.dmg.sha256)
echo "Built $APP and build/StorageScope-mac.dmg"
echo "Open it with:  open build/StorageScope-mac.dmg"
