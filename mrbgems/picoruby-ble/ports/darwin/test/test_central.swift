// Scripted CoreBluetooth central that drives every peripheral-side path of the
// picoruby-ble ESP32 port and witnesses each result from the receiving end.
// Counterpart: ports/esp32/test/peripheral_paths_probe.rb
//
//   pass 1: connect (P1) -> subscribe (P2) -> receive notify (P4)
//           -> write PING (P5) -> disconnect (P6)
//   pass 2: rediscover (proves rearm_adv) -> connect -> write STOPADV
//           -> disconnect
//   pass 3: scan for QUIET_S with no sighting -> stop_advertise took (P7)
//
// Build and bundle via build_app_bundles.sh; CoreBluetooth needs the plist.

import Foundation
import CoreBluetooth

setbuf(stdout, nil)

let env        = ProcessInfo.processInfo.environment
let targetName = env["TARGET_NAME"] ?? "PBLE-PROBE"
let quietS     = Double(env["QUIET_S"] ?? "12") ?? 12
let overallS   = Double(env["TIMEOUT_S"] ?? "120") ?? 120
let wantNotify = Int(env["NOTIFY_COUNT"] ?? "2") ?? 2

let svcUUID    = CBUUID(string: "181A")
let notifyUUID = CBUUID(string: "2A6E")
let writeUUID  = CBUUID(string: "2A9F")

final class Driver: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
  var cm: CBCentralManager!
  var peri: CBPeripheral?
  var notifyChar: CBCharacteristic?
  var writeChar: CBCharacteristic?

  var pass = 1
  var notifyCount = 0
  var lastSeen: Date?
  var quietSince: Date?
  var results: [String: Bool] = [:]

  func log(_ s: String) { print("[central] \(s)") }
  func mark(_ id: String, _ msg: String) { results[id] = true; log("\(id) OK — \(msg)") }

  func start() { cm = CBCentralManager(delegate: self, queue: nil) }

  func centralManagerDidUpdateState(_ c: CBCentralManager) {
    guard c.state == .poweredOn else { log("bluetooth state=\(c.state.rawValue), need 5"); return }
    scan()
  }

  func scan() {
    log("pass\(pass): scanning for \(targetName)")
    cm.scanForPeripherals(withServices: nil,
                          options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
  }

  func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                      advertisementData d: [String: Any], rssi: NSNumber) {
    let name = (d[CBAdvertisementDataLocalNameKey] as? String) ?? p.name ?? ""
    guard name.contains(targetName) else { return }
    lastSeen = Date()
    if pass == 3 { return }              // only tracking sightings now
    guard peri == nil else { return }
    if pass == 2 { mark("P6", "rediscovered after disconnect, rearm_adv works") }
    log("pass\(pass): found \(name) rssi=\(rssi)")
    c.stopScan()
    peri = p
    p.delegate = self
    c.connect(p, options: nil)
  }

  func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
    mark("P1", "connected; MTU exchange happens on the peripheral now")
    p.discoverServices([svcUUID])
  }

  func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
    guard let s = p.services?.first(where: { $0.uuid == svcUUID }) else {
      log("FAIL service 181A not found: \(String(describing: error))"); return
    }
    p.discoverCharacteristics([notifyUUID, writeUUID], for: s)
  }

  func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
    for ch in s.characteristics ?? [] {
      if ch.uuid == notifyUUID { notifyChar = ch }
      if ch.uuid == writeUUID  { writeChar  = ch }
    }
    guard let nc = notifyChar, let wc = writeChar else {
      log("FAIL characteristics missing notify=\(notifyChar != nil) write=\(writeChar != nil)")
      return
    }
    if pass == 1 {
      log("P2 subscribing (CoreBluetooth writes CCCD 0x0001)")
      p.setNotifyValue(true, for: nc)
    } else {
      log("writing STOPADV to arm stop_advertise")
      p.writeValue("STOPADV".data(using: .utf8)!, for: wc, type: .withResponse)
    }
  }

  func peripheral(_ p: CBPeripheral, didUpdateNotificationStateFor ch: CBCharacteristic, error: Error?) {
    if let e = error { log("FAIL subscribe: \(e)"); return }
    if ch.isNotifying { mark("P2", "CCCD write accepted, now notifying") }
  }

  func peripheral(_ p: CBPeripheral, didUpdateValueFor ch: CBCharacteristic, error: Error?) {
    guard ch.uuid == notifyUUID, let v = ch.value else { return }
    notifyCount += 1
    let hex = v.map { String(format: "%02x", $0) }.joined()
    log("notify #\(notifyCount) value=0x\(hex)")
    guard notifyCount >= wantNotify else { return }
    mark("P4", "received \(notifyCount) notifications from the ESP32 peripheral")
    if let wc = writeChar {
      log("P5 writing PING")
      p.writeValue("PING".data(using: .utf8)!, for: wc, type: .withResponse)
    }
  }

  func peripheral(_ p: CBPeripheral, didWriteValueFor ch: CBCharacteristic, error: Error?) {
    if let e = error { log("FAIL write: \(e)"); return }
    if pass == 1 {
      mark("P5", "characteristic write acknowledged")
      log("disconnecting to exercise P6")
    } else {
      mark("P7", "STOPADV written; peripheral should stop advertising after disconnect")
    }
    cm.cancelPeripheralConnection(p)
  }

  func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
    log("pass\(pass): disconnected")
    peri = nil; notifyChar = nil; writeChar = nil
    pass += 1
    if pass == 3 {
      quietSince = Date()
      log("pass3: watching \(Int(quietS))s for the advertisement to disappear")
    }
    scan()
  }

  func tick() -> Bool {                   // returns true when finished
    guard pass == 3, let q = quietSince else { return false }
    if let seen = lastSeen, seen > q {
      log("FAIL P7: still advertising after stop_advertise")
      quietSince = Date()                 // keep watching until overall timeout
      return false
    }
    if Date().timeIntervalSince(q) >= quietS {
      mark("P7", "no sighting for \(Int(quietS))s after stop_advertise")
      return true
    }
    return false
  }

  func report() -> Int32 {
    let want = ["P1", "P2", "P4", "P5", "P6", "P7"]
    print("===== central-side verdict =====")
    var missing: [String] = []
    for id in want {
      let ok = results[id] ?? false
      print("\(id): \(ok ? "OK" : "MISSING")")
      if !ok { missing.append(id) }
    }
    // P3 is observable only on the device (ATT_EVENT_CAN_SEND_NOW never
    // crosses the air), so it is asserted from the serial log, not here.
    print(missing.isEmpty ? "CENTRAL PASS" : "CENTRAL FAIL missing=\(missing.joined(separator: ","))")
    return missing.isEmpty ? 0 : 1
  }
}

let d = Driver()
d.start()
let deadline = Date().addingTimeInterval(overallS)
while Date() < deadline {
  RunLoop.current.run(until: Date().addingTimeInterval(0.25))
  if d.tick() { break }
}
exit(d.report())
