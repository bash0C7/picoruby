# picoruby-ble — Darwin (CoreBluetooth) port

The Darwin port drives CoreBluetooth (a high-level GATT API) and synthesizes the
BTstack-format event byte strings that `mrblib/ble_central.rb` decodes. The
decoder branches on `event_packet.getbyte(0)`; this port produces exactly those
bytes. The source of truth for behavior is the code under
`mrbgems/picoruby-ble/`; this document describes the contract that code
implements.

## Scope

Central, observer, and peripheral. A central/observer drives `CBCentralManager`
and synthesizes the GATT-client events `ble_central.rb` decodes; a peripheral
drives `CBPeripheralManager` and serves a GATT server built from the same
`profile_data` blob the rp2040 port consumes (subclass `BLE` with role
`:peripheral`). The broadcaster backend is a no-op stub. The central path does
not synthesize 0xA3 included-service, 0xA6 long-value, or 0xA7/0xA8
notification/indication events, because `ble_central.rb` has no decode body for
them.

### Peripheral role

The peripheral backend (`PicoBLEPeripheral.swift`) parses the BTstack ATT-DB blob
`mrblib/ble_gatt_database.rb` produces, rebuilds it as CoreBluetooth
services/characteristics, and serves reads, writes, subscriptions, and
notifications driven from Ruby. The GAP (0x1800) and GATT (0x1801) services are
skipped — CoreBluetooth owns them. The CCCD (0x2902) is mapped to
`CBPeripheralManager`'s subscribe/unsubscribe callbacks, so the canonical Ruby
path (`pop_write_value(cccd_handle)` returning `"\x01\x00"`/`"\x00\x00"`) holds.

Characteristic values live in the generic mruby read/write tables (`BLE_read_data`
/ `BLE_write_data`), exactly as on rp2040. Because every CoreBluetooth delegate
callback runs on the `pble.cb.peri` queue — not the VM thread — the backend never
calls those tables from a callback: a read is answered from a Swift-side cache, a
write or subscribe is queued, and both the cache refresh and the queued writes are
applied on the VM thread by `pump()` (driven from `pble_drain_one` each poll
tick). This keeps the "mruby only on the VM thread" invariant the central path
relies on. These four peripheral events reach the Ruby `packet_callback`:

| event | code | when |
|---|---|---|
| BTSTACK_EVENT_STATE (power-on) | 0x60 | `[0]=0x60, [2]=0x02`. Emitted once all services are added; Ruby advertises in response. |
| HCI_EVENT_DISCONNECTION_COMPLETE | 0x05 | `[0]=0x05`. A central unsubscribes / drops. |
| ATT_EVENT_MTU_EXCHANGE_COMPLETE | 0xB5 | `[0]=0xB5`. First subscription from a central. |
| ATT_EVENT_CAN_SEND_NOW | 0xB7 | `[0]=0xB7`. In response to `request_can_send_now_event`. |

## Decoder ABI

The decoder reads each packet at the offsets given below; the port emits
exactly those layouts. BTstack 1.6.2 itself serializes GATT events with a
service_id/connection_id field inserted 4 bytes before the struct base, so
forwarding real BTstack bytes would not decode — the port produces the bytes
the decoder reads, not the bytes BTstack emits.

## Event byte layouts (9 events)

Handles are read with `byteslice(N,1)` (low byte), so GATT handles must be ≤ 255.
The only exception is the connection handle (`byteslice(4,2)`, 16-bit). Value and
descriptor lengths are also one byte (≤ 255). UUIDs are emitted as 128-bit,
LSB-first; the decoder's `uuid128_to_uuid32` does not recover the 16-bit alias
(0x180D → 0x0D180000), so comparisons use the full 128-bit UUID.

