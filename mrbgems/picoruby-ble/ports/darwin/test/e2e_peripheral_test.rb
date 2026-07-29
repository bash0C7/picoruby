# Darwin-side peripheral that the ESP32 central writes to and subscribes from.
# Counterpart: ports/esp32/test/central_paths_probe.rb
#
#   C1 the ESP32 writes a characteristic  -> pop_write_value sees it
#   C2 the ESP32 writes the CCCD          -> pop_write_value sees "\x01\x00"
#   C3 this peripheral notifies           -> the ESP32 probe sees raw 0xA7
#
# C2 works because CBPeripheralManager reports a CCCD write as didSubscribeTo
# rather than as a plain ATT write; PicoBLEPeripheral.swift:335-340 turns that
# back into a pending write of "\x01\x00" against the CCCD's handle, which is
# what pop_write_value below reads.
#
# Run under PicoRubyBLE.app (CoreBluetooth needs the bundle's Info.plist):
#   open -W -a ~/Applications/PicoRubyBLE.app --stdout /tmp/dp.log \
#     --stderr /tmp/dp.log --args <path to this file>

class DarwinProbePeripheral < BLE
  ADV_FLAGS     = 0x06
  AD_TYPE_FLAGS = 0x01
  AD_TYPE_NAME  = 0x09

  EVT_DISCONNECT   = 0x05
  EVT_STATE        = 0x60
  EVT_CAN_SEND_NOW = 0xB7

  SERVICE     = 0x181A
  CHAR_NOTIFY = 0x2A6E
  CHAR_WRITE  = 0x2A9F

  def initialize
    @adv_data = AdvertisingData.build do |a|
      a.add(AD_TYPE_FLAGS, ADV_FLAGS)
      a.add(AD_TYPE_NAME, "PBLE-DARWIN")
    end
    db = GattDatabase.new do |d|
      d.add_service(GATT_PRIMARY_SERVICE_UUID, GAP_SERVICE_UUID) do |s|
        s.add_characteristic(READ, GAP_DEVICE_NAME_UUID, READ, "darwin_probe")
      end
      d.add_service(GATT_PRIMARY_SERVICE_UUID, SERVICE) do |s|
        s.add_characteristic(READ|NOTIFY|DYNAMIC, CHAR_NOTIFY, READ|DYNAMIC, "") do |c|
          c.add_descriptor(READ|WRITE|WRITE_WITHOUT_RESPONSE|DYNAMIC,
                           CLIENT_CHARACTERISTIC_CONFIGURATION, "\x00\x00")
        end
        s.add_characteristic(READ|WRITE|WRITE_WITHOUT_RESPONSE|DYNAMIC,
                             CHAR_WRITE, READ|WRITE|DYNAMIC, "")
      end
    end
    @notify_handle = db.handle_table[SERVICE][CHAR_NOTIFY][:value_handle]
    @cccd_handle   = db.handle_table[SERVICE][CHAR_NOTIFY][CLIENT_CHARACTERISTIC_CONFIGURATION]
    @write_handle  = db.handle_table[SERVICE][CHAR_WRITE][:value_handle]
    super(:peripheral, db.profile_data)
    @counter = 0
    @notify_on = false
    @saw_write = false
    puts "[darwin-peri] handles notify=#{@notify_handle} cccd=#{@cccd_handle} write=#{@write_handle}"
  end

  # Overridden without super on purpose: the base packet_callback in
  # ble_central.rb calls restrict_central_or_observer and would raise here.
  def packet_callback(event_packet)
    case event_packet.getbyte(0)
    when EVT_STATE
      return unless event_packet.getbyte(2) == HCI_STATE_WORKING
      puts "[darwin-peri] up, advertising as PBLE-DARWIN"
      advertise(@adv_data)
    when EVT_CAN_SEND_NOW
      notify @notify_handle
      puts "[darwin-peri] C3 notify sent counter=#{@counter}"
    when EVT_DISCONNECT
      puts "[darwin-peri] disconnected"
      @notify_on = false
      advertise(@adv_data)
    end
    # poll runs here as well as from heartbeat_callback because the darwin
    # port's heartbeat never fires for a peripheral -- measured: a tick print
    # every 30 ticks produced nothing in 18s, so pop_write_value would never be
    # called and this side could never witness anything. packet_callback does
    # fire (the "up" line above proves it), and PicoBLEPeripheral.swift:336
    # pushes an MTU event on didSubscribeTo, so a subscribe always gives us a
    # tick to drain both pending writes on.
    poll
  end

  def poll
    @counter += 1
    push_read_value(@notify_handle, Utils.int16_to_little_endian(@counter))

    if (v = pop_write_value(@cccd_handle))
      @notify_on = (v == "\x01\x00")
      puts "[darwin-peri] C2 CCCD written value=#{v.inspect} notify_on=#{@notify_on}"
    end
    if (v = pop_write_value(@write_handle))
      @saw_write = true
      puts "[darwin-peri] C1 characteristic write received value=#{v.inspect}"
    end
    request_can_send_now_event if @notify_on
  end

  def heartbeat_callback
    poll
  end
end

peri = DarwinProbePeripheral.new
peri.debug = true
peri.start
