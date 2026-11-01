#include "nimble_owner.h"
#include "ble_common.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

static const char *TAG = "prb_ble_evq";

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"

#include "../../include/ble.h"
#include "esp_idf_version.h"
#include "syscfg/syscfg.h"

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 4, 4) || \
    (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0) && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 4))
#error "picoruby-ble requires ESP-IDF v5.5.4 or later, or v5.4.4 or later on the v5.4 line"
#endif
#if MYNEWT_VAL(BLE_EXT_ADV)
#error "picoruby-ble requires CONFIG_BT_NIMBLE_EXT_ADV=n"
#endif
#if MYNEWT_VAL(BLE_ENABLE_CONN_REATTEMPT)
#error "picoruby-ble requires CONFIG_BT_NIMBLE_ENABLE_CONN_REATTEMPT=n"
#endif
#if !MYNEWT_VAL(BLE_STORE_CONFIG_PERSIST)
#error "picoruby-ble requires CONFIG_BT_NIMBLE_NVS_PERSIST=y"
#endif
#if !MYNEWT_VAL(BLE_SM_SC)
#error "picoruby-ble requires CONFIG_BT_NIMBLE_SM_SC=y"
#endif
#if PICORUBY_NIMBLE_WRITE_MAX != BLE_ATT_ATTR_MAX_LEN
#error "write queue entry size must equal BLE_ATT_ATTR_MAX_LEN"
#endif

void ble_store_config_init(void);

#define EVQ_DEPTH 32
#define EVQ_DATA_DEPTH 16
#define EVQ_PKT_MAX PICORUBY_NIMBLE_EVT_MAX
#define WRQ_DEPTH PICORUBY_NIMBLE_WRITE_DEPTH
#define WRQ_PKT_MAX PICORUBY_NIMBLE_WRITE_MAX
#define HEARTBEAT_PERIOD_US (1000 * 1000)
#define SYNC_TIMEOUT_TICKS pdMS_TO_TICKS(2000)
#define HOST_DONE_TIMEOUT_TICKS SYNC_TIMEOUT_TICKS

typedef struct {
  uint16_t len;
  uint8_t data[EVQ_PKT_MAX];
} evq_entry_t;

// 遅延 callback に備え、確保後は元の static 配列と同じ寿命で保持する。
static evq_entry_t *evq = NULL;
static int evq_head = 0;
static int evq_count = 0;
static uint32_t evq_gen = 0;
static portMUX_TYPE evq_mux = portMUX_INITIALIZER_UNLOCKED;
static picoruby_nimble_event_rejections_t event_rejections;
static uint32_t host_reset_count;
static bool host_reset_saturated;
static uint8_t control_fault;

typedef struct {
  uint16_t ruby_handle;
  uint16_t len;
  uint8_t data[WRQ_PKT_MAX];
  uint32_t generation;
  int16_t next;
  bool occupied;
} wrq_entry_t;

static wrq_entry_t wrq[WRQ_DEPTH];
static int wrq_head = -1;
static int wrq_tail = -1;
static int wrq_count = 0;
static int wrq_cccd_reserved = 0;
static uint32_t wrq_generation = 0;
/* resetで無効化し、次のVM peekだけが再設定する。世代wrapでも旧ticketを復活させない。 */
static bool wrq_snapshot_valid = false;
static int wrq_snapshot_slot = -1;
static uint32_t wrq_dropped = 0;
static bool wrq_drop_logged = false;
static portMUX_TYPE wrq_mux = portMUX_INITIALIZER_UNLOCKED;

static bool started = false;
static volatile bool synced = false;
static uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
static SemaphoreHandle_t sync_sem = NULL;
static SemaphoreHandle_t host_done_sem = NULL;
static struct ble_npl_event host_exit_event;
static bool host_exit_initialized;
static bool host_stop_pending = false;
static bool host_done_seen = false;
static bool cleanup_blocked = false;
static bool lifecycle_busy = false;
static portMUX_TYPE owner_state_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t heartbeat_timer = NULL;
static int heartbeat_depth = 0; // nest count: only the outermost disable actually stops it
// All runtime accesses use owner_state_mux; timer arrivals may coalesce.
static bool heartbeat_pending = false;

