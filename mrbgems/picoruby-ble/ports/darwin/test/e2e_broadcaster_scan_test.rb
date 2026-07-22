# Live-radio E2E: proves a real BLE Broadcaster (e.g. the ESP32 NimBLE port's
# example/esp32/broadcaster-observer/broadcaster example) is visible and its
# payload changes over time. Uses :central purely as a scanner (never calls
# connect) — darwin's own :observer role has the same restrict_central gap
# the ESP32 port had, so this intentionally avoids :observer until that's
# fixed. Skipped without RUN_E2E=1. See ports/darwin/README.md for the
# central E2E procedure this mirrors.

require "picotest"

class ScanOnlyCentral < BLE
  attr_reader :seen

  def initialize(target_name)
    super(:central)
    @target_name = target_name
    @seen = []
  end

  def advertising_report_callback(r)
    return unless @state == :TC_W4_SCAN_RESULT
    name = r.reports[:complete_local_name] || r.reports[:shortened_local_name]
    return unless name && !@target_name.empty? && name.include?(@target_name)
    @seen << [r.rssi, r.reports[:manufacturer_specific_data]]
    STDOUT.puts "[scan-only] saw #{name.inspect} rssi=#{r.rssi} mfg=#{r.reports[:manufacturer_specific_data].inspect}"
  end
end

class BroadcasterScanTest < Picotest::Test
  TARGET_NAME = ENV["TARGET_NAME"] || "PicoRuby"
  SCAN_MS     = (ENV["SCAN_MS"] || "15000").to_i

  # One test method so the scan runs once, not once per assertion.
  def test_broadcaster_visible_and_changing
    skip "RUN_E2E=1 not set" unless ENV["RUN_E2E"] == "1"

    c = ScanOnlyCentral.new(TARGET_NAME)
    c.scan(timeout_ms: SCAN_MS, stop_state: :no_stop, debug: true)

    STDOUT.puts "[scan-only] scan done; #{c.seen.size} matching report(s)"
    assert(c.seen.size >= 2)
    assert(c.seen.map { |(_, mfg)| mfg }.uniq.size >= 2)
  end
end

# Picotest::Runner is CRuby-only (spawns target VM as subprocess); drive
# in-process so this file runs via picoruby directly.
test = BroadcasterScanTest.new
puts "BroadcasterScanTest"
test.list_tests.reverse.each do |m|
  fresh = BroadcasterScanTest.new
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
