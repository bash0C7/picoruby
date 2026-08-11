# port-darwin Rebase onto upstream/master — Design

**Goal:** Rebase `bash0C7/picoruby`'s `port-darwin` branch onto `picoruby/picoruby` master, so the Darwin BLE test driver builds against current upstream and keeps working as the real-hardware verification tool for `picoruby-ble-esp32-port`.

**Architecture:** Same method already used for `picoruby-ble-esp32-port`: `git rebase upstream/master` in an isolated worktree, resolve conflicts commit-by-commit. Upstream replaced the old poll-flag BLE event delivery (`packet_flag`/`heatbeat_flag`/`mrb_pop_packet`/`mrb_pop_heartbeat`) with a `Task::Queue`-based `event_queue` (`BLE_push_event`/`BLE_heartbeat`/`mrb_event_popped`). `port-darwin`'s `#ifdef PICORB_PLATFORM_DARWIN` hook in `src/mruby/ble.c` (CoreBluetooth FIFO drain via `pble_drain_one`) lived inside the now-removed `mrb_pop_packet`; it moves into `mrb_event_popped`, mirroring the fix already applied on `picoruby-ble-esp32-port` for the ESP32 NimBLE hook.

## Components

- **Rebase**: `git rebase upstream/master` on `port-darwin` (merge-base is 252 commits behind). Expect the same conflict shape already seen on `picoruby-ble-esp32-port`: `mrbgem.rake`, `mrblib/ble.rb`, `src/ble.c`, `src/mruby/ble.c`. `ports/darwin/*` doesn't exist upstream, so it won't conflict.
- **Hook relocation**: move the `#ifdef PICORB_PLATFORM_DARWIN` drain block from `mrb_pop_packet` (removed) into `mrb_event_popped`.
- **Call-site sweep**: grep `mrbgems/picoruby-ble/ports/darwin/test/*.rb` for direct `pop_packet`/`pop_heartbeat` calls after the rebase lands, and fix any found the same way as R2P2-darwin's `virtual-peripheral/app.rb` (non-blocking `@event_queue.pop(timeout_ms: 0)` + `_event_popped`). `heartbeat_probe.rb` already goes through `BLE#start` and needs no change — confirmed by reading it, not by assumption.

## Code style

No defensive/just-in-case additions, no padding comments. Delete anything the rebase makes dead (old poll-flag variables, functions, comments referencing them) rather than leaving it in place "for safety." Match the conciseness of the surrounding code.

## Testing

1. Host build: `decoder_contract_test.rb` (no radio) as a fast sanity gate.
2. Real hardware: flash `picoruby-ble-esp32-port`'s on-device test probes (`ports/esp32/test/*.rb`) onto the real M5Stack via `stackchan-picoruby`'s `r2p2:*` rake tasks, then run `port-darwin`'s matching Mac-side tests (`e2e_central_test.rb`, flood/GATT-dump/dynamic-read scripts) against it. Human involvement limited to power-cycling the device and one-time Bluetooth permission grants.

## Out of scope

- R2P2-darwin's own re-sync from this branch, R2P2-ESP32's submodule bump, and stackchan-picoruby's vendor refresh + end-to-end verification are separate, later pieces (already agreed decomposition).
