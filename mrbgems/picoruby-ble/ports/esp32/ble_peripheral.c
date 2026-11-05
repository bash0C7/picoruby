#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "../../include/ble.h"
#include "../../include/ble_peripheral.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs_mbuf.h"
#include "nimble/ble.h"
#include "os/os_mbuf.h"

#include "ble_common.h"
#include "nimble_owner.h"

#define BTSTACK_ACL_BUFFERS_FULL 0x57

static const char *BLE_TAG = "prb_ble_peripheral";

/* All entry points take this gate before adv_mutex or SDK host locks.
 * GAP callbacks run without the SDK host lock; synchronous SDK callbacks
 * re-enter on this same task. Never hold this gate across stop / join. */
static SemaphoreHandle_t connection_gate;
static uint16_t con_handle = 0xffff;
static bool connection_accepting;
static bool central_connect_pending;
static uint8_t central_connect_deferred;
#define CONNECT_DEFERRED_REQUEST 0x01
#define CONNECT_DEFERRED_RECORD 0x02
/* Holds only requests not yet generated, and does not reserve resources for an already generated CAN or for later notifies. */
static bool send_permit_pending;
static uint16_t send_permit_handle = 0xffff;

static void
cancel_send_permit(void)
{
  send_permit_pending = false;
  send_permit_handle = 0xffff;
}

bool
picoruby_ble_connection_gate_enter(void)
{
  return connection_gate != NULL &&
         xSemaphoreTakeRecursive(connection_gate, portMAX_DELAY) == pdTRUE;
}

void
picoruby_ble_connection_gate_exit(void)
{
  xSemaphoreGiveRecursive(connection_gate);
}

/* Caller holds the gate. These helpers never call the SDK or VM. */
bool
picoruby_ble_connection_matches(uint16_t handle)
{
  return connection_accepting && picoruby_nimble_ready() && handle != 0xffff && handle == con_handle;
}

bool
picoruby_ble_connection_claim(uint16_t handle)
{
  if (!connection_accepting || !picoruby_nimble_ready() || con_handle != 0xffff || handle == 0xffff) return false;
  con_handle = handle;
  central_connect_pending = false;
  return true;
}

bool
picoruby_ble_connection_release(uint16_t handle)
{
  if (!picoruby_ble_connection_matches(handle)) return false;
  cancel_send_permit();
  con_handle = 0xffff;
  return true;
}

void
picoruby_ble_connection_forget(uint16_t handle)
{
  if (handle == 0xffff || handle != con_handle) return;
  cancel_send_permit();
  con_handle = 0xffff;
}

bool
picoruby_ble_connection_available(void)
{
  return connection_accepting && picoruby_nimble_ready() && con_handle == 0xffff &&
         !central_connect_pending && central_connect_deferred == 0;
}

bool
picoruby_ble_connection_defer_connect(void)
{
  if (!picoruby_ble_connection_available()) return false;
  central_connect_deferred = CONNECT_DEFERRED_REQUEST | CONNECT_DEFERRED_RECORD;
  return true;
}

void
picoruby_ble_connection_clear_deferred(void)
{
  central_connect_deferred = 0;
}

bool
picoruby_ble_connection_release_deferred_record(void)
{
  bool had = (central_connect_deferred & CONNECT_DEFERRED_RECORD) != 0;
  central_connect_deferred &= (uint8_t)~CONNECT_DEFERRED_RECORD;
  return had;
}

bool
picoruby_ble_connection_connect_pending(void)
{
  return connection_accepting && central_connect_pending;
}

bool
picoruby_ble_connection_begin_connect(void)
{
  if (!picoruby_ble_connection_available()) return false;
  central_connect_pending = true;
  return true;
}

void
picoruby_ble_connection_end_connect(void)
{
  central_connect_pending = false;
}

void
picoruby_ble_connection_close(void)
{
  if (!picoruby_ble_connection_gate_enter()) return;
  cancel_send_permit();
  connection_accepting = false;
  central_connect_pending = false;
  central_connect_deferred = 0;
  picoruby_ble_connection_gate_exit();
}

void
picoruby_ble_connection_reopen(void)
{
  if (!picoruby_ble_connection_gate_enter()) return;
  cancel_send_permit();
  con_handle = 0xffff;
  central_connect_pending = false;
  central_connect_deferred = 0;
  connection_accepting = true;
  picoruby_ble_connection_gate_exit();
}

uint16_t
picoruby_ble_connection_handle_snapshot(void)
{
  if (!picoruby_ble_connection_gate_enter()) return 0xffff;
  uint16_t handle = connection_accepting ? con_handle : 0xffff;
  picoruby_ble_connection_gate_exit();
  return handle;
}

/* Inside the connection gate, actually builds the minimal non-empty notify payload and an independent header at the same time.
 * Using only the public API, places the same leading bytes as the fixed SDK's l2cap_pkt; the probe sends nothing. */
