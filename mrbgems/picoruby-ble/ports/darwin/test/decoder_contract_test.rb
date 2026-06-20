# Contract test: the BTstack-format byte layouts the Darwin port synthesizes are
# decoded by the real ble_central.rb into the correct @services tree, reaching
# :TC_IDLE. Runs on the host picoruby binary; no radio. The byte vectors below
# are the exact ones the Swift builders emit, so this closes the loop
# Swift-bytes -> decoder.
#
#   build/host/bin/picoruby \
#     mrbgems/picoruby-ble/ports/darwin/test/decoder_contract_test.rb

require "picotest"

# A central whose port ABI calls are no-ops so packet_callback advances purely
# on the packets we feed; helper sets the FSM into the service-discovery phase.
class TBLE < BLE
  def gap_local_bd_addr; "\x00\x00\x00\x00\x00\x00"; end
  def start_scan; end
  def stop_scan; end
  def discover_primary_services(_c); 0; end
  def discover_characteristics_for_service(_c, _s, _e); 0; end
  def read_value_of_characteristic_using_value_handle(_c, _v); 0; end
  def discover_characteristic_descriptors(_c, _v, _e); 0; end
  def goto_service_phase; @conn_handle = 0x40; @state = :TC_W4_SERVICE_RESULT; end
end

class DecoderContractTest < Picotest::Test
  # 16-bit UUID -> 16-byte wire (LSB-first), same as Swift
  # pbleUuidWire(fromCBUUIDData:).
  def wire16(x)
    [0xFB,0x34,0x9B,0x5F,0x80,0x00,0x00,0x80,0x00,0x10,0x00,0x00,
     x & 0xff, (x>>8)&0xff, 0x00, 0x00].pack("C*")
  end
  def pkt(bytes); bytes.pack("C*"); end

  def setup
    # GATT tree (pre-order DFS handles): service[1..6] uuid 0x180D >
    #   char A: start=2 value=3 end=4 props=READ uuid 0x2A37, value "hr!",
    #           descriptor handle=4 uuid 0x2902 value "cc";
    #   char B: start=5 value=6 end=6 props=READ uuid 0x2A38, value "xy".
    # The port batches reads of every readable value handle, emitting a single
    # 0xA0 only after the LAST handle (max), so the decoder files every
    # characteristic's value instead of dropping the 2nd+ when an early 0xA0
    # ends the phase.
    svc_uuid   = wire16(0x180D)
    charA_uuid = wire16(0x2A37)
    charB_uuid = wire16(0x2A38)
    desc_uuid  = wire16(0x2902)

    @b = TBLE.new(:central)
    @b.goto_service_phase
    [
      pkt([0xA1,0x01,0,0, 1,0, 6,0] + svc_uuid.bytes),                    # service result (1..6)
      pkt([0xA0,0x01]),                                                   # service query complete -> discover chars
      pkt([0xA2,0x01,0,0, 2,0, 3,0, 4,0, 0x02,0] + charA_uuid.bytes),     # char A result
      pkt([0xA2,0x01,0,0, 5,0, 6,0, 6,0, 0x02,0] + charB_uuid.bytes),     # char B result
      pkt([0xA0,0x01]),                                                   # char query complete -> read value 3
      pkt([0xA5,0x01,0,0, 3,0, 3,0, 0x68,0x72,0x21]),                     # batched value handle 3 = "hr!" -> read 6
      pkt([0xA5,0x01,0,0, 6,0, 2,0, 0x78,0x79]),                          # batched value handle 6 = "xy" (last)
      pkt([0xA0,0x01]),                                                   # batched value complete -> discover descriptors (char A)
      pkt([0xA4,0x01,0,0, 4,0] + desc_uuid.bytes),                        # descriptor result handle 4
      pkt([0xA0,0x01]),                                                   # descriptor discovery complete -> read descriptor 4
      pkt([0xA5,0x01,0,0, 4,0, 2,0, 0x63,0x63]),                          # batched descriptor value handle 4 = "cc" (last)
      pkt([0xA0,0x01]),                                                   # batched descriptor-value complete -> TC_IDLE
    ].each { |p| @b.packet_callback(p) }
  end

  def test_reaches_idle
    assert_equal(:TC_IDLE, @b.state)
  end

  def test_one_service
    assert_equal(1, @b.services.size)
  end

  def test_service_start_handle
    assert_equal(1, @b.services[0][:start_handle])
  end

  def test_service_uuid128_canonical
    assert_equal(
      [0x00,0x00,0x18,0x0D,0x00,0x00,0x10,0x00,0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB],
      @b.services[0][:uuid128].bytes
    )
  end

  def test_service_uuid32_base_uuid_quirk
    # uuid128_to_uuid32 does not recover the 16-bit alias (0x180D -> 0x0D180000);
    # the 16-bit value lands in the high bytes of uuid32.
    assert_equal(0x0D180000, @b.services[0][:uuid32])
  end

  def test_two_characteristics
    assert_equal(2, @b.services[0][:characteristics].size)
  end

  def test_char_a_value_handle
    assert_equal(3, @b.services[0][:characteristics][0][:value_handle])
  end

  def test_char_a_properties_read
    assert_equal(0x02, @b.services[0][:characteristics][0][:properties])
  end

  def test_char_a_value_decoded
    assert_equal("hr!", @b.services[0][:characteristics][0][:value])
  end

  def test_char_a_one_descriptor
    assert_equal(1, @b.services[0][:characteristics][0][:descriptors].size)
  end

  def test_char_b_value_handle
    assert_equal(6, @b.services[0][:characteristics][1][:value_handle])
  end

  def test_char_b_value_decoded
    assert_equal("xy", @b.services[0][:characteristics][1][:value])
  end

  def test_char_a_descriptor_handle
    assert_equal(4, @b.services[0][:characteristics][0][:descriptors][0][:handle])
  end

  def test_char_a_descriptor_uuid32_cccd_quirk
    assert_equal(0x02290000, @b.services[0][:characteristics][0][:descriptors][0][:uuid32])
  end

  def test_char_a_descriptor_value_decoded
    assert_equal("cc", @b.services[0][:characteristics][0][:descriptors][0][:value])
  end
end

# Self-driver: Picotest::Runner is CRuby-only (it spawns the target VM as a
# subprocess), so when this file is executed by picoruby directly we drive
# setup -> each test_* method -> teardown in-process and collect results from
# Picotest::Test#result.
test = DecoderContractTest.new
puts "DecoderContractTest"
test.list_tests.reverse.each do |m|
  fresh = DecoderContractTest.new
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
  puts
end

r = test.result
puts ""
puts "success: #{r["success_count"]}, failure: #{r["failures"].size}, exception: #{r["exceptions"].size}"
r["failures"].each do |f|
  puts "  FAIL #{f[:method] || f["method"]}: #{f[:error_message] || f["error_message"]}"
  puts "    expected: #{(f[:expected] || f["expected"]).inspect}"
  puts "    actual:   #{(f[:actual]   || f["actual"]).inspect}"
end
r["exceptions"].each do |e|
  puts "  ERR  #{e[:method] || e["method"]}: #{e[:raise_message] || e["raise_message"]}"
end
exit(r["failures"].empty? && r["exceptions"].empty? ? 0 : 1)
