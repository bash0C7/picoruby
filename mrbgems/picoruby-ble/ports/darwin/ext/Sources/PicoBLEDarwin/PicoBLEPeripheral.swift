// CBPeripheralManager / CBMutableService / CBMutableCharacteristic は watchOS SDK が
// API_UNAVAILABLE(watchos, tvos) と宣言している。watchOS はこのポートを central
// ロールでしか使わないので、peripheral バックエンドごとコンパイル対象から外す。
// C 側 ble_peripheral.c は watchOS でもアーカイブに入り pble_peripheral_* を extern
// 参照するので、その export は PicoBLEExports.swift の no-op stub が受け持つ。
#if !os(watchOS)
import Foundation
import CoreBluetooth
import CBLEBridge

// CoreBluetooth peripheral backend for picoruby-ble. Brings the Darwin port to
// role-parity with rp2040: a GATT server driven entirely from Ruby (subclass BLE
// with role :peripheral). The shared C (src/mruby/ble.c) and Ruby (mrblib/ble.rb)
// are unchanged; everything here lives under ports/darwin.
//
// Threading — the invariant is "mruby state is touched ONLY on the VM thread":
//   * CBPeripheralManagerDelegate callbacks run on the private `cbQueue`. They
//     NEVER call BLE_read_data / BLE_write_data (those touch mruby hashes and would
//     race the GC). A read is answered from a Swift-side cache; a write / subscribe
//     is appended to a Swift-side pending queue.
//   * `pump()` runs on the VM thread (driven from pble_drain_one every poll tick).
//     It is the ONLY place BLE_write_data / BLE_read_data are called: it flushes the
//     pending writes into mruby and refreshes the read cache from mruby.
// btstack-format events (state / disconnect / MTU / can-send-now) are pushed to the
// shared FIFO, draining through the same role-agnostic path the central uses.

private struct PBLECharSpec {
  let uuid: CBUUID
  let properties: CBCharacteristicProperties
  let permissions: CBAttributePermissions
  let valueHandle: UInt16
  let initialValue: [UInt8]
  let cccdHandle: UInt16?      // synthetic CCCD handle for a notify/indicate char
}

private struct PBLEServiceSpec {
  let uuid: CBUUID
  var characteristics: [PBLECharSpec]
}

final class PBLEPeripheral: NSObject, CBPeripheralManagerDelegate, @unchecked Sendable {
  static let shared = PBLEPeripheral()

  private let cbQueue = DispatchQueue(label: "pble.cb.peri")
  private var manager: CBPeripheralManager?
  private var powerRequested = false
  private var servicesAdded = false
  private var pendingServiceAdds = 0

  // Parsed GATT model (built once at init, before power-on).
  private var serviceSpecs: [PBLEServiceSpec] = []
  private var charByAttHandle: [UInt16: CBMutableCharacteristic] = [:]
  private var attHandleForChar: [ObjectIdentifier: UInt16] = [:]
  private var cccdHandleForChar: [ObjectIdentifier: UInt16] = [:]

  // VM-thread <-> cbQueue shared state, guarded by `lock`.
  private let lock = NSLock()
  private var valueByAttHandle: [UInt16: [UInt8]] = [:]   // read cache
  private var pendingWrites: [(UInt16, [UInt8])] = []      // writes/CCCD awaiting the VM thread
  private var valueHandles: [UInt16] = []
  private var active = false

  private var mtuAnnounced = false

  // ---- init / parse (VM thread, before power) ----

  /// Parse the BTstack ATT-DB blob (mrblib/ble_gatt_database.rb format) and build
  /// the CoreBluetooth service/characteristic objects. Called from BLE_init.
  /// A broadcaster passes no profile: it advertises with no GATT database at all.
  func setup(profile: UnsafePointer<UInt8>?) {
    if let profile { parseProfile(profile) }
    lock.lock()
    active = true
    lock.unlock()
    cbQueue.async { [self] in
      if manager == nil { manager = CBPeripheralManager(delegate: self, queue: cbQueue) }
    }
  }

