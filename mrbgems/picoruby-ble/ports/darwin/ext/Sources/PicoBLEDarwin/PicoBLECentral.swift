import Foundation
import CoreBluetooth

// CoreBluetooth central backend for picoruby-ble (central/observer v1). All
// CBCentralManager/CBPeripheral delegate callbacks and all command entry points
// run on one private serial queue ("pble.cb"), so the GATT tree below needs no
// internal locking. Built packets are pushed to pbleSharedFifo; the mruby VM
// thread drains them (one per tick) and is the ONLY caller of BLE_push_event.
// No mrb_* / BLE_push_event ever runs on this queue.
//
// Handle model: CoreBluetooth has no ATT handles, so we mint synthetic uint8
// handles by pre-order DFS over the FULLY discovered tree (eager discovery), then
// emit results lazily as the decoder requests each phase. Real tight ranges
// (service.end < next service.start) make the decoder's first-match characteristic
// filing (ble_central.rb:169) correct for multi-service peripherals — the design's
// "service end = 0xFF" shortcut is NOT used because it misfiles characteristics
// across services.

final class PBLEDescNode {
  // cb is nil for a synthetic CCCD: CoreBluetooth hides the 0x2902 descriptor,
  // so we mint one for notify/indicate characteristics to expose the handle the
  // canonical subscribe path ("write [0x01,0x00] to the CCCD handle") needs.
  let cb: CBDescriptor?
  let cccdChar: CBCharacteristic?   // set only for a synthetic CCCD: the char to setNotifyValue on
  var handle: UInt8 = 0
  init(_ d: CBDescriptor) { cb = d; cccdChar = nil }
  init(syntheticCCCDFor char: CBCharacteristic) { cb = nil; cccdChar = char }
  /// CBUUID.data-style bytes (big-endian) for the wire encoder.
  var uuidData: [UInt8] { cb.map { [UInt8]($0.uuid.data) } ?? [0x29, 0x02] }
}

final class PBLECharNode {
  let cb: CBCharacteristic
  var start: UInt8 = 0
  var value: UInt8 = 0
  var end: UInt8 = 0
  var descriptors: [PBLEDescNode] = []
  init(_ c: CBCharacteristic) { cb = c }
}

final class PBLESvcNode {
  let cb: CBService
  var start: UInt8 = 0
  var end: UInt8 = 0
  var characteristics: [PBLECharNode] = []
  init(_ s: CBService) { cb = s }
}

final class PBLECentral: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate, @unchecked Sendable {
  static let shared = PBLECentral()

  // Bluetooth SIG-assigned 16-bit UUID for the Client Characteristic
  // Configuration Descriptor (matches mrblib/ble.rb's
  // CLIENT_CHARACTERISTIC_CONFIGURATION = 0x2902).
  private let cccdUUID = CBUUID(string: "2902")

  private let cbQueue = DispatchQueue(label: "pble.cb")
  private var manager: CBCentralManager?
  private var powerRequested = false

  // Discovered-during-scan peripherals, keyed by the 6-byte address the decoder
  // hands back to gap_connect (i.e. the synthetic wire address, reversed).
  private var peripheralsByConnectKey: [[UInt8]: CBPeripheral] = [:]

  // The single connected peripheral and its synthetic GATT tree (v1: one connection).
  private var peripheral: CBPeripheral?
  private var connHandle: UInt16 = 0x0040
  private var services: [PBLESvcNode] = []
  private var svcByStart: [UInt8: PBLESvcNode] = [:]
  private var readCharByValueHandle: [UInt8: CBCharacteristic] = [:]
  private var readDescByHandle: [UInt8: CBDescriptor] = [:]
  private var valueHandleForChar: [ObjectIdentifier: UInt8] = [:]
  private var handleForDesc: [ObjectIdentifier: UInt8] = [:]
  private var cccdCharByHandle: [UInt8: CBCharacteristic] = [:]  // synthetic CCCD handle -> its characteristic
  private var cccdStateByHandle: [UInt8: [UInt8]] = [:]          // synthetic CCCD handle -> last-written [enable,0x00]
  private var pendingReadChars: Set<ObjectIdentifier> = []       // chars with an in-flight explicit read (vs notification)
  private var maxCharValueHandle: UInt8 = 0
  private var maxDescriptorHandle: UInt8 = 0

  // Eager-discovery progress (cb queue only).
  private var pendingCharSvcs = 0
  private var pendingDescChars = 0

