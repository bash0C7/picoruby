# Live-radio E2E for the CoreBluetooth central. Needs a peripheral on a 2nd
# device (a Mac central cannot see a same-Mac peripheral). Skipped without
# RUN_E2E=1. See ports/darwin/README.md for the full procedure.

require "picotest"

class E2ECentral < BLE
  attr_reader :picked, :seen, :notified

  # Not exposed by ble_central.rb's own attr_reader list.
  def conn_handle
    @conn_handle
  end

  def initialize(role, target_name)
    super(role)
    @target_name = target_name
    @seen = []
    @picked = nil
    @notified = []
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

  # Find the first characteristic across all discovered services whose
  # property bitmask includes `prop` (BLE::NOTIFY / BLE::WRITE). Matching on
  # properties, not UUID, sidesteps the byte-swap this port applies to 16-bit
  # UUIDs (see esp32_dynamic_read_test.rb's own comment on the same issue).
  def find_by_property(prop)
    services.each do |s|
      s[:characteristics].each do |ch|
        return ch if (ch[:properties] & prop) != 0
      end
    end
    nil
  end

  # Non-blocking drain of one event from the queue, mirroring
  # pc/stackchan-pico/app/ble_client.rb's StackchanRadio#pop_and_dispatch —
  # this test exercises the exact same primitive the production central uses.
  def pop_and_dispatch
    event = @event_queue.pop(timeout_ms: 0)
    return nil unless event
    _event_popped
    packet_callback(event) if event.is_a?(String)
    event
  end

  # The base packet_callback's GATT_EVENT_NOTIFICATION branch is a bare
  # `# TODO` (ble_central.rb:297) -- notifications are otherwise silently
  # dropped. Decode and log every one so a failed wait leaves evidence behind.
  def packet_callback(event_packet)
    super
    return unless event_packet.getbyte(0) == GATT_EVENT_NOTIFICATION
    handle = Utils.little_endian_to_int16(event_packet.byteslice(4, 1))
    len    = Utils.little_endian_to_int16(event_packet.byteslice(6, 1))
    value  = event_packet.byteslice(8, len)
    @notified << [handle, value]
    STDOUT.puts "[notify-e2e] NOTIFY handle=#{handle} value=#{value.inspect}"
  end

  # Cooperative wait for at least one notification, draining via
  # pop_and_dispatch rather than calling scan/start again (which would
  # re-trigger hci_power_control and flush any in-flight packet -- see
  # ble_client.rb's file-level comment for the full rationale).
  def await_notification(ticks, poll_ms)
    i = 0
    while @notified.empty? && i < ticks
      pop_and_dispatch
      sleep_ms(poll_ms)
      i += 1
    end
    @notified.shift
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

  # Counterpart: mrbgems/picoruby-ble/example/esp32/probe/peripheral_paths_probe.rb
  # (bash0C7/picoruby, picoruby-ble-esp32-port branch). That probe's README
  # names "a Central that connects, subscribes, writes" as its required peer;
  # this fills that gap using the exact primitives StackchanCentral (the
  # production Mac-side BLE central) relies on for its write-then-await-ACK
  # path. Scoped to a single connect cycle -- the probe's own disconnect-twice
  # (P6/P7) requirement needs a GAP disconnect this port's central role has no
  # API for at all (verified: no `disconnect` in ble.rbs, ble_central.rb, or
  # PicoBLECentral.swift), so it is out of scope here.
  def test_subscribe_write_receive_notification
    skip "RUN_E2E=1 not set" unless ENV["RUN_E2E"] == "1"

    probe_name = ENV["PROBE_TARGET_NAME"] || "PBLE-PROBE"
    c = E2ECentral.new(:central, probe_name)
    c.scan(timeout_ms: SCAN_MS, stop_state: :TC_IDLE, debug: true)

    STDOUT.puts "[notify-e2e] discovery done; state=#{c.state} services=#{c.services.size}"
    notify_ch = c.find_by_property(BLE::NOTIFY)
    write_ch  = c.find_by_property(BLE::WRITE)
    STDOUT.puts "[notify-e2e] notify_ch=#{notify_ch.inspect}"
    STDOUT.puts "[notify-e2e] write_ch=#{write_ch.inspect}"
    assert(!notify_ch.nil?)
    assert(!write_ch.nil?)
    return unless notify_ch && write_ch

    cccd_handle = notify_ch[:descriptors][0][:handle]
    STDOUT.puts "[notify-e2e] subscribing cccd_handle=#{cccd_handle}"
    c.write_characteristic_descriptor_using_descriptor_handle(c.conn_handle, cccd_handle, "\x01\x00")
    # Settle: give CoreBluetooth's setNotifyValue time to land before the
    # write goes out, matching ble_client.rb's subscribe_tx.
    10.times { c.pop_and_dispatch; sleep_ms(100) }

    payload = "E2E-WRITE-PROBE"
    STDOUT.puts "[notify-e2e] writing #{payload.inspect} to vh=#{write_ch[:value_handle]}"
    c.write_value_of_characteristic_without_response(c.conn_handle, write_ch[:value_handle], payload)

    # 80 x 100ms = 8s -- generous vs. StackchanCentral::ACK_TIMEOUT_TICKS (30
    # x 100ms = 3s) so a slow-but-working path still shows as a PASS here.
    received = c.await_notification(80, 100)
    STDOUT.puts "[notify-e2e] received=#{received.inspect} all_notified=#{c.notified.inspect}"
    assert(!received.nil?)
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