void
picoruby_nimble_cancel_control(uint8_t reason)
{
  taskENTER_CRITICAL(&evq_mux);
  control_fault |= reason;
  taskEXIT_CRITICAL(&evq_mux);
}

uint8_t
picoruby_nimble_control_fault(void)
{
  taskENTER_CRITICAL(&evq_mux);
  uint8_t value = control_fault;
  taskEXIT_CRITICAL(&evq_mux);
  return value;
}

static int
reserve_event_storage(void)
{
  taskENTER_CRITICAL(&evq_mux);
  bool reserved = evq != NULL;
  taskEXIT_CRITICAL(&evq_mux);
  if (reserved) return 0;

  evq_entry_t *storage = heap_caps_malloc(EVQ_DEPTH * sizeof(evq_entry_t),
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (storage == NULL) return -1;

  taskENTER_CRITICAL(&evq_mux);
  if (evq == NULL) {
    evq = storage;
    storage = NULL;
  }
  taskEXIT_CRITICAL(&evq_mux);
  if (storage != NULL) heap_caps_free(storage);
  return 0;
}

// evq_muxを保持して呼ぶ。累積は飽和させ、未観測の拒否をwrapで消さない。
static void
record_event_rejection(uint32_t *counter)
{
  if (*counter == UINT32_MAX) event_rejections.saturated = true;
  else (*counter)++;
}

// event作成前の拒否も、enqueue後の拒否と同じ非破壊snapshotへ記録する。
void
picoruby_nimble_record_oversized_event(void)
{
  taskENTER_CRITICAL(&evq_mux);
  record_event_rejection(&event_rejections.too_large);
  taskEXIT_CRITICAL(&evq_mux);
}

void
picoruby_nimble_record_copy_failure(void)
{
  taskENTER_CRITICAL(&evq_mux);
  event_rejections.copy_failed = true;
  taskEXIT_CRITICAL(&evq_mux);
}

picoruby_nimble_event_result_t
picoruby_nimble_enqueue_event(const uint8_t *pkt, uint16_t len, picoruby_nimble_event_class_t event_class)
{
  taskENTER_CRITICAL(&evq_mux);
  if (len > EVQ_PKT_MAX) {
    record_event_rejection(&event_rejections.too_large);
    taskEXIT_CRITICAL(&evq_mux);
    ESP_LOGW(TAG, "event exceeds queue capacity: %u", (unsigned)len);
    return PICORUBY_NIMBLE_EVENT_TOO_LARGE;
  }
  if (evq == NULL) {
    record_event_rejection(&event_rejections.not_ready);
    taskEXIT_CRITICAL(&evq_mux);
    return PICORUBY_NIMBLE_EVENT_NOT_READY;
  }
  int limit = event_class == PICORUBY_NIMBLE_EVENT_CONTROL ? EVQ_DEPTH : EVQ_DATA_DEPTH;
  if (evq_count >= limit) {
    record_event_rejection(&event_rejections.full);
    taskEXIT_CRITICAL(&evq_mux);
    return PICORUBY_NIMBLE_EVENT_FULL;
  }
  int slot = (evq_head + evq_count) % EVQ_DEPTH;
  evq[slot].len = len;
  memcpy(evq[slot].data, pkt, len);
  evq_count++;
  taskEXIT_CRITICAL(&evq_mux);
  return PICORUBY_NIMBLE_EVENT_ACCEPTED;
}

void
picoruby_nimble_get_event_rejections(picoruby_nimble_event_rejections_t *out)
{
  taskENTER_CRITICAL(&evq_mux);
  *out = event_rejections;
  taskEXIT_CRITICAL(&evq_mux);
}

void
picoruby_nimble_copy_event_rejections(uint8_t *out)
{
  picoruby_nimble_event_rejections_t snapshot;
  picoruby_nimble_get_event_rejections(&snapshot);
  uint32_t values[3] = { snapshot.full, snapshot.too_large, snapshot.not_ready };
  for (int reason = 0; reason < 3; reason++) {
    uint32_t value = values[reason];
    for (int byte = 0; byte < 4; byte++) {
      out[4 * reason + byte] = (uint8_t)(value >> (8 * byte));
    }
  }
  out[12] = (uint8_t)((snapshot.saturated ? PICORUBY_NIMBLE_REJECTION_SATURATED : 0) |
                     (snapshot.copy_failed ? PICORUBY_NIMBLE_REJECTION_COPY_FAILED : 0) |
                     (snapshot.write_rejected ? PICORUBY_NIMBLE_REJECTION_WRITE_REJECTED : 0));
}

void
picoruby_nimble_copy_host_reset_snapshot(uint8_t *out)
{
  taskENTER_CRITICAL(&evq_mux);
  uint32_t count = host_reset_count;
  bool saturated = host_reset_saturated;
  taskEXIT_CRITICAL(&evq_mux);
  for (int byte = 0; byte < 4; byte++) {
    out[byte] = (uint8_t)(count >> (8 * byte));
  }
  out[4] = saturated ? 1 : 0;
}

void
picoruby_nimble_reset_events(void)
{
  taskENTER_CRITICAL(&evq_mux);
  evq_head = 0;
  evq_count = 0;
  evq_gen++;
  taskEXIT_CRITICAL(&evq_mux);
  taskENTER_CRITICAL(&owner_state_mux);
  heartbeat_pending = false;
  taskEXIT_CRITICAL(&owner_state_mux);
}

static int
wrq_enqueue(uint16_t ruby_handle, const uint8_t *data, uint16_t len, bool cccd)
{
  if (ruby_handle == 0 || len == 0 || len > WRQ_PKT_MAX) {
    taskENTER_CRITICAL(&evq_mux);
    event_rejections.write_rejected = true;
    taskEXIT_CRITICAL(&evq_mux);
    return -1;
  }
  taskENTER_CRITICAL(&wrq_mux);
  if (cccd) {
    for (int slot = wrq_head; slot >= 0; slot = wrq[slot].next) {
      if (wrq[slot].ruby_handle != ruby_handle) continue;
      wrq[slot].generation++;
      wrq[slot].len = len;
      memcpy(wrq[slot].data, data, len);
      taskEXIT_CRITICAL(&wrq_mux);
      return 0;
    }
  }
  if (wrq_count >= (cccd ? WRQ_DEPTH : WRQ_DEPTH - wrq_cccd_reserved)) {
    wrq_dropped++;
    bool first = !wrq_drop_logged;
    wrq_drop_logged = true;
    taskEXIT_CRITICAL(&wrq_mux);
    taskENTER_CRITICAL(&evq_mux);
    record_event_rejection(&event_rejections.full);
    taskEXIT_CRITICAL(&evq_mux);
    if (first) {
      ESP_LOGW(TAG, "write queue full (depth=%d), dropping; total dropped=%lu",
               WRQ_DEPTH, (unsigned long)wrq_dropped);
    }
    return -1;
  }
  int slot = 0;
  while (wrq[slot].occupied) slot++;
  wrq[slot].occupied = true;
  wrq[slot].next = -1;
  wrq[slot].generation++;
  wrq[slot].ruby_handle = ruby_handle;
  wrq[slot].len = len;
  memcpy(wrq[slot].data, data, len);
  if (wrq_tail >= 0) wrq[wrq_tail].next = slot;
  else wrq_head = slot;
  wrq_tail = slot;
  wrq_count++;
  taskEXIT_CRITICAL(&wrq_mux);
  return 0;
}

int
picoruby_nimble_enqueue_write(uint16_t ruby_handle, const uint8_t *data, uint16_t len)
{
  return wrq_enqueue(ruby_handle, data, len, false);
}

int
picoruby_nimble_enqueue_cccd(uint16_t ruby_handle, const uint8_t *data, uint16_t len)
{
  return wrq_enqueue(ruby_handle, data, len, true);
}

void
picoruby_nimble_reserve_cccd_writes(int count)
{
  taskENTER_CRITICAL(&wrq_mux);
  wrq_cccd_reserved = count;
  taskEXIT_CRITICAL(&wrq_mux);
}

void
picoruby_nimble_reset_writes(void)
{
  taskENTER_CRITICAL(&wrq_mux);
  wrq_snapshot_valid = false;
  wrq_snapshot_slot = -1;
  wrq_generation++;
  wrq_head = wrq_tail = -1;
  wrq_count = 0;
  for (int i = 0; i < WRQ_DEPTH; i++) wrq[i].occupied = false;
  wrq_drop_logged = false;
  taskEXIT_CRITICAL(&wrq_mux);
}

int
picoruby_nimble_write_pending(uint16_t ruby_handle)
{
  int count = 0;
  taskENTER_CRITICAL(&wrq_mux);
  for (int slot = wrq_head; slot >= 0; slot = wrq[slot].next) {
    if (wrq[slot].ruby_handle == ruby_handle) count++;
  }
  taskEXIT_CRITICAL(&wrq_mux);
  return count;
}

bool
picoruby_nimble_peek_write(uint16_t ruby_handle, uint8_t *copy, uint16_t *len,
                          picoruby_nimble_write_ticket_t *ticket)
{
  taskENTER_CRITICAL(&wrq_mux);
  wrq_snapshot_valid = false;
  wrq_snapshot_slot = -1;
  for (int slot = wrq_head; slot >= 0; slot = wrq[slot].next) {
    if (wrq[slot].ruby_handle != ruby_handle) continue;
    *len = wrq[slot].len;
    memcpy(copy, wrq[slot].data, *len);
    *ticket = (picoruby_nimble_write_ticket_t){
      .queue_generation = wrq_generation,
      .slot_generation = wrq[slot].generation,
      .ruby_handle = ruby_handle,
      .slot = slot,
    };
    wrq_snapshot_slot = slot;
    wrq_snapshot_valid = true;
    taskEXIT_CRITICAL(&wrq_mux);
    return true;
  }
  taskEXIT_CRITICAL(&wrq_mux);
  return false;
}

bool
picoruby_nimble_commit_write(const picoruby_nimble_write_ticket_t *ticket)
{
  taskENTER_CRITICAL(&wrq_mux);
  int slot = ticket->slot;
  bool valid = wrq_snapshot_valid && slot >= 0 && slot < WRQ_DEPTH &&
               wrq_snapshot_slot == slot && ticket->queue_generation == wrq_generation &&
               wrq[slot].occupied && ticket->slot_generation == wrq[slot].generation &&
               ticket->ruby_handle == wrq[slot].ruby_handle;
  wrq_snapshot_valid = false;
  wrq_snapshot_slot = -1;
  if (!valid) {
    taskEXIT_CRITICAL(&wrq_mux);
    return false;
  }
  int prev = -1;
  for (int i = wrq_head; i != slot; i = wrq[i].next) prev = i;
  if (prev < 0) wrq_head = wrq[slot].next;
  else wrq[prev].next = wrq[slot].next;
  if (wrq_tail == slot) wrq_tail = prev;
  wrq[slot].occupied = false;
  wrq_count--;
  taskEXIT_CRITICAL(&wrq_mux);
  return true;
}

static void
drain_events(void)
{
  static uint8_t buf[EVQ_PKT_MAX];
  for (;;) {
    uint16_t len;
    uint32_t gen;
    taskENTER_CRITICAL(&evq_mux);
    if (evq == NULL || evq_count == 0) {
      taskEXIT_CRITICAL(&evq_mux);
      return;
    }
    len = evq[evq_head].len;
    memcpy(buf, evq[evq_head].data, len);
    gen = evq_gen;
    taskEXIT_CRITICAL(&evq_mux);
    if (BLE_push_event(buf, len) != 0) return;
    taskENTER_CRITICAL(&evq_mux);
    if (gen == evq_gen) {
      evq_head = (evq_head + 1) % EVQ_DEPTH;
      evq_count--;
    }
    taskEXIT_CRITICAL(&evq_mux);
  }
}

static void
heartbeat_timer_cb(void *arg)
{
  (void)arg;
  taskENTER_CRITICAL(&owner_state_mux);
  heartbeat_pending = true;
  taskEXIT_CRITICAL(&owner_state_mux);
}

void
picoruby_nimble_pump(void)
{
  picoruby_ble_refresh_read_mirrors();
  picoruby_ble_peripheral_service_send_permit();
  drain_events();
  taskENTER_CRITICAL(&owner_state_mux);
  bool pending = heartbeat_pending;
  heartbeat_pending = false;
  taskEXIT_CRITICAL(&owner_state_mux);
  if (pending && BLE_heartbeat() != 0) {
    taskENTER_CRITICAL(&owner_state_mux);
    heartbeat_pending = true;
    taskEXIT_CRITICAL(&owner_state_mux);
  }
}

void
picoruby_nimble_heartbeat_enable(bool enable)
{
  if (heartbeat_timer == NULL) return;
  bool start_timer = false;
  bool stop_timer = false;
  taskENTER_CRITICAL(&owner_state_mux);
  bool ready = started && synced && !host_stop_pending && !cleanup_blocked && !lifecycle_busy;
  if (enable) {
    if (ready && heartbeat_depth++ == 0) start_timer = true;
  } else if (!ready) {
    heartbeat_depth = 0;
    stop_timer = true;
  } else {
    if (heartbeat_depth > 0) heartbeat_depth--;
    if (heartbeat_depth == 0) stop_timer = true;
  }
  taskEXIT_CRITICAL(&owner_state_mux);
  if (start_timer) esp_timer_start_periodic(heartbeat_timer, HEARTBEAT_PERIOD_US);
  if (stop_timer) esp_timer_stop(heartbeat_timer);
}

// Unconditional stop, not depth-gated: runs on every BLE_init and must not carry nesting state across sessions.
static void
heartbeat_force_stop(void)
{
  taskENTER_CRITICAL(&owner_state_mux);
  heartbeat_depth = 0;
  taskEXIT_CRITICAL(&owner_state_mux);
  if (heartbeat_timer) esp_timer_stop(heartbeat_timer);
}

static void
on_sync(void)
{
  taskENTER_CRITICAL(&owner_state_mux);
  bool allow_sync = !cleanup_blocked && !host_stop_pending && (!lifecycle_busy || !started);
  taskEXIT_CRITICAL(&owner_state_mux);
  if (!allow_sync) return;
  int rc = ble_hs_util_ensure_addr(0);
  uint8_t addr_type = BLE_OWN_ADDR_PUBLIC;
  if (rc == 0) rc = ble_hs_id_infer_auto(0, &addr_type);
  taskENTER_CRITICAL(&owner_state_mux);
  bool give_sync = !cleanup_blocked && !host_stop_pending && (!lifecycle_busy || !started);
  if (give_sync) {
    if (rc == 0) own_addr_type = addr_type;
    synced = rc == 0;
  }
  taskEXIT_CRITICAL(&owner_state_mux);
  if (give_sync && rc == 0) picoruby_ble_connection_reopen();
  if (give_sync && sync_sem) xSemaphoreGive(sync_sem);
}

static void
on_reset(int reason)
{
  (void)reason;
  picoruby_ble_connection_close();
  taskENTER_CRITICAL(&owner_state_mux);
  synced = false;
  taskEXIT_CRITICAL(&owner_state_mux);
  taskENTER_CRITICAL(&evq_mux);
  if (host_reset_count == UINT32_MAX) host_reset_saturated = true;
  else host_reset_count++;
  taskEXIT_CRITICAL(&evq_mux);
}

/* SDK stop completion and this event are separate: the public SDK stop
 * handshake finishes first, then this own event exits our public NPL loop. */
static void
host_exit_event_cb(struct ble_npl_event *event)
{
  (void)event;
}

static bool
run_host_event(struct ble_npl_event *event)
{
  if (!picoruby_ble_connection_gate_enter()) {
    esp_system_abort("BLE connection gate unavailable");
  }
  ble_npl_event_run(event);
  bool exit = event == &host_exit_event;
  if (!exit) {
    picoruby_ble_reconcile_connections();
    picoruby_ble_peripheral_service_send_permit();
  }
  picoruby_ble_connection_gate_exit();
  return exit;
}

static void
host_task(void *param)
{
  (void)param;
  struct ble_npl_eventq *queue = nimble_port_get_dflt_eventq();
  while (true) {
    /* Queue wait must never hold the connection gate. */
    struct ble_npl_event *event = ble_npl_eventq_get(queue, BLE_NPL_TIME_FOREVER);
    if (event != NULL && run_host_event(event)) break;
  }
  xSemaphoreGive(host_done_sem);
  vTaskDelete(NULL);
}

static int
ensure_timers(void)
{
  if (heartbeat_timer == NULL) {
    const esp_timer_create_args_t hargs = {
      .callback = heartbeat_timer_cb,
      .name = "prb_ble_hb",
    };
    if (esp_timer_create(&hargs, &heartbeat_timer) != ESP_OK) return -1;
  }
  return 0;
}

static int
finish_start(int status)
{
  taskENTER_CRITICAL(&owner_state_mux);
  if (status == 0) started = true;
  lifecycle_busy = false;
  taskEXIT_CRITICAL(&owner_state_mux);
  return status;
}

/* Caller owns lifecycle_busy until this existing stop sequence releases it. */
static int
stop_host(bool issue_stop, bool done_seen)
{
  if (issue_stop) {
    picoruby_ble_connection_close(); /* Unlocks before blocking host stop / join. */
    heartbeat_force_stop();
    ble_gap_adv_stop();
    ble_gap_disc_cancel();
    int rc = nimble_port_stop();
    if (rc != 0) {
      taskENTER_CRITICAL(&owner_state_mux);
      cleanup_blocked = true;
      lifecycle_busy = false;
      synced = false;
      taskEXIT_CRITICAL(&owner_state_mux);
      return rc;
    }
    taskENTER_CRITICAL(&owner_state_mux);
    host_stop_pending = true;
    taskEXIT_CRITICAL(&owner_state_mux);
    /* No allocation and no connection gate held on this stop path. */
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &host_exit_event);
  }
  if (!done_seen) {
    if (xSemaphoreTake(host_done_sem, HOST_DONE_TIMEOUT_TICKS) != pdTRUE) {
      taskENTER_CRITICAL(&owner_state_mux);
      lifecycle_busy = false;
      taskEXIT_CRITICAL(&owner_state_mux);
      return -1;
    }
    taskENTER_CRITICAL(&owner_state_mux);
    host_done_seen = true;
    taskEXIT_CRITICAL(&owner_state_mux);
  }
  if (host_exit_initialized) {
    ble_npl_event_deinit(&host_exit_event);
    host_exit_initialized = false;
  }
  int rc = nimble_port_deinit();
  taskENTER_CRITICAL(&owner_state_mux);
  lifecycle_busy = false;
  synced = false;
  if (rc != ESP_OK) {
    cleanup_blocked = true;
  } else {
    started = false;
    host_stop_pending = false;
    host_done_seen = false;
  }
  taskEXIT_CRITICAL(&owner_state_mux);
  return rc;
}