  // True only while scanning; gates advertising-report enqueue so a backlog of
  // reports cannot delay the connection-complete packet after we connect.
  private var scanning = false

  // ---- command entry points (called from the mruby VM thread) ----

  func setup(role: Int32) {
    cbQueue.async { [self] in
      if manager == nil { manager = CBCentralManager(delegate: self, queue: cbQueue) }
    }
  }

  func powerOn() {
    cbQueue.async { [self] in
      powerRequested = true
      // Every start() calls hci_power_control(ON); emit WORKING each time. The
      // decoder gates BTSTACK_EVENT_STATE on @state in {TC_OFF,TC_IDLE}, so a
      // WORKING during a connection (TC_W4_*) is ignored, and a fresh scan picks
      // it up. (If not poweredOn yet, centralManagerDidUpdateState emits it.)
      if manager?.state == .poweredOn { push(pbleStateWorking()) }
    }
  }

  func powerOff() {
    // hci_power_control(OFF) runs in every start()'s ensure; only stop scanning.
    // Do NOT drop an established connection, or connect()'s nested start(10) and
    // any later discovery loop would lose the peripheral.
    cbQueue.async { [self] in
      powerRequested = false   // so a late didUpdateState after stop cannot emit WORKING
      manager?.stopScan()
    }
  }

  func startScan() {
    cbQueue.async { [self] in
      peripheralsByConnectKey.removeAll()
      pbleSharedFifo.flush()
      scanning = true
      manager?.scanForPeripherals(withServices: nil, options: nil)
    }
  }

  func stopScan() {
    cbQueue.async { [self] in
      scanning = false
      manager?.stopScan()
      pbleSharedFifo.flush()   // drop the adv-report backlog before connecting
    }
  }

  func connect(addr: UnsafePointer<UInt8>, addrType: UInt8) -> Int32 {
    var key = [UInt8](repeating: 0, count: 6)
    for i in 0..<6 { key[i] = addr[i] }
    // sync (not async): the FSM needs the Int32 return synchronously (0 = connect
    // started, 1 = no such peripheral). Every other command entry is async, but this
    // one alone returns a value; async would always return 0 before the lookup runs.
    return cbQueue.sync { [self] in
      guard let p = peripheralsByConnectKey[key] else { return Int32(1) }
      manager?.stopScan()
      peripheral = p
      p.delegate = self
      manager?.connect(p, options: nil)
      return Int32(0)
    }
  }

  func discoverServices(_ conn: UInt16) {
    cbQueue.async { [self] in
      resetTree()
      peripheral?.discoverServices(nil)
    }
  }

  func discoverCharacteristics(_ conn: UInt16, _ start: UInt8, _ end: UInt8) {
    cbQueue.async { [self] in
      if let svc = svcByStart[start] {
        for ch in svc.characteristics {
          push(pbleCharacteristicResult(start: ch.start, value: ch.value, end: ch.end,
                                        properties: propertiesByte(ch.cb.properties),
                                        uuidWire: pbleUuidWire(fromCBUUIDData: Array(ch.cb.uuid.data))))
        }
      }
      push(pbleQueryComplete())
    }
  }

  func readValue(_ conn: UInt16, _ handle: UInt8) {
    cbQueue.async { [self] in
      if let ch = readCharByValueHandle[handle] {
        if ch.properties.contains(.read) {
          pendingReadChars.insert(ObjectIdentifier(ch))  // mark so didUpdateValueFor emits 0xA5, not 0xA7
          peripheral?.readValue(for: ch)            // CB responds via didUpdateValueFor
        } else {
          // Non-readable characteristic: the decoder still reads every value handle.
          // Synthesize an empty value so the value worklist advances without waiting
          // on a CB response that will never arrive (would stall the batched 0xA0).
          push(pbleValueResult(valueHandle: handle, value: []))
          if handle == maxCharValueHandle { push(pbleQueryComplete()) }
        }
      } else if let d = readDescByHandle[handle] {
        peripheral?.readValue(for: d)
      } else if cccdCharByHandle[handle] != nil {
        // Synthetic CCCD has no CoreBluetooth descriptor to read; answer from the
        // last-written state so the decoder's descriptor-value phase advances.
        push(pbleValueResult(valueHandle: handle, value: cccdStateByHandle[handle] ?? [0x00, 0x00]))
        if handle == maxDescriptorHandle { push(pbleQueryComplete()) }
      } else {
        // Unknown handle: still terminate the phase so the FSM does not stall.
        push(pbleQueryComplete())
      }
    }
  }