static struct os_mbuf *
send_permit_probe(void)
{
  uint8_t dummy = 0;
  struct os_mbuf *payload = ble_hs_mbuf_from_flat(&dummy, 1);
  if (payload == NULL) return NULL;

  uint16_t user_hdr_len = 0;
#if MYNEWT_VAL(BLE_CONTROLLER)
  user_hdr_len = sizeof(struct ble_mbuf_hdr);
#endif
  struct os_mbuf *header = os_msys_get_pkthdr(0, user_hdr_len);
  if (header == NULL) {
    os_mbuf_free_chain(payload);
    return NULL;
  }

  uint16_t leading = 4 + 4 + 1; /* ACL + L2CAP + HCI packet indicator */
#if !CONFIG_BT_NIMBLE_LEGACY_VHCI_ENABLE
  leading += BLE_HS_CTRL_DATA_HDR_SZ;
#endif
  if (OS_MBUF_TRAILINGSPACE(header) < leading) {
    os_mbuf_free_chain(header);
    os_mbuf_free_chain(payload);
    return NULL;
  }
  header->om_data += leading;
  if (os_mbuf_extend(header, 3) == NULL) {
    os_mbuf_free_chain(header);
    os_mbuf_free_chain(payload);
    return NULL;
  }
  os_mbuf_concat(header, payload);
  return header;
}

void
picoruby_ble_peripheral_service_send_permit(void)
{
  if (connection_gate == NULL ||
      xSemaphoreTakeRecursive(connection_gate, 0) != pdTRUE) return;
  if (!send_permit_pending) {
    picoruby_ble_connection_gate_exit();
    return;
  }
  if (!picoruby_ble_connection_matches(send_permit_handle)) {
    cancel_send_permit();
    picoruby_ble_connection_gate_exit();
    return;
  }
  struct os_mbuf *probe = send_permit_probe();
  if (probe == NULL) {
    picoruby_ble_connection_gate_exit();
    return;
  }
  uint8_t p[2] = { EVT_ATT_CAN_SEND_NOW, 0 };
  int result = picoruby_nimble_enqueue_event(p, sizeof(p), PICORUBY_NIMBLE_EVENT_CONTROL);
  cancel_send_permit();
  os_mbuf_free_chain(probe);
  if (result != PICORUBY_NIMBLE_EVENT_ACCEPTED) {
    picoruby_nimble_cancel_control(PICORUBY_NIMBLE_CONTROL_WORK_LOST);
  }
  picoruby_ble_connection_gate_exit();
}

#define ADV_INTERVAL_0625MS 800

static uint8_t adv_data_buf[31];
static uint8_t adv_data_len = 0;
static bool adv_connectable = false;
static bool adv_wanted = false;
static SemaphoreHandle_t adv_mutex;

int
picoruby_ble_peripheral_setup(void)
{
  if (connection_gate == NULL) connection_gate = xSemaphoreCreateRecursiveMutex();
  if (connection_gate == NULL) return -1;
  if (!picoruby_ble_connection_gate_enter()) return -1;
  cancel_send_permit();
  picoruby_ble_connection_gate_exit();
  if (adv_mutex == NULL) adv_mutex = xSemaphoreCreateMutex();
  if (adv_mutex == NULL) return -1;
  if (xSemaphoreTake(adv_mutex, portMAX_DELAY) != pdTRUE) return -1;
  adv_wanted = false;
  adv_data_len = 0;
  adv_connectable = false;
  xSemaphoreGive(adv_mutex);
  return 0;
}

static bool
adv_deferred(void)
{
  return !picoruby_ble_power_working() || con_handle != 0xffff;
}

static int
adv_start(void)
{
  if (!picoruby_nimble_ready()) return -1;
  if (adv_deferred()) return 0;
  struct ble_gap_adv_params params;
  memset(&params, 0, sizeof(params));
  params.conn_mode = adv_connectable ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  params.itvl_min = ADV_INTERVAL_0625MS;
  params.itvl_max = ADV_INTERVAL_0625MS;
  int rc = ble_gap_adv_set_data(adv_data_buf, adv_data_len);
  if (rc == 0) {
    rc = ble_gap_adv_start(picoruby_nimble_own_addr_type(), NULL, BLE_HS_FOREVER,
                           &params, picoruby_ble_gap_event, NULL);
    if (rc != 0) ESP_LOGW(BLE_TAG, "advertising start failed: %d", rc);
  } else {
    ESP_LOGW(BLE_TAG, "advertising set data failed: %d", rc);
  }
  return rc;
}

static void
adv_stop_sdk(void)
{
  int rc = ble_gap_adv_stop();
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    ESP_LOGW(BLE_TAG, "advertising stop failed: %d", rc);
  }
}