| event | code | byte layout |
|---|---|---|
| BTSTACK_EVENT_STATE (power-on) | 0x60 | `[0]=0x60, [2]=0x02 (HCI_STATE_WORKING)`. Emitted once after `centralManagerDidUpdateState == .poweredOn`. |
| GAP_EVENT_ADVERTISING_REPORT | 0xda | ≥ 14 bytes: `[0]=0xda, [2]=adv subcode, [3]=addr_type (0x01 random), [4..9]=6-byte synthetic BD_ADDR (LSB-first, each byte non-zero), [10]=(rssi_dBm+256)&0xff, [11]=AD-data len, [12..]=AD TLV ([len][type][value])`. Includes a complete-local-name (0x09) TLV so `name_include?` works. |
| LE_CONNECTION_COMPLETE | 0x3E (sub 0x01) | ≥ 6 bytes: `[0]=0x3E, [2]=0x01, [4..5]=conn_handle little-endian 16-bit` (e.g. 0x0040 → `40 00`). |
| LE disconnection | 0x3E (sub 0x05) | `[0]=0x3E, [2]=0x05`. Omitted on the read-only happy path. |
| GATT_EVENT_SERVICE_QUERY_RESULT | 0xA1 | ≥ 24 bytes: `[0]=0xA1, [4]=start_handle low, [6]=end_handle low, [8..23]=uuid128 LSB-first`. One per CBService, then a terminating 0xA0. end_handle is the real DFS subtree end. |
| GATT_EVENT_CHARACTERISTIC_QUERY_RESULT | 0xA2 | ≥ 28 bytes: `[0]=0xA2, [4]=start low, [6]=value_handle low, [8]=end low, [10]=properties low, [12..27]=uuid128 LSB-first`. One per characteristic, then a 0xA0. Properties map from CBCharacteristicProperties (READ=0x02, WRITE=0x08, WRITE_WO_RESP=0x04, NOTIFY=0x10, INDICATE=0x20). |
| GATT_EVENT_ALL_CHARACTERISTIC_DESCRIPTORS_QUERY_RESULT | 0xA4 | ≥ 22 bytes: `[0]=0xA4, [4]=descriptor handle low, [6..21]=uuid128 LSB-first`. UUID is at offset 6. One per descriptor, then a 0xA0. |
| GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT | 0xA5 | `8+len` bytes: `[0]=0xA5, [4]=value_handle low, [6]=value len (≤255), [8..]=value`. Shared by characteristic value and descriptor value. |
| GATT_EVENT_QUERY_COMPLETE | 0xA0 | 2 bytes: `[0]=0xA0`. Exactly one after each phase batch; the decoder advances its FSM and shifts its worklist on it. A missing 0xA0 stalls the FSM (the decode loop has no timeout). |

## Threading

Two threads, with all mruby access confined to the VM thread.

- **CoreBluetooth serial queue** (`DispatchQueue(label: "pble.cb")`): every
  `CBCentralManagerDelegate` / `CBPeripheralDelegate` callback runs here. It
  does three things only: update the handle/address registry, build the
  BTstack-format `[UInt8]` packet, and push it onto a thread-safe FIFO. It
  never calls `mrb_*`, `BLE_push_event`, or `BLE_heartbeat`. mruby state is
  unreachable from this queue.
- **The FIFO** is `PicoBLEFifo` (Swift, NSLock). It holds raw byte packets
  only. It buffers the burst of results from a discovery phase, which the
  single-slot mailbox in shared `src/mruby/ble.c` would otherwise overwrite.
  An oversize packet (larger than the drain buffer) is dropped and logged so
  it cannot wedge the FIFO head and stall every later packet.
- **The VM thread** drains one packet per poll tick. The shared decoder's
  `mrb_pop_packet`, under `#ifdef PICORB_PLATFORM_DARWIN`, calls
  `pble_drain_one` to copy one packet out and feeds it to `BLE_push_event`.
  This is the only place `BLE_push_event` runs.
- `mrblib/ble.rb` carries no Darwin-specific code and stays
  architecture-neutral. The only shared-code touch is the
  `#ifdef PICORB_PLATFORM_DARWIN` drain hook in `src/mruby/ble.c`, following
  the platform `#ifdef` convention used by other gems.