  // ---- GATT write / subscribe (called from the mruby VM thread) ----

  /// Write without response to a characteristic value handle (e.g. NUS RX).
  func writeValueWithoutResponse(_ conn: UInt16, _ valueHandle: UInt8, _ bytes: [UInt8]) {
    cbQueue.async { [self] in
      guard let ch = readCharByValueHandle[valueHandle], let p = peripheral else { return }
      p.writeValue(Data(bytes), for: ch, type: .withoutResponse)
    }
  }

  /// Write to a descriptor handle. For a synthetic CCCD this maps to
  /// setNotifyValue (CoreBluetooth manages the real CCCD); a non-zero first byte
  /// enables notify/indicate. Any other (real) descriptor is written directly.
  func writeDescriptor(_ conn: UInt16, _ descHandle: UInt8, _ bytes: [UInt8]) {
    cbQueue.async { [self] in
      let enable = (bytes.first ?? 0) != 0
      if let ch = cccdCharByHandle[descHandle] {
        cccdStateByHandle[descHandle] = enable ? [0x01, 0x00] : [0x00, 0x00]
        peripheral?.setNotifyValue(enable, for: ch)
      } else if let d = readDescByHandle[descHandle], let p = peripheral {
        p.writeValue(Data(bytes), for: d)
      }
    }
  }

  func discoverDescriptors(_ conn: UInt16, _ value: UInt8, _ end: UInt8) {
    cbQueue.async { [self] in
      if let node = charNode(forValueHandle: value) {
        for d in node.descriptors {
          push(pbleDescriptorResult(handle: d.handle,
                                    uuidWire: pbleUuidWire(fromCBUUIDData: d.uuidData)))
        }
      }
      push(pbleQueryComplete())
    }
  }

  // ---- CBCentralManagerDelegate ----

  func centralManagerDidUpdateState(_ central: CBCentralManager) {
    if central.state == .poweredOn, powerRequested {
      push(pbleStateWorking())
    }
  }

