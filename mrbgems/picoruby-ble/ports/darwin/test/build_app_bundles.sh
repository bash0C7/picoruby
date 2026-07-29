#!/bin/bash
# Rebuilds the .app bundles the CoreBluetooth verification depends on.
#
# TCC ties the Bluetooth grant to the binary's ad-hoc signature, so every
# rebuild revokes it. After running this, a human must re-grant Bluetooth to
# each app (System Settings > Privacy & Security > Bluetooth, or accept the
# prompt on first launch). That step cannot be automated.
set -euo pipefail

DARWIN_ROOT=${DARWIN_ROOT:-/Users/bash/dev/src/github.com/bash0C7/picoruby-port-darwin}
TEST_DIR="$DARWIN_ROOT/mrbgems/picoruby-ble/ports/darwin/test"
DEST=${DEST:-$HOME/Applications}
ONLY=${1:-all}

bundle_it() {   # $1=AppName $2=srcBinary $3=bundleId $4=exeName
  local app="$DEST/$1.app"
  rm -rf "$app"
  mkdir -p "$app/Contents/MacOS"
  cp "$2" "$app/Contents/MacOS/$4"
  cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>$4</string>
  <key>CFBundleIdentifier</key><string>$3</string>
  <key>CFBundleName</key><string>$1</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>NSBluetoothAlwaysUsageDescription</key><string>Verifies picoruby-ble's darwin port against a real BLE peer.</string>
  <key>NSBluetoothPeripheralUsageDescription</key><string>Verifies picoruby-ble's darwin port against a real BLE peer.</string>
</dict>
</plist>
PLIST
  codesign --force --deep -s - "$app"
  echo "[bundles] built $app from $2"
}

if [ "$ONLY" = all ] || [ "$ONLY" = vm ]; then
  cd "$DARWIN_ROOT"
  export LDFLAGS="-L$(brew --prefix openssl@3)/lib"
  export CFLAGS="-I$(brew --prefix openssl@3)/include"
  MRUBY_CONFIG="$DARWIN_ROOT/build_config/darwin-ble-test.rb" rake
  bundle_it PicoRubyBLE "$DARWIN_ROOT/build/darwin-ble-test/bin/picoruby" \
            com.bash0c7.picorubyble picoruby-bin
fi

if [ "$ONLY" = all ] || [ "$ONLY" = peripheral ]; then
  swiftc "$TEST_DIR/test_peripheral.swift" -o /tmp/test_peripheral_bin
  bundle_it PicoPeripheralTest /tmp/test_peripheral_bin \
            com.bash0c7.picoperipheraltest test_peripheral_bin
fi

if [ "$ONLY" = all ] || [ "$ONLY" = central ]; then
  swiftc "$TEST_DIR/test_central.swift" -o /tmp/test_central_bin
  bundle_it PicoCentralTest /tmp/test_central_bin \
            com.bash0c7.picocentraltest test_central_bin
fi

echo "[bundles] done. Bluetooth permission must be re-granted for each rebuilt app."
