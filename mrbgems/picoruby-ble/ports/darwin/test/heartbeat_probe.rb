# Counts how many times BLE#heartbeat_callback fires during a fixed run.
#
# mrblib/ble.rb's start loop calls heartbeat_callback when @event_queue pops a
# :heartbeat symbol, which only happens when a port has called BLE_heartbeat().
# rp2040 drives that from a btstack timer and the esp32 port from an esp_timer;
# this probe measures whether the darwin port drives it at all. Expected on a
# port that does: RUN_MS / HEARTBEAT_PERIOD_MS ticks.
#
# Run through an already-granted bundle so TCC is not re-triggered:
#   open -W -a ~/Applications/<App>.app --stdout LOG --stderr LOG \
#     --env RUN_MS=15000 --args <absolute path to this file>
class HeartbeatProbe < BLE
  attr_reader :ticks

  def initialize(role)
    super(role)
    @ticks = 0
  end

  def heartbeat_callback
    @ticks += 1
    STDOUT.puts "[hbprobe] tick #{@ticks}"
  end
end

run_ms = (ENV["RUN_MS"] || "15000").to_i
role   = (ENV["ROLE"] || "central").to_sym

p = HeartbeatProbe.new(role)
STDOUT.puts "[hbprobe] role=#{role} run_ms=#{run_ms} polling_unit_ms=#{BLE::POLLING_UNIT_MS}"
p.start(run_ms)
STDOUT.puts "[hbprobe] ticks=#{p.ticks} in #{run_ms}ms"
STDOUT.puts(p.ticks > 0 ? "[hbprobe] HEARTBEAT ALIVE" : "[hbprobe] HEARTBEAT DEAD — no port called BLE_heartbeat")
