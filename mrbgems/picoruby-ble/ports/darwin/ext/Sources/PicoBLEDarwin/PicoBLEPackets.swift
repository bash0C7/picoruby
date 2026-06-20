// BTstack-format packet builders for the picoruby-ble decoder (mrblib/ble_central.rb).
// Pure functions: (handles / UUID / value) -> [UInt8]. The synthesis target is the
// decoder's byteslice offsets, NOT real BTstack 1.6.2 (which is +4 bytes on GATT
// structs). byte[1] is a filler the decoder
// never reads; kept 0x01 for parity with how BTstack events carry a length there.

// Bluetooth Base UUID suffix (canonical bytes 4..15): -0000-1000-8000-00805F9B34FB.
private let baseUuidSuffix: [UInt8] = [0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB]

public func pbleStateWorking() -> [UInt8] { [0x60, 0x01, 0x02] }

public func pbleQueryComplete() -> [UInt8] { [0xA0, 0x01] }

public func pbleConnComplete(_ connHandle: UInt16) -> [UInt8] {
  [0x3E, 0x01, 0x01, 0x00, UInt8(connHandle & 0xff), UInt8((connHandle >> 8) & 0xff)]
}

public func pbleDisconnect() -> [UInt8] { [0x3E, 0x01, 0x05] }

/// 16-bit UUID v -> 128-bit canonical (MSB) 0000vvvv-0000-1000-8000-00805F9B34FB.
public func pbleUuid16Canonical(_ v: UInt16) -> [UInt8] {
  [0x00, 0x00, UInt8((v >> 8) & 0xff), UInt8(v & 0xff)] + baseUuidSuffix
}

/// canonical (MSB, 16 bytes) -> wire (LSB-first); the decoder's reverse_128() undoes this.
public func pbleUuidWire(fromCanonical canonical: [UInt8]) -> [UInt8] {
  Array(canonical.reversed())
}

/// CBUUID.data is big-endian: 2 bytes (16-bit) or 16 bytes (128-bit). -> 16-byte wire.
public func pbleUuidWire(fromCBUUIDData data: [UInt8]) -> [UInt8] {
  let canonical: [UInt8]
  if data.count == 2 {
    canonical = pbleUuid16Canonical((UInt16(data[0]) << 8) | UInt16(data[1]))
  } else {
    canonical = data
  }
  return pbleUuidWire(fromCanonical: canonical)
}

/// 0xA1 GATT_EVENT_SERVICE_QUERY_RESULT: start(4), end(6), uuid128(8..23).
public func pbleServiceResult(start: UInt8, end: UInt8, uuidWire: [UInt8]) -> [UInt8] {
  [0xA1, 0x01, 0x00, 0x00, start, 0x00, end, 0x00] + uuidWire
}

/// 0xA2 GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: start(4), value(6), end(8), properties(10), uuid128(12..27).
public func pbleCharacteristicResult(start: UInt8, value: UInt8, end: UInt8, properties: UInt8, uuidWire: [UInt8]) -> [UInt8] {
  [0xA2, 0x01, 0x00, 0x00, start, 0x00, value, 0x00, end, 0x00, properties, 0x00] + uuidWire
}

/// 0xA4 GATT_EVENT_ALL_CHARACTERISTIC_DESCRIPTORS_QUERY_RESULT: handle(4), uuid128 at offset 6 (NOT 8).
public func pbleDescriptorResult(handle: UInt8, uuidWire: [UInt8]) -> [UInt8] {
  [0xA4, 0x01, 0x00, 0x00, handle, 0x00] + uuidWire
}

/// 0xA5 GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT: value_handle(4), len(6), value(8..). len is 1 byte (<=255).
public func pbleValueResult(valueHandle: UInt8, value: [UInt8]) -> [UInt8] {
  let len = min(value.count, 255)
  return [0xA5, 0x01, 0x00, 0x00, valueHandle, 0x00, UInt8(len), 0x00] + value.prefix(len)
}

/// 0xA7 GATT_EVENT_NOTIFICATION: same byte layout as 0xA5 — value_handle(4),
/// len(6), value(8..). The canonical decoder (ble_central.rb) leaves 0xA7 as a
/// no-op, so the application subclass parses these offsets; kept identical to
/// pbleValueResult so the same byteslice reader serves both.
public func pbleNotification(valueHandle: UInt8, value: [UInt8]) -> [UInt8] {
  let len = min(value.count, 255)
  return [0xA7, 0x01, 0x00, 0x00, valueHandle, 0x00, UInt8(len), 0x00] + value.prefix(len)
}

/// 0xda GAP_EVENT_ADVERTISING_REPORT: addr_type(3), addr(4..9 LSB-first), rssi=(rssi+256)&0xff(10),
/// adv-data len(11), AD TLV(12..). AdvertisingReport requires >=14 bytes.
public func pbleAdvReport(addr: [UInt8], addrType: UInt8, rssi: Int, advData: [UInt8]) -> [UInt8] {
  let advLen = min(advData.count, 255)
  var p: [UInt8] = [0xDA, 0x01, 0x00, addrType]
  p.append(contentsOf: addr.prefix(6))
  while p.count < 10 { p.append(0x00) }
  p.append(UInt8((rssi + 256) & 0xff))
  p.append(UInt8(advLen))
  p.append(contentsOf: advData.prefix(advLen))
  while p.count < 14 { p.append(0x00) }
  return p
}
