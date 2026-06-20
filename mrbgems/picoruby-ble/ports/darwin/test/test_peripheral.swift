// Test fixture: a CoreBluetooth GATT peripheral for verifying the
// picoruby-ble Darwin central. Exposes one service (Device Information 0x180A)
// with one readable characteristic (Manufacturer Name 0x2A29) and one
// Characteristic-User-Description descriptor (0x2901). Advertises a local name
// so AdvertisingReport#name_include? can select it. Run it on a SECOND device:
// a Mac acting as central cannot receive advertisements from a peripheral on
// the same Mac.
//
//   swift mrbgems/picoruby-ble/ports/darwin/test/test_peripheral.swift
//
// Leave it running (it serves a run loop) while the central scans/connects/reads.

import Foundation
import CoreBluetooth

setbuf(stdout, nil)   // unbuffered so logs are visible when piped/redirected

let serviceUUID = CBUUID(string: "180A")          // Device Information
let charUUID    = CBUUID(string: "2A29")          // Manufacturer Name String (read)
let descUUID    = CBUUID(string: "2901")          // Characteristic User Description
let charValue   = "PBLE-TEST-MFR"
let descValue   = "pble-demo-desc"
let localName   = "PBLE-TEST"

final class Peripheral: NSObject, CBPeripheralManagerDelegate {
  var pm: CBPeripheralManager!

  func start() { pm = CBPeripheralManager(delegate: self, queue: nil) }

  func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
    guard peripheral.state == .poweredOn else {
      print("[peripheral] state=\(peripheral.state.rawValue) (need poweredOn=5)")
      return
    }
    // Dynamic (value: nil) characteristic so it can carry a descriptor; reads
    // are answered in didReceiveRead. A cached-value characteristic cannot
    // have descriptors.
    let chr = CBMutableCharacteristic(type: charUUID, properties: [.read], value: nil, permissions: [.readable])
    let desc = CBMutableDescriptor(type: descUUID, value: descValue)
    chr.descriptors = [desc]
    let svc = CBMutableService(type: serviceUUID, primary: true)
    svc.characteristics = [chr]
    peripheral.add(svc)
    peripheral.startAdvertising([
      CBAdvertisementDataLocalNameKey: localName,
      CBAdvertisementDataServiceUUIDsKey: [serviceUUID],
    ])
    print("[peripheral] advertising '\(localName)': service 180A / char 2A29 (read) / desc 2901")
  }

  func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveRead request: CBATTRequest) {
    if request.characteristic.uuid == charUUID {
      request.value = charValue.data(using: .utf8)
      peripheral.respond(to: request, withResult: .success)
      print("[peripheral] served char read -> '\(charValue)'")
    } else {
      peripheral.respond(to: request, withResult: .attributeNotFound)
    }
  }

  func peripheralManager(_ peripheral: CBPeripheralManager, central: CBCentral, didSubscribeTo characteristic: CBCharacteristic) {}
}

let p = Peripheral()
p.start()
print("[peripheral] starting run loop (Ctrl-C to stop)")
RunLoop.main.run()
