// C-callable surface for the Darwin port (ports/darwin/*.c). All use `@c`
// (SE-0495); `@_cdecl` emits the symbol but not the declaration in the generated
// -Swift.h, causing implicit-function-declaration errors. Direction is C -> Swift
// only; Swift never calls back into the gem's C (the FIFO + drain live here so no
// `-undefined dynamic_lookup` is needed).

@c public func pble_central_init(_ role: Int32) {
  PBLECentral.shared.setup(role: role)
}

@c public func pble_power_on() { PBLECentral.shared.powerOn() }

@c public func pble_power_off() { PBLECentral.shared.powerOff() }

@c public func pble_start_scan() { PBLECentral.shared.startScan() }

@c public func pble_stop_scan() { PBLECentral.shared.stopScan() }

@c public func pble_connect(_ addr: UnsafePointer<UInt8>, _ addrType: UInt8) -> Int32 {
  PBLECentral.shared.connect(addr: addr, addrType: addrType)
}

@c public func pble_discover_services(_ conn: UInt16) {
  PBLECentral.shared.discoverServices(conn)
}

@c public func pble_discover_characteristics(_ conn: UInt16, _ start: UInt8, _ end: UInt8) {
  PBLECentral.shared.discoverCharacteristics(conn, start, end)
}

@c public func pble_read_value(_ conn: UInt16, _ handle: UInt8) {
  PBLECentral.shared.readValue(conn, handle)
}

@c public func pble_discover_descriptors(_ conn: UInt16, _ value: UInt8, _ end: UInt8) {
  PBLECentral.shared.discoverDescriptors(conn, value, end)
}

@c public func pble_write_value(_ conn: UInt16, _ valueHandle: UInt8, _ data: UnsafePointer<UInt8>, _ size: UInt16) {
  let bytes = [UInt8](UnsafeBufferPointer(start: data, count: Int(size)))
  PBLECentral.shared.writeValueWithoutResponse(conn, valueHandle, bytes)
}

@c public func pble_write_descriptor(_ conn: UInt16, _ descHandle: UInt8, _ data: UnsafePointer<UInt8>, _ size: UInt16) {
  let bytes = [UInt8](UnsafeBufferPointer(start: data, count: Int(size)))
  PBLECentral.shared.writeDescriptor(conn, descHandle, bytes)
}

// ---- peripheral / broadcaster roles (ports/darwin/ble.c, ble_peripheral.c) ----

/// `profile` is NULL for the broadcaster role, which advertises without a GATT database.
@c public func pble_peripheral_init(_ profile: UnsafePointer<UInt8>?) {
  PBLEPeripheral.shared.setup(profile: profile)
}

@c public func pble_peripheral_power_on() { PBLEPeripheral.shared.powerOn() }

@c public func pble_peripheral_power_off() { PBLEPeripheral.shared.powerOff() }

@c public func pble_peripheral_advertise(_ data: UnsafePointer<UInt8>, _ size: UInt16) {
  PBLEPeripheral.shared.advertise([UInt8](UnsafeBufferPointer(start: data, count: Int(size))))
}

@c public func pble_peripheral_stop_advertise() { PBLEPeripheral.shared.stopAdvertise() }

@c public func pble_peripheral_notify(_ attHandle: UInt16, _ data: UnsafePointer<UInt8>, _ size: UInt16) {
  PBLEPeripheral.shared.notify(attHandle: attHandle,
                               value: [UInt8](UnsafeBufferPointer(start: data, count: Int(size))))
}

@c public func pble_peripheral_request_can_send_now() {
  PBLEPeripheral.shared.requestCanSendNow()
}

/// VM-thread drain: copy one queued packet into `buf` (capacity `cap`); returns
/// the packet length, or 0 when empty or when an oversize packet was dropped.
/// Also pumps the peripheral backend (flush pending writes + refresh the read
/// cache) here, on the VM thread — the one place it may touch mruby. A no-op when
/// the peripheral backend is inactive (central/observer builds).
@c public func pble_drain_one(_ buf: UnsafeMutablePointer<UInt8>, _ cap: Int32) -> Int32 {
  PBLEPeripheral.shared.pump()
  return pbleSharedFifo.drainInto(buf, Int(cap))
}
