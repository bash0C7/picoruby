# Reads the ESP32 port's dynamic characteristic and checks the value came from
# push_read_value, not from the profile's static value. Counterpart:
# ports/esp32/test/dynamic_read_probe.rb.
#
# Run with the already-granted bundle so TCC is not re-triggered:
#   RUN_E2E=1 TARGET_NAME=PBLE-RDPROBE \
#     ~/Applications/PicoRubyBLE.app/Contents/MacOS/picoruby-bin \
#     mrbgems/picoruby-ble/ports/darwin/test/esp32_dynamic_read_test.rb
require "picotest"

class RdCentral < BLE
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
    if name && name.include?(@target_name)
      pick(r.address, r.address_type_code, name, r.rssi)
    end
  end

  def pick(addr, atype, name, rssi)
    return if @picked
    @picked = [addr, atype, name, rssi]
    STDOUT.puts "[rdcentral] connecting to #{name.inspect} rssi=#{rssi}"
    stop_scan
    @state = :TC_W4_CONNECT
    gap_connect(addr, atype)
  end
end

class Esp32DynamicReadTest < Picotest::Test
  TARGET_NAME = ENV["TARGET_NAME"] || "PBLE-RDPROBE"
  SCAN_MS     = (ENV["SCAN_MS"] || "20000").to_i
  SERVICE32   = 0x0000181A
  CHAR32      = 0x00002A6E

  def test_dynamic_value_comes_from_push_read_value
    skip "RUN_E2E=1 not set" unless ENV["RUN_E2E"] == "1"

    c = RdCentral.new(:central, TARGET_NAME)
    c.scan(timeout_ms: SCAN_MS, stop_state: :TC_IDLE, debug: true)

    STDOUT.puts "[rdcentral] discovery done; state=#{c.state} services=#{c.services.size}"
    c.services.each do |s|
      STDOUT.puts "  svc uuid32=#{sprintf('0x%08X', s[:uuid32] || 0)}"
      s[:characteristics].each do |ch|
        STDOUT.puts "    char uuid32=#{sprintf('0x%08X', ch[:uuid32] || 0)} vh=#{ch[:value_handle]} value=#{ch[:value].inspect}"
      end
    end

    svc = c.services.find { |s| s[:uuid32] == SERVICE32 }
    assert(!svc.nil?)

    ch = svc[:characteristics].find { |x| x[:uuid32] == CHAR32 }
    assert(!ch.nil?)
    assert(!ch[:value].nil?)

    # "STATIC" means the mirror was never populated and gatt_access_cb fell
    # back to the profile's static value.
    assert(ch[:value] != "STATIC")
    assert(ch[:value].start_with?("RDPROBE-"))
    STDOUT.puts "[rdcentral] READ PASS value=#{ch[:value].inspect}"
  end
end

test = Esp32DynamicReadTest.new
puts "Esp32DynamicReadTest"
test.list_tests.reverse.each do |m|
  fresh = Esp32DynamicReadTest.new
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