int
picoruby_nimble_start(picoruby_nimble_setup_fn setup)
{
  taskENTER_CRITICAL(&owner_state_mux);
  if (cleanup_blocked || lifecycle_busy || host_stop_pending) {
    taskEXIT_CRITICAL(&owner_state_mux);
    return -1;
  }
  if (started) {
    taskEXIT_CRITICAL(&owner_state_mux);
    return 0;
  }
  lifecycle_busy = true;
  taskEXIT_CRITICAL(&owner_state_mux);
  if (reserve_event_storage() != 0) return finish_start(-1);

  /* Shared NVS may contain data owned by the application. */
  esp_err_t err = nvs_flash_init();
  if (err != ESP_OK) {
    return finish_start(-1);
  }

  if (nimble_port_init() != ESP_OK) {
    return finish_start(-1);
  }

  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.reset_cb = on_reset;
  ble_hs_cfg.gatts_register_cb = picoruby_ble_gatts_register_cb;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_store_config_init();

  if (setup && setup() != 0) {
    if (nimble_port_deinit() != ESP_OK) {
      esp_system_abort("NimBLE startup cleanup failed");
    }
    return finish_start(-1);
  }

  if (ensure_timers() != 0) {
    if (nimble_port_deinit() != ESP_OK) {
      esp_system_abort("NimBLE startup cleanup failed");
    }
    return finish_start(-1);
  }
  picoruby_nimble_attach_vm(NULL);
  picoruby_nimble_reset_events();
  picoruby_nimble_reset_writes();

  if (sync_sem == NULL) sync_sem = xSemaphoreCreateBinary();
  if (sync_sem == NULL) {
    if (nimble_port_deinit() != ESP_OK) {
      esp_system_abort("NimBLE startup cleanup failed");
    }
    return finish_start(-1);
  }
  if (host_done_sem == NULL) host_done_sem = xSemaphoreCreateBinary();
  if (host_done_sem == NULL) {
    if (nimble_port_deinit() != ESP_OK) {
      esp_system_abort("NimBLE startup cleanup failed");
    }
    return finish_start(-1);
  }
  /* Public NPL event_init allocates/asserts at initialization, never at stop. */
  ble_npl_event_init(&host_exit_event, host_exit_event_cb, NULL);
  host_exit_initialized = true;
  (void)xSemaphoreTake(sync_sem, 0);
  taskENTER_CRITICAL(&owner_state_mux);
  synced = false;
  host_stop_pending = false;
  host_done_seen = false;
  taskEXIT_CRITICAL(&owner_state_mux);
  taskENTER_CRITICAL(&evq_mux);
  control_fault = 0;
  taskEXIT_CRITICAL(&evq_mux);
  if (xTaskCreatePinnedToCore(host_task, "nimble_host", NIMBLE_HS_STACK_SIZE,
                              NULL, (configMAX_PRIORITIES - 4), NULL, NIMBLE_CORE) != pdPASS) {
    ble_npl_event_deinit(&host_exit_event);
    host_exit_initialized = false;
    if (nimble_port_deinit() != ESP_OK) {
      esp_system_abort("NimBLE startup cleanup failed");
    }
    return finish_start(-1);
  }

  if (xSemaphoreTake(sync_sem, SYNC_TIMEOUT_TICKS) != pdTRUE) {
    esp_system_abort("NimBLE initial sync timeout");
  }

  taskENTER_CRITICAL(&owner_state_mux);
  bool sync_ok = synced;
  if (!sync_ok) started = true; /* Host resources need the normal stop/join/deinit. */
  taskEXIT_CRITICAL(&owner_state_mux);
  if (!sync_ok) {
    (void)stop_host(true, false);
    return -1;
  }

  return finish_start(0);
}