- Commands issued from the VM thread (connect, scan, discover, read)
  dispatch the actual CoreBluetooth calls onto `pble.cb` so all
  CoreBluetooth interaction stays on that one queue.

This invariant is enforced by convention, not by code: nothing asserts or
panics if a future change calls `BLE_read_data` / `BLE_write_data` /
`mrb_*` from `pble.cb` or `pble.cb.peri`. The ThreadSanitizer run under
Tests is the only check that would catch a violation, and only if the
offending path is actually exercised.

## Synthetic handle registry

CoreBluetooth objects live on the Swift side, so the registry does too
(lock-guarded).

- **Connection handle**: uint16 counter from 0x0040, incremented per
  connection.
- **GATT handles**: a single monotonic uint8 cursor (from 1), assigned by
  pre-order DFS, nesting discovery strictly — a service's characteristics
  and their descriptors are numbered before the next service's
  characteristics are discovered. A service's end handle is its DFS subtree
  end; a characteristic's end handle is its real value (the descriptor upper
  bound). This makes the decoder's containment checks hold by construction.
- **Caps**: entities past handle 255 are dropped and logged; values longer
  than 255 bytes are truncated.
- **Address**: CoreBluetooth exposes `peripheral.identifier` (a UUID), not a
  BD_ADDR. The port hashes it to six bytes and ORs each byte with 0x01,
  because `BLE_central_gap_connect` reads the address with `mrb_get_args 'z'`
  (NUL terminated) and an interior 0x00 would truncate it. Both the wire
  order and the reversed order are mapped back to the CBPeripheral.

## Port files

| file | responsibility |
|---|---|
| `ext/Sources/PicoBLEDarwin/PicoBLECentral.swift` | CBCentralManager + delegates on `pble.cb`; owns the registry; builds packets per the layouts above and pushes them to the FIFO; `@c` exports for init/power/scan/connect/discover/read. |
| `ext/Sources/PicoBLEDarwin/PicoBLEPeripheral.swift` | CBPeripheralManager + delegates on `pble.cb.peri`; parses the ATT-DB blob into CB services/characteristics; answers reads from a cache and queues writes; `pump()` (VM thread) flushes writes and refreshes the cache via the generic tables. |
| `ext/Sources/PicoBLEDarwin/PicoBLEFifo.swift` | Thread-safe byte-packet FIFO; `drainInto` copies one packet to the VM thread and drops oversize packets. |
| `ext/Sources/CBLEBridge/` | Declarations-only C module exposing `BLE_read_data` / `BLE_write_data` to Swift; bound at load time via `-undefined dynamic_lookup` (contributes no symbols). |
| `ble.c`, `ble_central.c` | port ABI (`BLE_*`, `BLE_central_*`) delegating to the Swift `@c` exports; `BLE_init` routes by role; `pble_drain_one` bridges the FIFO to the C side. |
| `ble_peripheral.c` | peripheral ABI (`BLE_peripheral_*`) delegating to the Swift peripheral backend; `notify` reads the value via the generic table and hands the bytes to Swift. |
| `ble_common.h` | shared port include. |
| `../../src/mruby/ble.c` (shared) | the `#ifdef PICORB_PLATFORM_DARWIN` drain hook in `mrb_pop_packet`. |
| `../../mrbgem.rake` | on `build.darwin?`, defines `PICORB_PLATFORM_DARWIN`, builds the Swift backend (`PicoBLEDarwin`) and links the dylib, and compiles `ports/darwin/*.c`. |

The C export from Swift uses `@c` (SE-0495, swift-tools-version 6.3);
`@_cdecl` emits the C symbol but not a declaration in the generated
`-Swift.h`.

## Build

