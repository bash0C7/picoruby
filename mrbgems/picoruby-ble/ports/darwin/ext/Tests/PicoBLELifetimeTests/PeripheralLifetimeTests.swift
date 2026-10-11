import XCTest
@testable import PicoBLELifetime

private final class Peer {}

final class PeripheralLifetimeTests: XCTestCase {
  func testConnectNeverCompletedIsGivenBackAtPowerOff() {
    var life = PeripheralLifetime<Peer>()
    let p = Peer()
    life.hold(p)
    XCTAssertTrue(life.giveBackAtPowerOff() === p)
    XCTAssertNil(life.held)
  }

  func testCompletionAfterPowerOffGiveBackIsAStrangerToCancel() {
    var life = PeripheralLifetime<Peer>()
    let p = Peer()
    life.hold(p)
    _ = life.giveBackAtPowerOff()
    XCTAssertEqual(life.connectCompleted(p), .cancelStranger)
    XCTAssertFalse(life.linked)
  }

  func testEstablishedLinkSurvivesPowerOff() {
    var life = PeripheralLifetime<Peer>()
    let p = Peer()
    life.hold(p)
    XCTAssertEqual(life.connectCompleted(p), .adopt)
    XCTAssertNil(life.giveBackAtPowerOff())
    XCTAssertTrue(life.held === p)
    XCTAssertTrue(life.linked)
  }

  func testScanStartGivesBackAnEstablishedLinkAndItsLateDisconnectIsNotReported() {
    var life = PeripheralLifetime<Peer>()
    let p = Peer()
    life.hold(p)
    _ = life.connectCompleted(p)
    XCTAssertTrue(life.giveBackAtScanStart() === p)
    XCTAssertNil(life.held)
    XCTAssertFalse(life.linked)
    XCTAssertFalse(life.lost(p))
  }

  func testScanStartGivesBackAPendingConnect() {
    var life = PeripheralLifetime<Peer>()
    let p = Peer()
    life.hold(p)
    XCTAssertTrue(life.giveBackAtScanStart() === p)
    XCTAssertNil(life.held)
  }

  func testDisconnectOfTheHeldLinkedPeripheralIsReportedAndForgetsIt() {
    var life = PeripheralLifetime<Peer>()
    let p = Peer()
    life.hold(p)
    _ = life.connectCompleted(p)
    XCTAssertTrue(life.lost(p))
    XCTAssertNil(life.held)
    XCTAssertFalse(life.linked)
  }

  func testFailureOfTheHeldPendingPeripheralIsReportedAndForgetsIt() {
    var life = PeripheralLifetime<Peer>()
    let p = Peer()
    life.hold(p)
    XCTAssertTrue(life.lost(p))
    XCTAssertNil(life.held)
  }

  func testFailureOfAStrangerIsIgnored() {
    var life = PeripheralLifetime<Peer>()
    let held = Peer()
    life.hold(held)
    XCTAssertFalse(life.lost(Peer()))
    XCTAssertTrue(life.held === held)
  }

  func testCompletionOfAStrangerWhileAnotherIsHeldLeavesTheHeldOneAlone() {
    var life = PeripheralLifetime<Peer>()
    let held = Peer()
    life.hold(held)
    XCTAssertEqual(life.connectCompleted(Peer()), .cancelStranger)
    XCTAssertTrue(life.held === held)
    XCTAssertFalse(life.linked)
  }

  func testNothingHeldMeansNothingToGiveBack() {
    var life = PeripheralLifetime<Peer>()
    XCTAssertNil(life.giveBackAtPowerOff())
    XCTAssertNil(life.giveBackAtScanStart())
    XCTAssertFalse(life.lost(Peer()))
    XCTAssertEqual(life.connectCompleted(Peer()), .cancelStranger)
  }

  func testHoldingANewPeripheralStartsUnlinked() {
    var life = PeripheralLifetime<Peer>()
    let a = Peer()
    life.hold(a)
    _ = life.connectCompleted(a)
    life.hold(Peer())
    XCTAssertFalse(life.linked)
  }
}