int
BLE_peripheral_advertise(uint8_t *adv_data, uint8_t adv_data_len_in, bool connectable)
{
  if (adv_data_len_in > sizeof(adv_data_buf)) return -1;
  if (!picoruby_ble_connection_gate_enter()) return -1;
  if (adv_mutex == NULL || xSemaphoreTake(adv_mutex, portMAX_DELAY) != pdTRUE) {
    picoruby_ble_connection_gate_exit();
    return -1;
  }
  memcpy(adv_data_buf, adv_data, adv_data_len_in);
  adv_data_len = adv_data_len_in;
  adv_connectable = connectable;
  adv_wanted = true;
  int rc = 0;
  if (!adv_deferred()) {
    adv_stop_sdk();
    rc = adv_start();
  }
  xSemaphoreGive(adv_mutex);
  picoruby_ble_connection_gate_exit();
  return rc == 0 ? 0 : -1;
}

void
BLE_peripheral_stop_advertise(void)
{
  if (!picoruby_ble_connection_gate_enter()) return;
  if (adv_mutex == NULL || xSemaphoreTake(adv_mutex, portMAX_DELAY) != pdTRUE) {
    picoruby_ble_connection_gate_exit();
    return;
  }
  adv_wanted = false;
  adv_stop_sdk();
  xSemaphoreGive(adv_mutex);
  picoruby_ble_connection_gate_exit();
}

void
picoruby_ble_peripheral_rearm_adv(void)
{
  if (!picoruby_ble_connection_gate_enter()) return;
  if (adv_mutex == NULL || xSemaphoreTake(adv_mutex, portMAX_DELAY) != pdTRUE) {
    picoruby_ble_connection_gate_exit();
    return;
  }
  if (adv_wanted && !ble_gap_adv_active()) adv_start();
  xSemaphoreGive(adv_mutex);
  picoruby_ble_connection_gate_exit();
}

void
picoruby_ble_peripheral_suspend_adv(void)
{
  if (!ble_hs_is_enabled()) return;
  if (!picoruby_ble_connection_gate_enter()) return;
  if (adv_mutex == NULL || xSemaphoreTake(adv_mutex, portMAX_DELAY) != pdTRUE) {
    picoruby_ble_connection_gate_exit();
    return;
  }
  adv_stop_sdk();
  xSemaphoreGive(adv_mutex);
  picoruby_ble_connection_gate_exit();
}

void
picoruby_ble_peripheral_resume_adv(void)
{
  if (!picoruby_ble_connection_gate_enter()) return;
  if (adv_mutex == NULL || xSemaphoreTake(adv_mutex, portMAX_DELAY) != pdTRUE) {
    picoruby_ble_connection_gate_exit();
    return;
  }
  if (adv_wanted) {
    adv_stop_sdk();
    adv_start();
  }
  xSemaphoreGive(adv_mutex);
  picoruby_ble_connection_gate_exit();
}

static int
notify_status(int rc)
{
  if (rc == BLE_HS_ENOMEM) return BTSTACK_ACL_BUFFERS_FULL;
  return picoruby_ble_request_status(rc);
}

int
BLE_peripheral_notify(uint16_t att_handle)
{
  BLE_read_value_t read_value = { .att_handle = att_handle, .data = NULL, .size = 0 };
  if (BLE_read_data(&read_value) < 0) return -1;
  if (read_value.size == 0) return -1;
  uint16_t nimble_handle = picoruby_ble_handle_r2n(att_handle);
  if (nimble_handle == 0 || !picoruby_ble_connection_gate_enter()) return -1;
  uint16_t handle = picoruby_ble_connection_handle_snapshot();
  /* Revalidate and keep the gate through SDK transmission. GAP disconnect
   * cannot finish and process a reused handle while this operation runs. */
  if (!picoruby_ble_connection_matches(handle)) {
    picoruby_ble_connection_gate_exit();
    return notify_status(BLE_HS_ENOTCONN);
  }
  int payload_rc = picoruby_ble_att_payload_status(handle, read_value.size);
  if (payload_rc != 0) {
    picoruby_ble_connection_gate_exit();
    return notify_status(payload_rc);
  }
  struct os_mbuf *om = ble_hs_mbuf_from_flat(read_value.data, read_value.size);
  int rc = om == NULL ? BLE_HS_ENOMEM : ble_gatts_notify_custom(handle, nimble_handle, om);
  picoruby_ble_connection_gate_exit();
  return notify_status(rc);
}

void
BLE_peripheral_request_can_send_now_event(void)
{
  if (!picoruby_ble_connection_gate_enter()) return;
  if (!picoruby_ble_role_peripheral()) {
    picoruby_ble_connection_gate_exit();
    return;
  }
  uint16_t handle = picoruby_ble_connection_handle_snapshot();
  if (!picoruby_ble_connection_matches(handle)) {
    picoruby_ble_connection_gate_exit();
    return;
  }
  if (!send_permit_pending) {
    send_permit_pending = true;
    send_permit_handle = handle;
  }
  picoruby_ble_peripheral_service_send_permit();
  picoruby_ble_connection_gate_exit();
}
