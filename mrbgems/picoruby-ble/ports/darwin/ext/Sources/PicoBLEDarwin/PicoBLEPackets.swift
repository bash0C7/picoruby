// BTstack-format packet builders for the picoruby-ble decoder (mrblib/ble_central.rb).
// Pure functions: (handles / UUID / value) -> [UInt8].
//
// GATT client events use the BTstack 1.6+ layout the shared decoder reads
// (GATT_EVENT_PAYLOAD_OFFSET = 8, same synthesis as ports/esp32/ble.c):
//   [0] event type, [1] length (total - 2), [2..3] con_handle (LE),
//   [4..5] service_id, [6..7] connection_id, [8..] payload.
// The decoder never reads con_handle/service_id/connection_id from GATT
// events (it tracks the connection itself), so those stay zero here.
// HCI-layer events (0x60/0x3E/0x05/0xB5/0xB7/0xDA) keep their own layouts.

// Bluetooth Base UUID suffix (canonical bytes 4..15): -0000-1000-8000-00805F9B34FB.
private let baseUuidSuffix: [UInt8] = [0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB]

/// 8-byte GATT event header; length byte covers everything after [0..1].
private func gattEventHeader(_ type: UInt8, payloadLength: Int) -> [UInt8] {
  [type, UInt8(min(6 + payloadLength, 255)), 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
}

public func pbleStateWorking() -> [UInt8] { [0x60, 0x01, 0x02] }

// ---- peripheral-role events (consumed by a BLE peripheral subclass's packet_callback) ----
// The peripheral decoder branches on event_packet[0] (and [2] for state); these carry
// the codes mrblib/ble.rb's BTSTACK_EVENT_STATE / HCI_EVENT_DISCONNECTION_COMPLETE /
// ATT_EVENT_MTU_EXCHANGE_COMPLETE / ATT_EVENT_CAN_SEND_NOW peers expect.
public func pblePeripheralStateWorking() -> [UInt8] { [0x60, 0x01, 0x02] }
public func pblePeripheralDisconnect() -> [UInt8] { [0x05, 0x01, 0x00] }
public func pblePeripheralMtuComplete() -> [UInt8] { [0xB5, 0x01, 0x00] }
public func pblePeripheralCanSendNow() -> [UInt8] { [0xB7, 0x01, 0x00] }

/// 0xA0 GATT_EVENT_QUERY_COMPLETE: status(8). The decoder only branches on the
/// event type, but the full header + status keeps parity with ports/esp32.
public func pbleQueryComplete() -> [UInt8] {
  gattEventHeader(0xA0, payloadLength: 1) + [0x00]
}

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

/// 0xA1 GATT_EVENT_SERVICE_QUERY_RESULT: start(8), end(10), uuid128(12..27).
public func pbleServiceResult(start: UInt8, end: UInt8, uuidWire: [UInt8]) -> [UInt8] {
  gattEventHeader(0xA1, payloadLength: 4 + 16) + [start, 0x00, end, 0x00] + uuidWire
}

/// 0xA2 GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: start(8), value(10), end(12),
/// properties(14, LE16), uuid128(16..31).
public func pbleCharacteristicResult(start: UInt8, value: UInt8, end: UInt8, properties: UInt8, uuidWire: [UInt8]) -> [UInt8] {
  gattEventHeader(0xA2, payloadLength: 8 + 16) +
    [start, 0x00, value, 0x00, end, 0x00, properties, 0x00] + uuidWire
}

/// 0xA4 GATT_EVENT_ALL_CHARACTERISTIC_DESCRIPTORS_QUERY_RESULT: handle(8), uuid128(10..25).
public func pbleDescriptorResult(handle: UInt8, uuidWire: [UInt8]) -> [UInt8] {
  gattEventHeader(0xA4, payloadLength: 2 + 16) + [handle, 0x00] + uuidWire
}

/// 0xA5 GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT: value_handle(8), len(10, LE16), value(12..).
public func pbleValueResult(valueHandle: UInt8, value: [UInt8]) -> [UInt8] {
  let len = min(value.count, 255)
  return gattEventHeader(0xA5, payloadLength: 4 + len) +
    [valueHandle, 0x00, UInt8(len), 0x00] + value.prefix(len)
}

/// 0xA7 GATT_EVENT_NOTIFICATION: same payload layout as 0xA5 — value_handle(8),
/// len(10, LE16), value(12..). The canonical decoder (ble_central.rb) leaves 0xA7
/// to the application subclass; gatt_event_value() reads these offsets.
public func pbleNotification(valueHandle: UInt8, value: [UInt8]) -> [UInt8] {
  let len = min(value.count, 255)
  return gattEventHeader(0xA7, payloadLength: 4 + len) +
    [valueHandle, 0x00, UInt8(len), 0x00] + value.prefix(len)
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
