// swift-tools-version:6.3
import PackageDescription

// picoruby-ble Apple/Darwin port — its OWN CoreBluetooth backend.
// NOT related to the CRuby rb-corebluetooth-mac gem; written for this port's
// contract (drive CoreBluetooth, feed the C side that synthesizes BTstack events).
//
// CBLEBridge declares the two generic value-table functions (BLE_read_data /
// BLE_write_data) the peripheral backend calls from CBPeripheralManager
// callbacks. They are defined in the gem's shared C and bound at load time, so
// the dylib is linked with `-undefined dynamic_lookup` to leave them unresolved.
let package = Package(
  name: "PicoBLEDarwin",
  platforms: [.macOS(.v11), .iOS(.v13)],
  products: [
    .library(name: "PicoBLEDarwin", type: .dynamic, targets: ["PicoBLEDarwin"]),
  ],
  targets: [
    .target(name: "CBLEBridge", path: "Sources/CBLEBridge"),
    .target(
      name: "PicoBLEDarwin",
      dependencies: ["CBLEBridge"],
      path: "Sources/PicoBLEDarwin",
      linkerSettings: [
        .unsafeFlags(["-Xlinker", "-undefined", "-Xlinker", "dynamic_lookup"]),
      ]
    ),
  ]
)
