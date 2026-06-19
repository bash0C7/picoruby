import Foundation

/// Thread-safe FIFO of pre-serialized BTstack-format packets. The CoreBluetooth
/// delegate (a background serial queue) pushes; the mruby VM thread drains one
/// packet per 100ms poll tick. Holds raw bytes only — it never touches mruby, so
/// no mrb_* call ever happens off the VM thread.
public final class PBLEFifo: @unchecked Sendable {   // thread-safe via NSLock
  private let lock = NSLock()
  private var queue: [[UInt8]] = []

  public init() {}

  public func push(_ packet: [UInt8]) {
    lock.lock()
    queue.append(packet)
    lock.unlock()
  }

  /// Drop all queued packets. Used when scanning stops: a backlog of advertising
  /// reports must not delay the time-critical connection-complete packet (an idle
  /// post-connect central gets dropped by the peripheral).
  public func flush() {
    lock.lock()
    queue.removeAll()
    lock.unlock()
  }

  /// VM-thread drain: copy one packet into `buf` (capacity `cap`). Returns the
  /// packet length, 0 when empty or when an oversize packet was dropped.
  public func drainInto(_ buf: UnsafeMutablePointer<UInt8>, _ cap: Int) -> Int32 {
    lock.lock()
    defer { lock.unlock() }
    guard let first = queue.first else { return 0 }
    if first.count > cap {
      // Oversize packet cannot fit the drain buffer. Drop it (mirrors the 255-handle
      // cap discipline in finalizeDiscovery) and return 0 = "nothing this tick" so it
      // cannot wedge the FIFO head; a stuck head would stall every later packet and
      // park the decoder FSM until timeout.
      print("[ports/darwin] dropping \(first.count)-byte packet > drain buffer \(cap)")
      queue.removeFirst()
      return 0
    }
    for i in 0..<first.count { buf[i] = first[i] }
    queue.removeFirst()
    return Int32(first.count)
  }
}

/// The single FIFO shared between the CoreBluetooth delegate and the VM-thread
/// drain (`pble_drain_one`).
let pbleSharedFifo = PBLEFifo()
