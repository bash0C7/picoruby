#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../include/ble.h"
#include "../../include/ble_central.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"

#include "ble_common.h"
#include "nimble_owner.h"
#include "esp_log.h"

#define ERROR_CODE_COMMAND_DISALLOWED 0x0C
#define BTSTACK_MEMORY_ALLOC_FAILED 0x56

static const char *BLE_TAG = "prb_ble_central";

static ble_addr_t central_connect_peer;

static struct ble_gap_disc_params scan_params = {
  .itvl = 0x60,
  .window = 0x30,
  .passive = 1,
};

int
BLE_central_set_scan_params(uint8_t scan_type, uint16_t scan_interval, uint16_t scan_window, uint8_t scanning_filter_policy)
{
  scan_params.itvl = scan_interval;
  scan_params.window = scan_window;
  scan_params.passive = (scan_type == 0);
  scan_params.filter_policy = scanning_filter_policy;
  scan_params.filter_duplicates = 0;
  return 0;
}

static bool scan_wanted;

static int
scan_start(void)
{
  return ble_gap_disc(picoruby_nimble_own_addr_type(), BLE_HS_FOREVER,
               &scan_params, picoruby_ble_gap_event, NULL);
}

static void
scan_restart(void)
{
  int rc = scan_start();
  if (rc != 0 && !(rc == BLE_HS_EALREADY && ble_gap_disc_active())) {
    ESP_LOGW(BLE_TAG, "scan restart failed: %d", rc);
  }
}

int
BLE_central_start_scan(void)
{
  if (!picoruby_ble_connection_gate_enter()) return -1;
  int rc = 0;
  if (picoruby_ble_power_working() && !picoruby_ble_connection_connect_pending()) {
    rc = scan_start();
    if (rc == BLE_HS_EALREADY && ble_gap_disc_active()) rc = 0;
  }
  if (rc == 0) scan_wanted = true;
  picoruby_ble_connection_gate_exit();
  if (rc == 0) return 0;
  ESP_LOGW(BLE_TAG, "scan start failed: %d", rc);
  return -1;
}

int
BLE_central_stop_scan(void)
{
  if (!picoruby_ble_connection_gate_enter()) return -1;
  scan_wanted = false;
  int rc = ble_gap_disc_cancel();
  picoruby_ble_connection_gate_exit();
  if (rc == 0 || rc == BLE_HS_EALREADY) return 0;
  ESP_LOGW(BLE_TAG, "scan stop failed: %d", rc);
  return -1;
}

void
picoruby_ble_central_scan_reset(void)
{
  scan_wanted = false;
}

void
picoruby_ble_central_suspend_scan(void)
{
  if (!ble_hs_is_enabled()) return;
  ble_gap_disc_cancel();
}

void
picoruby_ble_central_resume_after_connect(void)
{
  if (!scan_wanted || !picoruby_ble_power_working() || picoruby_ble_connection_connect_pending()) return;
  if (ble_gap_disc_active()) return;
  scan_restart();
}

void
picoruby_ble_central_resume_scan(void)
{
  if (!scan_wanted || picoruby_ble_connection_connect_pending()) return;
  ble_gap_disc_cancel();
  scan_restart();
}

uint8_t
BLE_central_gap_connect(const uint8_t *addr, uint8_t addr_type)
{
  if (!picoruby_ble_connection_gate_enter()) {
    ESP_LOGW(BLE_TAG, "connect rejected: connection gate unavailable");
    return ERROR_CODE_COMMAND_DISALLOWED;
  }
  ble_addr_t peer;
  peer.type = addr_type;
  for (int i = 0; i < 6; i++) peer.val[i] = addr[5 - i];
  bool deferred = !picoruby_ble_power_working();
  bool started = picoruby_nimble_ready() &&
                 (deferred ? picoruby_ble_connection_defer_connect() : picoruby_ble_connection_begin_connect());
  if (!started) {
    uint8_t status = ERROR_CODE_COMMAND_DISALLOWED;
    if (picoruby_nimble_ready() && !picoruby_ble_connection_connect_pending() &&
        picoruby_ble_connection_handle_snapshot() != 0xffff &&
        ble_gap_conn_find_by_addr(&peer, NULL) == BLE_HS_ENOTCONN) {
      status = BTSTACK_MEMORY_ALLOC_FAILED;
    }
    ESP_LOGW(BLE_TAG, "connect rejected: status=0x%02x ready=%d pending=%d held=0x%04x deferred=%d",
             (unsigned)status, (int)picoruby_nimble_ready(), (int)picoruby_ble_connection_connect_pending(),
             (unsigned)picoruby_ble_connection_handle_snapshot(), (int)deferred);
    picoruby_ble_connection_gate_exit();
    return status;
  }
  if (deferred) {
    picoruby_ble_connection_gate_exit();
    return 0;
  }
  if (ble_gap_disc_active()) {
    ble_gap_disc_cancel();
  }
  central_connect_peer = peer;

  struct ble_gap_conn_params params = {
    .scan_itvl = 60000 / 625,
    .scan_window = 30000 / 625,
    .itvl_min = 10000 / 1250,
    .itvl_max = 30000 / 1250,
    .latency = 4,
    .supervision_timeout = 7200 / 10,
    .min_ce_len = 10000 / 625,
    .max_ce_len = 30000 / 625,
  };

  int rc = ble_gap_connect(picoruby_nimble_own_addr_type(), &peer, BLE_HS_FOREVER,
                           &params, picoruby_ble_gap_event, NULL);
  if (rc != 0) {
    picoruby_ble_connection_end_connect();
    picoruby_ble_central_resume_after_connect();
  }
  picoruby_ble_connection_gate_exit();
  if (rc == 0) return 0;
  ESP_LOGW(BLE_TAG, "connect failed: %d", rc);
  return rc == BLE_HS_ENOMEM ? BTSTACK_MEMORY_ALLOC_FAILED : ERROR_CODE_COMMAND_DISALLOWED;
}

bool
picoruby_ble_central_reconcile_connect(void)
{
  if (!picoruby_ble_connection_connect_pending() || ble_gap_conn_active()) return false;
  if (ble_gap_conn_find_by_addr(&central_connect_peer, NULL) != BLE_HS_ENOTCONN) return false;
  picoruby_ble_connection_end_connect();
  return true;
}

bool
picoruby_ble_central_connect_lost(const struct ble_gap_conn_desc *conn)
{
  if (!picoruby_ble_connection_connect_pending()) return false;
  if (ble_addr_cmp(&central_connect_peer, &conn->peer_ota_addr) != 0 &&
      ble_addr_cmp(&central_connect_peer, &conn->peer_id_addr) != 0) return false;
  picoruby_ble_connection_end_connect();
  return true;
}
