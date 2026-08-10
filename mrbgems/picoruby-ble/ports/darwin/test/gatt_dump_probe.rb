# Dumps every service and characteristic a peripheral exposes, by full 128-bit
# UUID. Written to answer one question: does the peer actually serve the
# characteristic a client says it cannot find, or is the client's view stale?
#
# uuid32 is deliberately not printed. BLE::Utils.uuid128_to_uuid32 byte-swaps
# 16-bit UUIDs (0x181A comes back as 0x1A180000), so it cannot be used to
# recognise anything. The raw 128-bit bytes are what the peer actually sent.
#
# Run through the already-granted bundle so TCC is not re-triggered:
#   TARGET_NAME=StackChan tools/run_device_probe.sh-style open -W -a ...
class GattDump < BLE
  def initialize(role, target_name)
    super(role)
    @target_name = target_name
    @picked = nil
  end

  def advertising_report_callback(r)
    return unless @state == :TC_W4_SCAN_RESULT
    name = r.reports[:complete_local_name] || r.reports[:shortened_local_name]
    if name && name.include?(@target_name)
      return if @picked
      @picked = true
      STDOUT.puts "[gattdump] connecting to #{name.inspect} rssi=#{r.rssi}"
      stop_scan
      @state = :TC_W4_CONNECT
      gap_connect(r.address, r.address_type_code)
    end
  end
end

def hex128(s)
  return "(nil)" if s.nil?
  out = ""
  i = 0
  while i < s.bytesize
    out << sprintf("%02X", s.getbyte(i))
    i += 1
  end
  out
end

target  = ENV["TARGET_NAME"] || "StackChan"
scan_ms = (ENV["SCAN_MS"] || "35000").to_i

c = GattDump.new(:central, target)
c.scan(timeout_ms: scan_ms, stop_state: :TC_IDLE, debug: true)

STDOUT.puts "[gattdump] state=#{c.state} services=#{c.services.size}"
c.services.each do |s|
  STDOUT.puts "  SVC #{hex128(s[:uuid128])} chars=#{s[:characteristics].size}"
  s[:characteristics].each do |ch|
    STDOUT.puts "    CHR #{hex128(ch[:uuid128])} vh=#{ch[:value_handle]} props=#{ch[:properties]}"
  end
end
STDOUT.puts "[gattdump] done"