int
picoruby_nimble_stop(void)
{
  taskENTER_CRITICAL(&owner_state_mux);
  if (cleanup_blocked || lifecycle_busy) {
    taskEXIT_CRITICAL(&owner_state_mux);
    return -1;
  }
  if (!started) {
    taskEXIT_CRITICAL(&owner_state_mux);
    return 0;
  }
  lifecycle_busy = true;
  bool issue_stop = !host_stop_pending;
  bool done_seen = host_done_seen;
  taskEXIT_CRITICAL(&owner_state_mux);
  return stop_host(issue_stop, done_seen);
}

bool
picoruby_nimble_started(void)
{
  taskENTER_CRITICAL(&owner_state_mux);
  bool value = started;
  taskEXIT_CRITICAL(&owner_state_mux);
  return value;
}

bool
picoruby_nimble_ready(void)
{
  taskENTER_CRITICAL(&owner_state_mux);
  bool value = started && synced && !host_stop_pending && !cleanup_blocked && !lifecycle_busy;
  taskEXIT_CRITICAL(&owner_state_mux);
  return value && picoruby_nimble_control_fault() == 0;
}

bool
picoruby_nimble_cleanup_blocked(void)
{
  taskENTER_CRITICAL(&owner_state_mux);
  bool value = cleanup_blocked;
  taskEXIT_CRITICAL(&owner_state_mux);
  return value;
}

uint8_t
picoruby_nimble_own_addr_type(void)
{
  taskENTER_CRITICAL(&owner_state_mux);
  uint8_t value = own_addr_type;
  taskEXIT_CRITICAL(&owner_state_mux);
  return value;
}