  func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                      advertisementData: [String: Any], rssi RSSI: NSNumber) {
    guard scanning else { return }   // ignore stragglers once we stop to connect
    let (key, wireAddr) = synthAddress(for: peripheral)
    peripheralsByConnectKey[key] = peripheral   // retain so connect works later
    push(pbleAdvReport(addr: wireAddr, addrType: 0x01, rssi: RSSI.intValue,
                       advData: advTLV(advertisementData)))
  }

  func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
    self.peripheral = peripheral
    peripheral.delegate = self
    push(pbleConnComplete(connHandle))
  }

  func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
    connectionLost()
  }

  func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
    connectionLost()
  }

  /// Connection gone: emit the disconnect packet AND a terminating query-complete
  /// so any GATT phase the decoder is waiting on (a read with no didUpdateValueFor,
  /// or discovery that never finalized) is released instead of stalling forever.
  /// The stray 0xA0 is a no-op in :TC_W4_CONNECT / :TC_IDLE. Then drop the tree.
  private func connectionLost() {
    push(pbleDisconnect())
    push(pbleQueryComplete())
    resetTree()
    peripheral = nil
  }

  // ---- CBPeripheralDelegate (eager discovery) ----

  func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
    services = (peripheral.services ?? []).map { PBLESvcNode($0) }
    pendingCharSvcs = services.count
    pendingDescChars = 0
    if services.isEmpty { finalizeDiscovery(); return }
    for svc in services { peripheral.discoverCharacteristics(nil, for: svc.cb) }
  }

  func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
    // Decrement unconditionally: a missed service (identity mismatch / error) must
    // not wedge the counter, or finalizeDiscovery never runs and the decoder parks
    // in :TC_W4_SERVICE_RESULT forever.
    pendingCharSvcs -= 1
    if let node = services.first(where: { $0.cb === service }) {
      node.characteristics = (service.characteristics ?? []).map { PBLECharNode($0) }
      pendingDescChars += node.characteristics.count
      for ch in node.characteristics { peripheral.discoverDescriptors(for: ch.cb) }
    }
    finalizeIfReady()
  }

  func peripheral(_ peripheral: CBPeripheral, didDiscoverDescriptorsFor characteristic: CBCharacteristic, error: Error?) {
    if let chNode = charNode(forCB: characteristic) {
      chNode.descriptors = (characteristic.descriptors ?? []).map { PBLEDescNode($0) }
    }
    pendingDescChars -= 1
    finalizeIfReady()
  }

  func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
    let oid = ObjectIdentifier(characteristic)
    let vh = valueHandleForChar[oid] ?? 0
    if pendingReadChars.remove(oid) != nil {
      // Response to an explicit read issued during eager discovery.
      push(pbleValueResult(valueHandle: vh, value: bytesOf(characteristic.value)))
      if vh == maxCharValueHandle { push(pbleQueryComplete()) }
    } else {
      // Unsolicited update from a subscribed characteristic = notification/indication.
      push(pbleNotification(valueHandle: vh, value: bytesOf(characteristic.value)))
    }
  }

  func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor descriptor: CBDescriptor, error: Error?) {
    let h = handleForDesc[ObjectIdentifier(descriptor)] ?? 0
    push(pbleValueResult(valueHandle: h, value: bytesOfAny(descriptor.value)))
    if h == maxDescriptorHandle { push(pbleQueryComplete()) }
  }

  // ---- internals (cb queue only) ----

  private func push(_ packet: [UInt8]) { pbleSharedFifo.push(packet) }

  private func resetTree() {
    services.removeAll(); svcByStart.removeAll()
    readCharByValueHandle.removeAll(); readDescByHandle.removeAll()
    valueHandleForChar.removeAll(); handleForDesc.removeAll()
    cccdCharByHandle.removeAll(); cccdStateByHandle.removeAll()
    pendingReadChars.removeAll()
    pendingCharSvcs = 0; pendingDescChars = 0
  }

  private func finalizeIfReady() {
    if pendingCharSvcs == 0 && pendingDescChars == 0 { finalizeDiscovery() }
  }

  /// CoreBluetooth hides the CCCD (0x2902) from discovered descriptors, so a
  /// notify/indicate-capable characteristic would otherwise expose no handle for
  /// the canonical subscribe path (write [0x01,0x00] to the CCCD handle). Mint a
  /// synthetic CCCD node — matching what BTstack-based ports surface — before
  /// handle allocation; writes to its handle map to setNotifyValue and reads are
  /// answered from the stored state.
  private func injectSyntheticCCCDs() {
    for svc in services {
      for ch in svc.characteristics {
        let p = ch.cb.properties
        guard p.contains(.notify) || p.contains(.indicate) else { continue }
        if !ch.descriptors.contains(where: { $0.cb?.uuid == cccdUUID }) {
          ch.descriptors.append(PBLEDescNode(syntheticCCCDFor: ch.cb))
        }
      }
    }
  }

  /// Allocate synthetic uint8 handles by pre-order DFS and emit all service results
  /// followed by one query-complete. Drops entities past 255 (logged).
  private func finalizeDiscovery() {
    injectSyntheticCCCDs()
    var cursor = 1
    func alloc() -> UInt8? { if cursor > 255 { return nil }; let h = UInt8(cursor); cursor += 1; return h }
    var capped = false

    outer: for svc in services {
      guard let s = alloc() else { capped = true; break }
      svc.start = s
      for ch in svc.characteristics {
        guard let cs = alloc() else { capped = true; break outer }
        ch.start = cs
        guard let cv = alloc() else { capped = true; break outer }
        ch.value = cv
        for d in ch.descriptors {
          guard let dh = alloc() else { capped = true; break outer }
          d.handle = dh
        }
        ch.end = UInt8(cursor - 1)
      }
      svc.end = UInt8(cursor - 1)
    }
    if capped { print("[ports/darwin] GATT tree exceeds 255 handles; truncated") }

    svcByStart.removeAll()
    readCharByValueHandle.removeAll()
    readDescByHandle.removeAll()
    valueHandleForChar.removeAll()
    handleForDesc.removeAll()
    cccdCharByHandle.removeAll()
    cccdStateByHandle.removeAll()
    // Emit/register only fully-allocated services. A service truncated by the 255
    // cap keeps svc.end == 0 (set only after its whole subtree is numbered); emitting
    // it with end=0 would make every characteristic fail the decoder's
    // char.end <= service.end filing test (ble_central.rb:169). Drop it instead.
    for svc in services where svc.start != 0 && svc.end >= svc.start {
      svcByStart[svc.start] = svc
      for ch in svc.characteristics where ch.value != 0 {
        readCharByValueHandle[ch.value] = ch.cb
        valueHandleForChar[ObjectIdentifier(ch.cb)] = ch.value
        for d in ch.descriptors where d.handle != 0 {
          if let cb = d.cb, cb.uuid == cccdUUID {
            // Some peripherals/OS combinations DO expose a real CBDescriptor
            // for the CCCD (injectSyntheticCCCDs then skips minting a
            // synthetic one), but CoreBluetooth always forbids writing a CCCD
            // directly regardless of whether it happens to be enumerable
            // (NSInternalInconsistencyException: "...must be configured using
            // setNotifyValue:forCharacteristic:"). Route it the same as a
            // synthetic CCCD so writeDescriptor always uses setNotifyValue.
            cccdCharByHandle[d.handle] = ch.cb
            cccdStateByHandle[d.handle] = [0x00, 0x00]
          } else if let cb = d.cb {
            readDescByHandle[d.handle] = cb
            handleForDesc[ObjectIdentifier(cb)] = d.handle
          } else if let cccdChar = d.cccdChar {
            cccdCharByHandle[d.handle] = cccdChar
            cccdStateByHandle[d.handle] = [0x00, 0x00]
          }
        }
      }
      push(pbleServiceResult(start: svc.start, end: svc.end,
                             uuidWire: pbleUuidWire(fromCBUUIDData: Array(svc.cb.uuid.data))))
    }
    maxCharValueHandle = readCharByValueHandle.keys.max() ?? 0
    maxDescriptorHandle = max(readDescByHandle.keys.max() ?? 0, cccdCharByHandle.keys.max() ?? 0)
    push(pbleQueryComplete())
  }

  private func charNode(forCB c: CBCharacteristic) -> PBLECharNode? {
    for svc in services { for ch in svc.characteristics where ch.cb === c { return ch } }
    return nil
  }

  private func charNode(forValueHandle vh: UInt8) -> PBLECharNode? {
    for svc in services { for ch in svc.characteristics where ch.value == vh { return ch } }
    return nil
  }

  private func propertiesByte(_ p: CBCharacteristicProperties) -> UInt8 {
    var b: UInt8 = 0
    if p.contains(.broadcast) { b |= 0x01 }
    if p.contains(.read) { b |= 0x02 }
    if p.contains(.writeWithoutResponse) { b |= 0x04 }
    if p.contains(.write) { b |= 0x08 }
    if p.contains(.notify) { b |= 0x10 }
    if p.contains(.indicate) { b |= 0x20 }
    if p.contains(.authenticatedSignedWrites) { b |= 0x40 }
    if p.contains(.extendedProperties) { b |= 0x80 }
    return b
  }

  /// 6-byte synthetic address from peripheral.identifier; every byte forced
  /// non-zero so gap_connect's NUL-terminated 'z' argument is not truncated.
  /// Returns (connectKey, wireAddr): wireAddr goes at adv offsets [4..9]; the
  /// decoder reverses it into @address, which arrives at connect as connectKey.
  private func synthAddress(for p: CBPeripheral) -> (key: [UInt8], wire: [UInt8]) {
    let u = p.identifier.uuid
    let raw = [u.0, u.1, u.2, u.3, u.4, u.5]
    let wire = raw.map { $0 | 0x01 }
    let key = Array(wire.reversed())
    return (key, wire)
  }

  private func advTLV(_ adv: [String: Any]) -> [UInt8] {
    var out: [UInt8] = []
    if let name = adv[CBAdvertisementDataLocalNameKey] as? String {
      let nb = Array(name.utf8)
      if !nb.isEmpty { out.append(UInt8(min(nb.count + 1, 255))); out.append(0x09); out.append(contentsOf: nb.prefix(254)) }
    }
    if let mfg = adv[CBAdvertisementDataManufacturerDataKey] as? Data, !mfg.isEmpty {
      let mb = [UInt8](mfg)
      out.append(UInt8(min(mb.count + 1, 255))); out.append(0xFF); out.append(contentsOf: mb.prefix(254))
    }
    return out
  }

  private func bytesOf(_ data: Data?) -> [UInt8] { data.map { [UInt8]($0) } ?? [] }

  private func bytesOfAny(_ v: Any?) -> [UInt8] {
    if let d = v as? Data { return [UInt8](d) }
    if let n = v as? NSNumber { let x = n.uint16Value; return [UInt8(x & 0xff), UInt8((x >> 8) & 0xff)] }
    if let s = v as? String { return Array(s.utf8) }
    return []
  }
}
