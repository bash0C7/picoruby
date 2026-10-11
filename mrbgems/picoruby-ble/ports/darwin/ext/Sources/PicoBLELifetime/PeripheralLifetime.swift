/// Which peripheral the central holds and whether its link is established.
///
/// Knows nothing about CoreBluetooth so the decisions run under `swift test`;
/// the adapter performs whatever an operation answers. Peripherals compare by
/// identity.
public struct PeripheralLifetime<Peripheral: AnyObject> {
  public enum Completion: Equatable {
    case adopt
    case cancelStranger
  }

  /// Set when connect is issued, cleared when the peripheral is given back or lost.
  public private(set) var held: Peripheral?
  /// True from connect-complete of the held peripheral until it is given back or lost.
  public private(set) var linked = false

  public init() {}

  public mutating func hold(_ peripheral: Peripheral) {
    held = peripheral
    linked = false
  }

  /// A connect completed. Anything but the held peripheral was given back earlier
  /// and must be cancelled, or the OS keeps a link nobody uses.
  public mutating func connectCompleted(_ peripheral: Peripheral) -> Completion {
    guard let h = held, h === peripheral else { return .cancelStranger }
    linked = true
    return .adopt
  }

  /// A connect failed or a link dropped. True only for the held peripheral, which is
  /// forgotten; a stranger's callback changes nothing.
  public mutating func lost(_ peripheral: Peripheral) -> Bool {
    guard let h = held, h === peripheral else { return false }
    held = nil
    linked = false
    return true
  }

  /// The decoder starts every scan from "not connected": whatever is held, pending
  /// or linked, is returned for cancelling.
  public mutating func giveBackAtScanStart() -> Peripheral? {
    let h = held
    held = nil
    linked = false
    return h
  }

  /// Power-off returns only a connect that has not completed; an established link
  /// stays so a caller can keep using it after scan returns.
  public mutating func giveBackAtPowerOff() -> Peripheral? {
    guard !linked else { return nil }
    return giveBackAtScanStart()
  }
}
