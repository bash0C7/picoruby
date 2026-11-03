#ifndef BLE_COMMON_DEFINED_H_
#define BLE_COMMON_DEFINED_H_

#include <stdint.h>
#include <stdbool.h>

struct ble_gap_event;
struct ble_gap_conn_desc;
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
void picoruby_ble_connection_forget(uint16_t handle);
bool picoruby_ble_connection_available(void);
bool picoruby_ble_connection_defer_connect(void);
void picoruby_ble_connection_clear_deferred(void);
bool picoruby_ble_connection_release_deferred_record(void);
bool picoruby_ble_connection_connect_pending(void);
bool picoruby_ble_connection_begin_connect(void);
void picoruby_ble_connection_end_connect(void);
void picoruby_ble_connection_close(void);
void picoruby_ble_connection_reopen(void);
/* Caller owns the gate for one complete SDK host event plus reconciliation. */
void picoruby_ble_reconcile_connections(void);
bool picoruby_ble_central_reconcile_connect(void);
bool picoruby_ble_central_connect_lost(const struct ble_gap_conn_desc *conn);
void picoruby_ble_halting_watchdog_fire(void);
void picoruby_ble_central_scan_reset(void);
void picoruby_ble_central_suspend_scan(void);
void picoruby_ble_central_resume_scan(void);
void picoruby_ble_central_resume_after_connect(void);
bool picoruby_ble_power_working(void);
bool picoruby_ble_role_peripheral(void);
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
void picoruby_ble_peripheral_suspend_adv(void);
void picoruby_ble_peripheral_resume_adv(void);
void picoruby_ble_peripheral_service_send_permit(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_COMMON_DEFINED_H_ */
