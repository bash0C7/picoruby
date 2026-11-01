#ifndef BLE_COMMON_DEFINED_H_
#define BLE_COMMON_DEFINED_H_

#include <stdint.h>
#include <stdbool.h>

struct ble_gap_event;
struct ble_gatt_register_ctxt;

#ifdef __cplusplus
extern "C" {
#endif

/* Candidate physical gate; retirement / immutable origin integration pending.
 * Gate precedes adv_mutex and SDK host locks. Never span stop / join. */
bool picoruby_ble_connection_gate_enter(void);
void picoruby_ble_connection_gate_exit(void);
bool picoruby_ble_connection_matches(uint16_t handle);
bool picoruby_ble_connection_claim(uint16_t handle);
bool picoruby_ble_connection_release(uint16_t handle);
bool picoruby_ble_connection_available(void);
bool picoruby_ble_connection_connect_pending(void);
bool picoruby_ble_connection_begin_connect(void);
void picoruby_ble_connection_end_connect(void);
void picoruby_ble_connection_close(void);
void picoruby_ble_connection_reopen(void);
/* Caller owns the gate for one complete SDK host event plus reconciliation. */
void picoruby_ble_reconcile_connections(void);
bool picoruby_ble_central_reconcile_connect(void);
uint16_t picoruby_ble_connection_handle_snapshot(void);
/* Caller owns the connection gate, or SDK init has discarded old procedures. */
void picoruby_ble_central_reset_request(void);
/* Caller keeps the gate from this MTU check through the SDK call. */
int picoruby_ble_att_payload_status(uint16_t handle, uint16_t size);

#define EVT_ATT_CAN_SEND_NOW 0xB7

int picoruby_ble_gap_event(struct ble_gap_event *event, void *arg);
void picoruby_ble_gatts_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg);
uint16_t picoruby_ble_handle_r2n(uint16_t ruby_handle);
#define PICORUBY_BLE_READ_MIRROR_MAX 256
void picoruby_ble_publish_read_value(uint16_t ruby_handle, const uint8_t *data, uint16_t size);
void picoruby_ble_refresh_read_mirrors(void);
int picoruby_ble_peripheral_setup(void);
void picoruby_ble_peripheral_rearm_adv(void);
void picoruby_ble_peripheral_service_send_permit(void);

// SDK operation failure snapshot: little-endian count:u32, last_rc:i32, stage:u8, flags:u8.
// Process-lifetime, read-only; failure count does not imply advertising is stopped.
#define PICORUBY_BLE_ADVERTISING_FAILURE_SNAPSHOT_SIZE 10
#define PICORUBY_BLE_ADVERTISING_FAILURE_STOP 1
#define PICORUBY_BLE_ADVERTISING_FAILURE_SET_DATA 2
#define PICORUBY_BLE_ADVERTISING_FAILURE_START 3
#define PICORUBY_BLE_ADVERTISING_FAILURE_SATURATED 1
void picoruby_ble_copy_advertising_failure_snapshot(uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BLE_COMMON_DEFINED_H_ */
