// swift-tools-version:6.3
import PackageDescription

// picoruby-ble Apple/Darwin port — its OWN CoreBluetooth backend.
// NOT related to the CRuby rb-corebluetooth-mac gem; written for this port's
// contract (drive CoreBluetooth, feed the C side that synthesizes BTstack events).
let package = Package(
  name: "PicoBLEDarwin",
  platforms: [.macOS(.v11), .iOS(.v13)],
  products: [
    .library(name: "PicoBLEDarwin", type: .dynamic, targets: ["PicoBLEDarwin"]),
  ],
  targets: [
    .target(name: "PicoBLEDarwin", path: "Sources/PicoBLEDarwin"),
  ]
)
