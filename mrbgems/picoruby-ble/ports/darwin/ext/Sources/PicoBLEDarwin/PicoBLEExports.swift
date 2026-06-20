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

/// VM-thread drain: copy one queued packet into `buf` (capacity `cap`); returns
/// the packet length, or 0 when empty or when an oversize packet was dropped.
@c public func pble_drain_one(_ buf: UnsafeMutablePointer<UInt8>, _ cap: Int32) -> Int32 {
  pbleSharedFifo.drainInto(buf, Int(cap))
}
