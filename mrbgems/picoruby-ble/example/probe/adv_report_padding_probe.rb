require 'ble'

class AdvProbe < BLE
  GAP_ADV = 0xda

  def initialize
    super(:central)
    @total = 0
    @d0 = 0
    @d1 = 0
    @badpad = 0
    @fail = 0
  end

  def packet_callback(event_packet)
    if event_packet.getbyte(0) == GAP_ADV
      raw  = event_packet.bytesize
      len1 = event_packet.getbyte(1)
      dlen = event_packet.getbyte(11)
      et   = event_packet.getbyte(2)
      @total += 1
      if dlen == 0 || dlen == 1
        @d0 += 1 if dlen == 0
        @d1 += 1 if dlen == 1
        bad = 0
        bad = 1 if raw < 14
        bad = 1 if len1 != raw - 2
        @badpad += bad
        puts "[probe] SHORT dlen=#{dlen} raw=#{raw} len1=#{len1} et=#{et} bad=#{bad}"
      end
    end
    begin
      super
    rescue => e
      @fail += 1
      puts "[probe] PARSE_FAIL #{e.message}"
    end
  end

  def advertising_report_callback(adv_report)
  end

  def heartbeat_callback
    puts "[probe] SUMMARY total=#{@total} dlen0=#{@d0} dlen1=#{@d1} badpad=#{@badpad} fail=#{@fail}"
  end
end

prefix = "\xda\x0a\x04\x00"
addr   = "\x11\x22\x33\x44\x55\x66"
tail   = "\xc0\x00"
unpadded = prefix + addr + tail
begin
  BLE::AdvertisingReport.new(unpadded)
  puts "[probe] NEGCTL bytes=#{unpadded.bytesize} raised=no  <-- probe is BLIND"
rescue => e
  puts "[probe] NEGCTL bytes=#{unpadded.bytesize} raised=yes msg=#{e.message}"
end

probe = AdvProbe.new
probe.scan(scan_type: :active, stop_state: :no_stop)