  private func parseProfile(_ profile: UnsafePointer<UInt8>) {
    func u16(_ p: Int) -> UInt16 { UInt16(profile[p]) | (UInt16(profile[p + 1]) << 8) }

    var pos = 1   // skip ATT_DB_VERSION
    var specs: [PBLEServiceSpec] = []
    var reservedService = false
    var pendingDecl: (props: UInt8, valueHandle: UInt16, uuid: [UInt8])? = nil

    while true {
      let len = Int(u16(pos))
      if len == 0 { break }                 // 0x0000 terminator
      let contentStart = pos + 2
      let flags = u16(contentStart)
      let handle = u16(contentStart + 2)
      let bodyStart = contentStart + 4
      let bodyEnd = pos + len
      var body: [UInt8] = []
      var i = bodyStart
      while i < bodyEnd { body.append(profile[i]); i += 1 }
      pos += len

      guard body.count >= 2 else { continue }
      let leadUuid = UInt16(body[0]) | (UInt16(body[1]) << 8)

      switch leadUuid {
      case 0x2800, 0x2801:   // primary / secondary service declaration
        let svcUuidBytes = Array(body[2...])
        reservedService = isReservedService(svcUuidBytes)
        if !reservedService {
          specs.append(PBLEServiceSpec(uuid: cbuuid(from: svcUuidBytes), characteristics: []))
        }
        pendingDecl = nil

      case 0x2803:           // characteristic declaration
        // body: [0x2803][props(1)][value_handle(2 LE)][char_uuid]
        let props = body[2]
        let valueHandle = UInt16(body[3]) | (UInt16(body[4]) << 8)
        let charUuid = Array(body[5...])
        pendingDecl = (props, valueHandle, charUuid)

      default:               // characteristic VALUE line or descriptor line
        if let decl = pendingDecl, handle == decl.valueHandle {
          // characteristic value: uuid == char_uuid (known length from the decl)
          let uuidLen = decl.uuid.count
          let value = body.count > uuidLen ? Array(body[uuidLen...]) : []
          if !reservedService, !specs.isEmpty {
            let spec = PBLECharSpec(
              uuid: cbuuid(from: decl.uuid),
              properties: cbProperties(decl.props),
              permissions: cbPermissions(decl.props),
              valueHandle: handle,
              initialValue: value,
              cccdHandle: nil)
            specs[specs.count - 1].characteristics.append(spec)
          }
          pendingDecl = nil
        } else {
          // descriptor for the most recent characteristic. UUID length: 16 when the
          // LONG_UUID flag (0x200) is set, else the 2-byte form (descriptors are
          // 0x29xx). A CCCD (0x2902) becomes the synthetic CCCD handle.
          let uuidLen = (flags & 0x0200) != 0 ? 16 : 2
          guard body.count >= uuidLen else { continue }
          let descUuid16 = UInt16(body[0]) | (UInt16(body[1]) << 8)
          if uuidLen == 2, descUuid16 == 0x2902,
             !reservedService, !specs.isEmpty,
             let last = specs[specs.count - 1].characteristics.last {
            let replaced = PBLECharSpec(
              uuid: last.uuid, properties: last.properties, permissions: last.permissions,
              valueHandle: last.valueHandle, initialValue: last.initialValue,
              cccdHandle: handle)
            specs[specs.count - 1].characteristics[specs[specs.count - 1].characteristics.count - 1] = replaced
          }
          // Non-CCCD descriptors are not surfaced to CoreBluetooth (it manages the
          // reserved ones); they need no handle mapping for the peripheral contract.
        }
      }
    }

    serviceSpecs = specs
    // Seed the read cache with the GATT initial values so a read before the first
    // pump tick still returns the declared value.
    var seed: [UInt16: [UInt8]] = [:]
    var handles: [UInt16] = []
    for s in specs {
      for c in s.characteristics {
        seed[c.valueHandle] = c.initialValue
        handles.append(c.valueHandle)
      }
    }
    lock.lock(); valueByAttHandle = seed; valueHandles = handles; lock.unlock()
  }

  // ---- power (VM thread) ----

  func powerOn() {
    cbQueue.async { [self] in
      powerRequested = true
      if manager?.state == .poweredOn { addServicesIfNeeded() }
    }
  }

  func powerOff() {
    cbQueue.async { [self] in
      powerRequested = false
      manager?.stopAdvertising()
    }
  }

  // ---- advertise (VM thread) ----

