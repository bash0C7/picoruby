import XCTest
import CoreBluetooth
@testable import PicoBLEDarwin

final class DisconnectPacketTests: XCTestCase {
  private func cbError(_ code: Int) -> Error {
    NSError(domain: CBErrorDomain, code: code)
  }

  func testDisconnectPacketCarriesTheReasonByte() {
    XCTAssertEqual(pbleDisconnect(reason: 0x06), [0x3E, 0x02, 0x05, 0x06])
  }

  func testNoErrorIsRemoteUserTerminated() {
    XCTAssertEqual(pbleDisconnectReason(for: nil), 0x16)
  }

  func testPeerRemovedPairingInformationIsKeyMissing() {
    XCTAssertEqual(pbleDisconnectReason(for: cbError(14)), 0x06)
  }

  func testEncryptionTimedOutIsAuthenticationFailure() {
    XCTAssertEqual(pbleDisconnectReason(for: cbError(15)), 0x05)
  }

  func testConnectionTimeoutIsConnectionTimeout() {
    XCTAssertEqual(pbleDisconnectReason(for: cbError(6)), 0x08)
  }

  func testPeripheralDisconnectedIsRemoteUserTerminated() {
    XCTAssertEqual(pbleDisconnectReason(for: cbError(7)), 0x13)
  }

  func testOtherCoreBluetoothErrorIsUnspecified() {
    XCTAssertEqual(pbleDisconnectReason(for: cbError(1)), 0x3E)
  }

  func testErrorOutsideTheCoreBluetoothDomainIsUnspecified() {
    XCTAssertEqual(pbleDisconnectReason(for: NSError(domain: "X", code: 14)), 0x3E)
  }
}
