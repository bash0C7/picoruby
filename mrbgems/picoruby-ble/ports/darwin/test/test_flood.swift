// PicoFloodTest — floods the ESP32 peripheral's write characteristic so its
// inbound write queue fills, exercising the bound and the overflow behaviour.
//
//   FLOOD_MODE=wwr  .withoutResponse — NUS audio's opcode; NimBLE discards the
//                   access callback's error, so excess writes vanish silently
//   FLOOD_MODE=wr   .withResponse    — ATT Write Request; a full queue comes
//                   back as an ATT error, counted here
//
// Counterpart: ports/esp32/test/write_flood_probe.rb
import Foundation
import CoreBluetooth

let env        = ProcessInfo.processInfo.environment
let targetName = env["TARGET_NAME"] ?? "PBLE-FLOOD"
let overallS   = Double(env["TIMEOUT_S"] ?? "90") ?? 90
let floodMode  = env["FLOOD_MODE"] ?? "wwr"
let floodCount = Int(env["FLOOD_COUNT"] ?? "500") ?? 500
let floodSize  = Int(env["FLOOD_SIZE"] ?? "128") ?? 128

let svcUUID   = CBUUID(string: "181A")
let writeUUID = CBUUID(string: "2A9F")

final class Flood: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
  var cm: CBCentralManager!
  var peri: CBPeripheral?
  var writeChar: CBCharacteristic?
  var sent = 0
  var acked = 0
  var attErrors = 0
  var finished = false

  func log(_ s: String) { print("[flood] \(s)") }
  func start() { cm = CBCentralManager(delegate: self, queue: nil) }

  func centralManagerDidUpdateState(_ c: CBCentralManager) {
    guard c.state == .poweredOn else { return }
    log("scanning for \(targetName)")
    c.scanForPeripherals(withServices: nil)
  }

  func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                      advertisementData d: [String: Any], rssi: NSNumber) {
    let name = (d[CBAdvertisementDataLocalNameKey] as? String) ?? p.name ?? ""
    guard name.contains(targetName) else { return }
    c.stopScan()
    peri = p
    p.delegate = self
    log("found \(name) rssi=\(rssi), connecting")
    c.connect(p)
  }

  func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
    log("connected; discovering")
    p.discoverServices([svcUUID])
  }

  func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
    guard let s = p.services?.first(where: { $0.uuid == svcUUID }) else {
      log("FAIL service 181A not found"); finish(); return
    }
    p.discoverCharacteristics([writeUUID], for: s)
  }

  func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
    guard let wc = s.characteristics?.first(where: { $0.uuid == writeUUID }) else {
      log("FAIL write characteristic 2A9F not found"); finish(); return
    }
    writeChar = wc
    log("flooding mode=\(floodMode) count=\(floodCount) size=\(floodSize)")
    pump()
  }

  // Frame i carries 0xA5 then i's low byte repeated, so the device can count
  // arrivals without parsing anything.
  func frame(_ i: Int) -> Data {
    var bytes = [UInt8](repeating: UInt8(i & 0xff), count: floodSize)
    bytes[0] = 0xA5
    return Data(bytes)
  }

  func pump() {
    guard let p = peri, let wc = writeChar else { return }
    if floodMode == "wr" {
      guard sent < floodCount else { return }
      p.writeValue(frame(sent), for: wc, type: .withResponse)
      sent += 1
      return
    }
    while sent < floodCount && p.canSendWriteWithoutResponse {
      p.writeValue(frame(sent), for: wc, type: .withoutResponse)
      sent += 1
    }
    if sent >= floodCount {
      log("sent=\(sent); waiting 5s for the device to drain")
      DispatchQueue.main.asyncAfter(deadline: .now() + 5) { self.finish() }
    }
  }

  func peripheralIsReady(toSendWriteWithoutResponse p: CBPeripheral) { pump() }

  func peripheral(_ p: CBPeripheral, didWriteValueFor ch: CBCharacteristic, error: Error?) {
    if let e = error {
      attErrors += 1
      if attErrors == 1 { log("first ATT error: \(e.localizedDescription)") }
    } else {
      acked += 1
    }
    if sent < floodCount { pump() } else { finish() }
  }

  func finish() {
    guard !finished else { return }
    finished = true
    log("===== flood verdict =====")
    log("mode=\(floodMode) sent=\(sent) acked=\(acked) att_errors=\(attErrors)")
    if floodMode == "wr" {
      log(attErrors > 0 ? "FLOOD PASS — the peripheral refused what it could not queue"
                        : "FLOOD NO-BACKPRESSURE — every Write Request was accepted")
    } else {
      log("FLOOD SENT — compare with the device's received count and drop warning")
    }
    exit(0)
  }
}

let f = Flood()
f.start()
DispatchQueue.main.asyncAfter(deadline: .now() + overallS) {
  f.log("TIMEOUT after \(overallS)s"); f.finish()
}
RunLoop.main.run()