  /// adv_data is the raw AD-TLV blob built by mrblib/ble_advertising_data.rb.
  /// CoreBluetooth only advertises a local name + service UUIDs, so parse those
  /// two TLVs out of the blob and hand them to startAdvertising.
  func advertise(_ advData: [UInt8]) {
    cbQueue.async { [self] in
      var dict: [String: Any] = [:]
      var uuids: [CBUUID] = []
      var i = 0
      while i < advData.count {
        let fieldLen = Int(advData[i])
        if fieldLen == 0 || i + fieldLen >= advData.count + 1 { break }
        let type = advData[i + 1]
        let valStart = i + 2
        let valEnd = i + 1 + fieldLen
        guard valEnd <= advData.count else { break }
        let val = Array(advData[valStart..<valEnd])
        switch type {
        case 0x08, 0x09:   // shortened / complete local name
          dict[CBAdvertisementDataLocalNameKey] = String(decoding: val, as: UTF8.self)
        case 0x02, 0x03:   // 16-bit service class UUID list
          var j = 0
          while j + 1 < val.count {
            let u = UInt16(val[j]) | (UInt16(val[j + 1]) << 8)
            uuids.append(CBUUID(string: String(format: "%04X", u)))
            j += 2
          }
        default:
          break
        }
        i = valEnd
      }
      if !uuids.isEmpty { dict[CBAdvertisementDataServiceUUIDsKey] = uuids }
      manager?.stopAdvertising()
      manager?.startAdvertising(dict)
    }
  }

  func stopAdvertise() {
    cbQueue.async { [self] in manager?.stopAdvertising() }
  }

  // ---- notify / can-send-now (VM thread; bytes already copied in C) ----

  func notify(attHandle: UInt16, value: [UInt8]) {
    cbQueue.async { [self] in
      guard let ch = charByAttHandle[attHandle] else { return }
      manager?.updateValue(Data(value), for: ch, onSubscribedCentrals: nil)
    }
  }

  func requestCanSendNow() {
    // The notify rate here is low; signal readiness immediately. updateValue still
    // guards its own transmit queue, and peripheralManagerIsReadyToUpdateSubscribers
    // re-arms it if a burst ever backs up.
    pbleSharedFifo.push(pblePeripheralCanSendNow())
  }

  // ---- VM-thread pump (the ONLY site that calls into mruby) ----

  /// Driven from pble_drain_one each poll tick. Flush pending writes into the mruby
  /// write-value table and refresh the read cache from the mruby read-value table.
  func pump() {
    lock.lock()
    guard active else { lock.unlock(); return }
    let writes = pendingWrites
    pendingWrites.removeAll()
    let handles = valueHandles
    lock.unlock()

    for (h, bytes) in writes {
      bytes.withUnsafeBufferPointer { _ = BLE_write_data(h, $0.baseAddress, UInt16($0.count)) }
    }

    var fresh: [UInt16: [UInt8]] = [:]
    for h in handles {
      var rv = BLE_read_value_t(att_handle: h, data: nil, size: 0)
      if BLE_read_data(&rv) == 0, let d = rv.data, rv.size > 0 {
        fresh[h] = Array(UnsafeBufferPointer(start: d, count: Int(rv.size)))
      }
    }
    if !fresh.isEmpty {
      lock.lock()
      for (h, v) in fresh { valueByAttHandle[h] = v }
      lock.unlock()
    }
  }

  // ---- CBPeripheralManagerDelegate (cbQueue) ----

  /// Without this, CoreBluetooth rejecting the advertisement is indistinguishable
  /// from a backend that never advertised at all.
  func peripheralManagerDidStartAdvertising(_ peripheral: CBPeripheralManager, error: Error?) {
    if let error { print("[ports/darwin] advertising rejected: \(error.localizedDescription)") }
  }

