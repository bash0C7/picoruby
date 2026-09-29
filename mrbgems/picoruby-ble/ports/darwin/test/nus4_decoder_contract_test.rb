# Contract test: a NUS-shaped GATT tree — 2 services, the second with FOUR
# characteristics and TWO notify CCCDs — decodes into the right @services
# tree and reaches :TC_IDLE. This is the exact shape that regressed on a
# StackChan robot (only 2 of 4 characteristics visible), so it pins the
# multi-characteristic / multi-CCCD path the smaller decoder_contract_test
# does not. Packet vectors contributed by the stackchan-picoruby session
# (2026-09-29), ported to the BTstack 1.6+ layout the Swift builders emit.
#
#   build/host/bin/picoruby \
#     mrbgems/picoruby-ble/ports/darwin/test/nus4_decoder_contract_test.rb

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

def wire16(x)
  [0xFB,0x34,0x9B,0x5F,0x80,0x00,0x00,0x80,0x00,0x10,0x00,0x00, x & 0xff, (x >> 8) & 0xff, 0x00, 0x00]
end

# NUS 6e4000xx-b5a3-f393-e0a9-e50e24dcca9e, canonical -> wire (LSB-first)
def nus_wire(lo)
  [0x6e,0x40,0x00,lo,0xb5,0xa3,0xf3,0x93,0xe0,0xa9,0xe5,0x0e,0x24,0xdc,0xca,0x9e].reverse
end

# BTstack 1.6+ GATT event: 8-byte header, payload at GATT_EVENT_PAYLOAD_OFFSET
def gatt(type, payload)
  ([type, 6 + payload.size, 0,0, 0,0,0,0] + payload).pack("C*")
end

NAME = "StackChan-PicoRuby".bytes

b = TBLE.new(:central)
b.goto_service_phase
# Every read ends with its own 0xA0: the BTstack 1.6+ decoder issues the
# next read from the QUERY_COMPLETE branch (one 0xA5 + one 0xA0 per handle).
[
  gatt(0xA1, [1,0, 3,0] + wire16(0x1800)),
  gatt(0xA1, [4,0, 14,0] + nus_wire(0x01)),
  gatt(0xA0, [0]),                                   # -> discover chars of svc 1
  gatt(0xA2, [2,0, 3,0, 3,0, 0x02,0] + wire16(0x2A00)),
  gatt(0xA0, [0]),                                   # -> discover chars of svc 2 (NUS)
  gatt(0xA2, [5,0, 6,0, 6,0, 0x0C,0] + nus_wire(0x02)),
  gatt(0xA2, [7,0, 8,0, 9,0, 0x12,0] + nus_wire(0x03)),
  gatt(0xA2, [10,0, 11,0, 11,0, 0x0C,0] + nus_wire(0x04)),
  gatt(0xA2, [12,0, 13,0, 14,0, 0x12,0] + nus_wire(0x05)),
  gatt(0xA0, [0]),                                   # -> read value 3
  gatt(0xA5, [3,0, NAME.size,0] + NAME),
  gatt(0xA0, [0]),                                   # -> read value 6
  gatt(0xA5, [6,0, 0,0]),
  gatt(0xA0, [0]),                                   # -> read value 8
  gatt(0xA5, [8,0, 0,0]),
  gatt(0xA0, [0]),                                   # -> read value 11
  gatt(0xA5, [11,0, 0,0]),
  gatt(0xA0, [0]),                                   # -> read value 13
  gatt(0xA5, [13,0, 0,0]),
  gatt(0xA0, [0]),                                   # -> discover descriptors (8..9]
  gatt(0xA4, [9,0] + wire16(0x2902)),
  gatt(0xA0, [0]),                                   # -> discover descriptors (13..14]
  gatt(0xA4, [14,0] + wire16(0x2902)),
  gatt(0xA0, [0]),                                   # -> read descriptor 9
  gatt(0xA5, [9,0, 2,0, 0,0]),
  gatt(0xA0, [0]),                                   # -> read descriptor 14
  gatt(0xA5, [14,0, 2,0, 0,0]),
  gatt(0xA0, [0]),                                   # -> TC_IDLE
].each { |p| b.packet_callback(p) }

# canonical prefix 6e 40 00 xx identifies each NUS characteristic
def find_nus_char(services, lo)
  services.each do |s|
    s[:characteristics].each do |c|
      u = c[:uuid128]
      next unless u && u.bytesize == 16
      if u.getbyte(0) == 0x6e && u.getbyte(1) == 0x40 && u.getbyte(2) == 0x00 && u.getbyte(3) == lo
        return c
      end
    end
  end
  nil
end

def cccd_handle(chara)
  return nil unless chara
  chara[:descriptors]&.each do |d|
    u = d[:uuid128]
    return d[:handle] if u && u.byteslice(0, 4) == "\x00\x00\x29\x02"
  end
  nil
end

checks = {
  "reaches TC_IDLE" => b.state == :TC_IDLE,
  "two services" => b.services.size == 2,
  "NUS has four characteristics" => b.services[1] && b.services[1][:characteristics].size == 4,
  "finds RX 6e400002" => !find_nus_char(b.services, 0x02).nil?,
  "finds TX 6e400003" => !find_nus_char(b.services, 0x03).nil?,
  "finds dRuby RX 6e400004" => !find_nus_char(b.services, 0x04).nil?,
  "finds dRuby TX 6e400005" => !find_nus_char(b.services, 0x05).nil?,
  "TX CCCD at 9" => cccd_handle(find_nus_char(b.services, 0x03)) == 9,
  "dRuby TX CCCD at 14" => cccd_handle(find_nus_char(b.services, 0x05)) == 14,
}
failed = checks.reject { |_, ok| ok }
checks.each { |name, ok| puts "#{ok ? 'ok  ' : 'FAIL'} #{name}" }
puts "services: #{b.services.map { |s| [s[:start_handle], s[:end_handle], s[:characteristics].map { |c| [c[:start_handle], c[:value_handle], c[:end_handle]] }] }.inspect}"
exit(failed.empty? ? 0 : 1)
