# End-to-end test: drive the real CoreBluetooth central against ANY BLE
# peripheral (the Swift fixture, or an iOS/Android app like nRF Connect) over
# the live radio. Requires Bluetooth permission for the running binary and a
# peripheral on a SECOND device — a Mac acting as central cannot receive
# advertisements from a peripheral on the same Mac.
#
# Skipped unless RUN_E2E=1 because this needs a real radio + a 2nd device.
#
#   # On a 2nd device (Mac shown; iOS nRF Connect also works):
#   swift mrbgems/picoruby-ble/ports/darwin/test/test_peripheral.swift &
#
#   # On the central Mac:
#   RUN_E2E=1 build/host/bin/picoruby \
#     mrbgems/picoruby-ble/ports/darwin/test/e2e_central_test.rb
#
# Target selection is robust to peripherals that don't advertise a name:
#   * if TARGET_NAME (env, default "PBLE-TEST") matches, connect to that
#   * else connect to the strongest-RSSI device seen
# PASS = connected, >=1 service discovered, >=1 characteristic value read.

require "picotest"

class E2ECentral < BLE
  attr_reader :picked, :seen
  def initialize(role, target_name)
    super(role)
    @target_name = target_name
    @seen = []           # [[rssi, address, address_type, name]]
    @picked = nil
  end

  def advertising_report_callback(r)
    return unless @state == :TC_W4_SCAN_RESULT
    name = r.reports[:complete_local_name] || r.reports[:shortened_local_name]
    @seen << [r.rssi, r.address, r.address_type_code, name]
    if !@target_name.empty? && name && name.include?(@target_name)
      pick(r.address, r.address_type_code, name, r.rssi)
    end
  end

  def pick(addr, atype, name, rssi)
    return if @picked
    @picked = [addr, atype, name, rssi]
    STDOUT.puts "[central] connecting to #{name.inspect} rssi=#{rssi}"
    stop_scan
    @state = :TC_W4_CONNECT
    gap_connect(addr, atype)
  end

  def connect_strongest
    best = nil
    @seen.each { |e| best = e if best.nil? || e[0] > best[0] }
    return false unless best
    STDOUT.puts "[central] no name match; strongest device rssi=#{best[0]} name=#{best[3].inspect}"
    pick(best[1], best[2], best[3], best[0])
    true
  end
end

class E2ECentralTest < Picotest::Test
  TARGET_NAME = ENV["TARGET_NAME"] || "PBLE-TEST"
  SCAN_MS     = (ENV["SCAN_MS"] || "12000").to_i

  def setup
    skip "set RUN_E2E=1 to exercise a live radio (needs a 2nd-device peripheral)" \
      unless ENV["RUN_E2E"] == "1"
    @c = E2ECentral.new(:central, TARGET_NAME)
    # Phase 1: scan; if a named target appears, advertising_report_callback
    # connects immediately and the loop runs through discovery to :TC_IDLE.
    @c.scan(timeout_ms: SCAN_MS, stop_state: :TC_IDLE, debug: true)
    # Phase 2: if nothing was picked by name, connect to the strongest device
    # and run discovery in a fresh poll loop.
    if !@c.picked && @c.connect_strongest
      @c.start(15000, :TC_IDLE)
    end
  end

  def test_reached_idle
    assert_equal(:TC_IDLE, @c.state)
  end

  def test_at_least_one_service
    assert(@c.services.size >= 1)
  end

  def test_at_least_one_characteristic_value_read
    read_any = @c.services.any? { |s| s[:characteristics].any? { |ch| !ch[:value].nil? } }
    assert(read_any)
  end
end

# Self-driver (see decoder_contract_test.rb for rationale).
test = E2ECentralTest.new
puts "E2ECentralTest"
test.list_tests.reverse.each do |m|
  fresh = E2ECentralTest.new
  print "  #{m} "
  begin
    fresh.setup
    fresh.send(m)
  rescue Picotest::Skip => e
    fresh.report_skip({ method: m.to_s, reason: e.message })
  rescue => e
    fresh.report_exception({ method: m.to_s, raise_message: "#{e.class}: #{e.message}" })
  ensure
    begin
      fresh.teardown
    rescue => e
      fresh.report_exception({ method: m.to_s, raise_message: "teardown #{e.class}: #{e.message}" })
    end
  end
  test.instance_variable_get(:@result)["success_count"] += fresh.result["success_count"]
  test.instance_variable_get(:@result)["failures"].concat(fresh.result["failures"])
  test.instance_variable_get(:@result)["exceptions"].concat(fresh.result["exceptions"])
  test.instance_variable_get(:@result)["skipped"].concat(fresh.result["skipped"])
  test.instance_variable_get(:@result)["skipped_count"] += fresh.result["skipped_count"]
  puts
end

r = test.result
puts ""
puts "success: #{r["success_count"]}, failure: #{r["failures"].size}, exception: #{r["exceptions"].size}, skip: #{r["skipped_count"]}"
r["failures"].each do |f|
  puts "  FAIL #{f[:method] || f["method"]}: #{f[:error_message] || f["error_message"]}"
end
r["exceptions"].each do |e|
  puts "  ERR  #{e[:method] || e["method"]}: #{e[:raise_message] || e["raise_message"]}"
end
(r["skipped"] || []).each do |s|
  puts "  SKIP #{s[:method] || s["method"]}: #{s[:reason] || s["reason"]}"
end
exit(r["failures"].empty? && r["exceptions"].empty? ? 0 : 1)