  func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
    if peripheral.state == .poweredOn, powerRequested { addServicesIfNeeded() }
  }

  private func addServicesIfNeeded() {
    guard !servicesAdded, let mgr = manager else { return }
    servicesAdded = true
    charByAttHandle.removeAll()
    attHandleForChar.removeAll()
    cccdHandleForChar.removeAll()

    if serviceSpecs.isEmpty { pbleSharedFifo.push(pblePeripheralStateWorking()); return }
    pendingServiceAdds = serviceSpecs.count
    for spec in serviceSpecs {
      let svc = CBMutableService(type: spec.uuid, primary: true)
      var chars: [CBMutableCharacteristic] = []
      for c in spec.characteristics {
        // Dynamic value (nil): CoreBluetooth routes reads/writes to the delegate so
        // Ruby drives them. CB requires a nil value for notify/indicate/dynamic chars.
        let ch = CBMutableCharacteristic(type: c.uuid, properties: c.properties,
                                         value: nil, permissions: c.permissions)
        chars.append(ch)
        charByAttHandle[c.valueHandle] = ch
        attHandleForChar[ObjectIdentifier(ch)] = c.valueHandle
        if let cccd = c.cccdHandle { cccdHandleForChar[ObjectIdentifier(ch)] = cccd }
      }
      svc.characteristics = chars
      mgr.add(svc)
    }
  }

  func peripheralManager(_ peripheral: CBPeripheralManager, didAdd service: CBService, error: Error?) {
    pendingServiceAdds -= 1
    // Emit state-working once all services are registered, so the advertise() that
    // Ruby's packet_callback issues in response lands after the GATT DB exists.
    if pendingServiceAdds <= 0 { pbleSharedFifo.push(pblePeripheralStateWorking()) }
  }

  func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveRead request: CBATTRequest) {
    guard let h = attHandleForChar[ObjectIdentifier(request.characteristic)] else {
      peripheral.respond(to: request, withResult: .attributeNotFound); return
    }
    lock.lock(); let bytes = valueByAttHandle[h] ?? []; lock.unlock()
    let data = Data(bytes)
    if request.offset > data.count {
      peripheral.respond(to: request, withResult: .invalidOffset); return
    }
    request.value = data.subdata(in: request.offset..<data.count)
    peripheral.respond(to: request, withResult: .success)
  }

  func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveWrite requests: [CBATTRequest]) {
    lock.lock()
    for req in requests {
      if let h = attHandleForChar[ObjectIdentifier(req.characteristic)] {
        pendingWrites.append((h, req.value.map { [UInt8]($0) } ?? []))
      }
    }
    lock.unlock()
    if let first = requests.first { peripheral.respond(to: first, withResult: .success) }
  }

  func peripheralManager(_ peripheral: CBPeripheralManager, central: CBCentral,
                         didSubscribeTo characteristic: CBCharacteristic) {
    if !mtuAnnounced { mtuAnnounced = true; pbleSharedFifo.push(pblePeripheralMtuComplete()) }
    if let cccd = cccdHandleForChar[ObjectIdentifier(characteristic)] {
      lock.lock(); pendingWrites.append((cccd, [0x01, 0x00])); lock.unlock()
    }
  }

  func peripheralManager(_ peripheral: CBPeripheralManager, central: CBCentral,
                         didUnsubscribeFrom characteristic: CBCharacteristic) {
    if let cccd = cccdHandleForChar[ObjectIdentifier(characteristic)] {
      lock.lock(); pendingWrites.append((cccd, [0x00, 0x00])); lock.unlock()
    }
    pbleSharedFifo.push(pblePeripheralDisconnect())
  }

  // ---- UUID / property / permission helpers ----

  private func isReservedService(_ uuidBytes: [UInt8]) -> Bool {
    // GAP (0x1800) and GATT (0x1801) are owned by CoreBluetooth; adding them throws.
    guard uuidBytes.count == 2 else { return false }
    let u = UInt16(uuidBytes[0]) | (UInt16(uuidBytes[1]) << 8)
    return u == 0x1800 || u == 0x1801
  }

  /// uuid bytes are LSB-first for 16-/32-bit (Utils.int16/32_to_little_endian); a
  /// 16-byte UUID is the app-supplied string, taken as CBUUID.data (big-endian).
  private func cbuuid(from bytes: [UInt8]) -> CBUUID {
    switch bytes.count {
    case 2:
      let u = UInt16(bytes[0]) | (UInt16(bytes[1]) << 8)
      return CBUUID(string: String(format: "%04X", u))
    case 4:
      let u = UInt32(bytes[0]) | (UInt32(bytes[1]) << 8) | (UInt32(bytes[2]) << 16) | (UInt32(bytes[3]) << 24)
      return CBUUID(string: String(format: "%08X", u))
    default:
      return CBUUID(data: Data(bytes))
    }
  }

  private func cbProperties(_ p: UInt8) -> CBCharacteristicProperties {
    var out: CBCharacteristicProperties = []
    if p & 0x01 != 0 { out.insert(.broadcast) }
    if p & 0x02 != 0 { out.insert(.read) }
    if p & 0x04 != 0 { out.insert(.writeWithoutResponse) }
    if p & 0x08 != 0 { out.insert(.write) }
    if p & 0x10 != 0 { out.insert(.notify) }
    if p & 0x20 != 0 { out.insert(.indicate) }
    if p & 0x40 != 0 { out.insert(.authenticatedSignedWrites) }
    if p & 0x80 != 0 { out.insert(.extendedProperties) }
    return out
  }

  private func cbPermissions(_ p: UInt8) -> CBAttributePermissions {
    var out: CBAttributePermissions = []
    if p & 0x02 != 0 { out.insert(.readable) }
    if p & (0x04 | 0x08) != 0 { out.insert(.writeable) }
    return out
  }
}
#endif  // !os(watchOS)