The port self-compiles when `picoruby-ble` is included on a Darwin host:
`mrbgem.rake` runs `swift build` to produce the PicoBLEDarwin dylib,
compiles `ports/darwin/*.c`, adds the linker flags, and defines
`PICORB_PLATFORM_DARWIN` on `build.darwin?` so the shared C code's
`#ifdef PICORB_PLATFORM_DARWIN` drain hook activates. The only build
config addition needed is the gem:

```ruby
conf.gem core: "picoruby-ble"
```

Add `conf.gem core: "picoruby-picotest"` too if you want to run the tests
in this directory.

Host requirements: Xcode Command Line Tools (clang + Swift 6.3+), Homebrew
`openssl@3` (the networking gembox links ssl/crypto). Bluetooth must be on
and the terminal (or app launching the binary) must hold Bluetooth
permission under *System Settings → Privacy & Security → Bluetooth*. Build
with the brew paths so the linker finds OpenSSL:

```
export LDFLAGS="-L$(brew --prefix openssl@3)/lib"
export CFLAGS="-I$(brew --prefix openssl@3)/include"
MRUBY_CONFIG=path/to/your_config.rb rake
```

## Tests

Both tests use `picoruby-picotest` (`Picotest::Test` + `assert_*`) and run
directly on the `picoruby` binary.

### Decoder contract (no radio, runnable in CI)

Drives synthesized byte vectors through the real decoder and checks that the
GATT tree is reconstructed end-to-end.

```
build/host/bin/picoruby \
  mrbgems/picoruby-ble/ports/darwin/test/decoder_contract_test.rb
```

### E2E central (live radio, requires a 2nd device)

Connects to a real peripheral, discovers services, and reads at least one
characteristic value. Skipped unless `RUN_E2E=1` because a Mac acting as
central cannot receive advertisements from a peripheral on the same Mac.

Pick a peripheral on a second device:

- **Swift fixture on a 2nd Mac (deterministic).**
  `test/test_peripheral.swift` exposes Device Information (0x180A) with a
  readable Manufacturer Name (0x2A29) and a User Description descriptor
  (0x2901), advertising the local name `PBLE-TEST`.
  ```
  swift mrbgems/picoruby-ble/ports/darwin/test/test_peripheral.swift
  # [peripheral] advertising 'PBLE-TEST': service 180A / char 2A29 (read) / desc 2901
  ```
- **nRF Connect on iOS.** Configure a GATT server with at least one Read
  characteristic, set the advertiser's *Complete Local Name* to `PBLE-TEST`
  and *Connectable* on, keep nRF Connect in the foreground (backgrounding
  drops the local name).

Then run the test:

```
RUN_E2E=1 build/host/bin/picoruby \
  mrbgems/picoruby-ble/ports/darwin/test/e2e_central_test.rb
```

The driver connects to a device whose name includes `TARGET_NAME` (default
`PBLE-TEST`), or to the strongest-RSSI device otherwise. Set `TARGET_NAME=""`
to always pick the strongest. PASS = ≥ 1 service discovered + ≥ 1
characteristic value read.

### ThreadSanitizer (optional)

To check the CoreBluetooth-queue / VM-thread boundary for data races,
instrument both sides: add `-fsanitize=thread` to the build config's `cc`
and linker flags, add `--sanitize=thread` to picoruby-ble's Swift build,
clear `ports/darwin/ext/.build`, then rebuild. Run `e2e_central_test.rb`
under `TSAN_OPTIONS="halt_on_error=0"`; connect → discover → read must
actually run for the boundary to be exercised.

## Troubleshooting

- **Many adv reports but the target peripheral is never found.** It is on
  the same Mac (loopback is not allowed). Use a separate device.
- **No adv reports at all.** Check Bluetooth permission for the terminal/app
  under *System Settings → Privacy & Security → Bluetooth*.
- **Discovers but stalls after connect.** Confirm the peripheral advertises
  connectable and the characteristic is readable. `e2e_central_test.rb`
  passes `debug: true` to print FSM transitions.
- **Empty descriptor value.** Some descriptors carry no static value. A
  readable characteristic value reaching `:TC_IDLE` means the GATT path
  works.
