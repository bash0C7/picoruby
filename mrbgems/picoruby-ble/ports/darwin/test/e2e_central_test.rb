# Live-radio E2E for the CoreBluetooth central. Needs a peripheral on a 2nd
# device (a Mac central cannot see a same-Mac peripheral). Skipped without
# RUN_E2E=1. See ports/darwin/README.md for the full procedure.

require "picotest"

class E2ECentral < BLE
  attr_reader :picked, :seen
  def initialize(role, target_name)
    super(role)
    @target_name = target_name
    @seen = []
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

  # One test method so the 12s scan runs once, not once per assertion.
  def test_scan_connect_discover_read
    skip "RUN_E2E=1 not set" unless ENV["RUN_E2E"] == "1"

    c = E2ECentral.new(:central, TARGET_NAME)
    c.scan(timeout_ms: SCAN_MS, stop_state: :TC_IDLE, debug: true)
    c.start(15000, :TC_IDLE) if !c.picked && c.connect_strongest

    STDOUT.puts "[central] discovery done; state=#{c.state} services=#{c.services.size}"
    c.services.each do |s|
      STDOUT.puts "  svc uuid32=#{sprintf('0x%08X', s[:uuid32] || 0)} #{s[:start_handle]}..#{s[:end_handle]}"
      s[:characteristics].each do |ch|
        STDOUT.puts "    char uuid32=#{sprintf('0x%08X', ch[:uuid32] || 0)} vh=#{ch[:value_handle]} props=#{ch[:properties]} value=#{ch[:value].inspect}"
        ch[:descriptors].each do |d|
          STDOUT.puts "      desc uuid32=#{sprintf('0x%08X', d[:uuid32] || 0)} handle=#{d[:handle]} value=#{d[:value].inspect}"
        end
      end
    end

    # state == :TC_IDLE is transient — the scan loop stops on it ("Stopped by
    # state: TC_IDLE" in the trace) and the central then transitions to
    # :TC_OFF. Discovery + read completion are what PASS actually means.
    assert(c.services.size >= 1)
    assert(c.services.any? { |s| s[:characteristics].any? { |ch| !ch[:value].nil? } })
  end
end

# Picotest::Runner is CRuby-only (spawns target VM as subprocess); drive
# in-process so this file runs via picoruby directly.
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
r["failures"].each   { |f| puts "  FAIL #{f[:method] || f["method"]}: #{f[:error_message] || f["error_message"]}" }
r["exceptions"].each { |e| puts "  ERR  #{e[:method] || e["method"]}: #{e[:raise_message]  || e["raise_message"]}" }
(r["skipped"] || []).each { |s| puts "  SKIP #{s[:method] || s["method"]}: #{s[:reason] || s["reason"]}" }
exit(r["failures"].empty? && r["exceptions"].empty? ? 0 : 1)
