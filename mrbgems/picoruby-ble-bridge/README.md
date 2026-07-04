# picoruby-ble-bridge

Thread-safe inbound-BLE bridge for PicoRuby on ESP32. It exists to remove the
single reboot that StackChan hits during BLE audio streaming, **without
modifying the shared `picoruby-ble` fork**.

## The problem it solves

`picoruby-ble`'s ESP32 port calls `BLE_write_data()` from the BTstack run-loop
task on every inbound GATT write. The upstream implementation builds an mruby
`String` and `mrb_ary_push`es it **on that task**, while the application's mruby
VM runs on the main task. Two FreeRTOS tasks touching one non-thread-safe mruby
allocator/GC corrupts the heap; under a sustained write flood (audio streaming)
this reliably reboots with `mrb_realloc → mrb_ary_push → StoreProhibited`.

## How it works

- **`ports/esp32/ble_bridge_port.c`** defines `__wrap_BLE_write_data`. The
  build links with `-Wl,--wrap=BLE_write_data`, so every call site (the fork's
  `att_write_callback`) is redirected here at link time. The wrapper only copies
  the bytes into a lock-guarded C FIFO — it touches no mruby and never calls
  `__real_BLE_write_data` (the buggy path becomes dead code). It also provides
  the FreeRTOS mutex the FIFO uses.
- **`src/ble_bridge_fifo.c`** is the portable, host-tested FIFO.
- **`src/mruby/ble_bridge.c`** exposes `BLEBridge`: `init`, `pop_write(handle)`,
  `reset`, `write_dropped`. The application drains writes via
  `BLEBridge.pop_write` on the main task, where the mruby `String` is finally
  built.

This gem lives in-tree here (branched off `picoruby-ble-esp32-port`, which
stays clean for its own upstream PR) rather than in the StackChan application
repo, since its `ports/esp32/*.c` needs the same ESP-IDF include-path access
that `picoruby-ble` itself gets by living under `picoruby/mrbgems/`. The
R2P2-ESP32 fork's `stackchan-integration` branch (the build branch, not its
own upstream PR branch) wires this gem into `build_config` and adds the port
C + `--wrap` link flags to the `picoruby-esp32` component's `CMakeLists.txt`
directly (no build-time patch/overlay).

Both `BLE_write_data` (the proven reboot driver under sustained write floods)
and `BLE_push_event` (lower-frequency HCI/ATT events, same cross-thread
hazard) are wrapped.

## Usage (application side)

```ruby
BLEBridge.init                       # before BLE.new / advertising
# in the BLE poll loop, instead of pop_write_value(handle):
while (data = BLEBridge.pop_write(@rx_handle))
  consume_rx(data)
end
BLEBridge.reset                      # on disconnect
```

## Host test

```
cc -std=c11 -pthread -Wall -Wextra -O2 \
  test/host_fifo_test.c -o /tmp/ble_bridge_fifo_test && /tmp/ble_bridge_fifo_test
```
