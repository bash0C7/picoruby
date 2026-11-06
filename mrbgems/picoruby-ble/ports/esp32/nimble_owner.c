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
#if MYNEWT_VAL(BLE_HOST_ALLOW_CONNECT_WITH_SCAN)
#error "picoruby-ble requires CONFIG_BT_NIMBLE_HOST_ALLOW_CONNECT_WITH_SCAN=n"
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
#define HALTING_WATCHDOG_US (1000 * 1000)
#define SYNC_TIMEOUT_TICKS pdMS_TO_TICKS(2000)
#define HOST_DONE_TIMEOUT_TICKS SYNC_TIMEOUT_TICKS

typedef struct {
  uint16_t len;
  uint8_t data[EVQ_PKT_MAX];
} evq_entry_t;

static evq_entry_t *evq = NULL;
static int evq_head = 0;
static int evq_count = 0;
static uint32_t evq_dropped = 0;
static portMUX_TYPE evq_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t control_fault;

typedef struct {
  uint16_t ruby_handle;
  uint16_t len;
  uint8_t data[WRQ_PKT_MAX];
  uint32_t generation;
  int16_t next;
  bool occupied;
  bool cccd;
} wrq_entry_t;

static wrq_entry_t wrq[WRQ_DEPTH];
static int wrq_head = -1;
static int wrq_tail = -1;
static int wrq_count = 0;
static int wrq_cccd_reserved = 0;
static int wrq_cccd_queued = 0;
static uint32_t wrq_dropped = 0;
static portMUX_TYPE wrq_mux = portMUX_INITIALIZER_UNLOCKED;

static bool started = false;
static volatile bool synced = false;
static uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
static SemaphoreHandle_t sync_sem = NULL;
static SemaphoreHandle_t host_done_sem = NULL;
static struct ble_npl_event host_exit_event;
static bool host_stop_pending = false;
static bool cleanup_blocked = false;
static bool lifecycle_busy = false;
static portMUX_TYPE owner_state_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t heartbeat_timer = NULL;
static esp_timer_handle_t halting_timer = NULL;
static bool heartbeat_pending = false;

#define LOG_LIMIT_INTERVAL_US 1000000
static portMUX_TYPE log_limit_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t log_next_us[PICORUBY_LOG_SITE_COUNT];
static uint32_t log_suppressed[PICORUBY_LOG_SITE_COUNT];
static const char *const log_site_name[PICORUBY_LOG_SITE_COUNT] = {
  "ATT access without connection gate",
  "ATT access without matching connection",
  "ATT access without attribute map",
  "ATT security check without connection",
  "read response append failure",
  "write exceeding capacity",
  "write mbuf copy failure",
  "late connection termination failure",
  "advertising report over 31 bytes",
  "notification receive failure",
  "encryption change failure",
  "bond read failure",
  "repeat pairing ignored",
  "advertising start failure",
  "notify failure",
  "event queue full",
  "write queue full",
  "CCCD value replaced before delivery",
  "write delivery failure",
};

bool
picoruby_log_due(picoruby_log_site_t site, uint32_t *suppressed)
{
  int64_t now = esp_timer_get_time();
  taskENTER_CRITICAL(&log_limit_mux);
  if (now < log_next_us[site]) {
    log_suppressed[site]++;
    taskEXIT_CRITICAL(&log_limit_mux);
    return false;
  }
  *suppressed = log_suppressed[site];
  log_suppressed[site] = 0;
  log_next_us[site] = now + LOG_LIMIT_INTERVAL_US;
  taskEXIT_CRITICAL(&log_limit_mux);
  return true;
}

void
picoruby_log_flush(void)
{
  int64_t now = esp_timer_get_time();
  for (int site = 0; site < PICORUBY_LOG_SITE_COUNT; site++) {
    uint32_t n = 0;
    taskENTER_CRITICAL(&log_limit_mux);
    if (log_suppressed[site] > 0 && now >= log_next_us[site]) {
      n = log_suppressed[site];
      log_suppressed[site] = 0;
      log_next_us[site] = now + LOG_LIMIT_INTERVAL_US;
    }
    taskEXIT_CRITICAL(&log_limit_mux);
    if (n > 0) {
      ESP_LOGW(TAG, "%s: %lu more suppressed", log_site_name[site], (unsigned long)n);
    }
  }
}

static void
heartbeat_mark_pending(void)
{
  taskENTER_CRITICAL(&owner_state_mux);
  heartbeat_pending = true;
  taskEXIT_CRITICAL(&owner_state_mux);
}

void
picoruby_nimble_cancel_control(uint8_t reason)
{
  taskENTER_CRITICAL(&evq_mux);
  control_fault |= reason;
  taskEXIT_CRITICAL(&evq_mux);
  heartbeat_mark_pending();
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
  evq = storage;
  taskEXIT_CRITICAL(&evq_mux);
  return 0;
}

picoruby_nimble_event_result_t
picoruby_nimble_enqueue_event(const uint8_t *pkt, uint16_t len, picoruby_nimble_event_class_t event_class)
{
  taskENTER_CRITICAL(&evq_mux);
  int limit = event_class == PICORUBY_NIMBLE_EVENT_CONTROL ? EVQ_DEPTH : EVQ_DATA_DEPTH;
  if (evq_count >= limit) {
    evq_dropped++;
    uint32_t dropped = evq_dropped;
    taskEXIT_CRITICAL(&evq_mux);
    uint32_t s;
    if (picoruby_log_due(PICORUBY_LOG_EVENT_QUEUE, &s)) {
      ESP_LOGW(TAG, "event queue full (limit=%d), dropping newest; total dropped=%lu (%lu suppressed)",
               limit, (unsigned long)dropped, (unsigned long)s);
    }
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
picoruby_nimble_reset_events(void)
{
  taskENTER_CRITICAL(&evq_mux);
  evq_head = 0;
  evq_count = 0;
  taskEXIT_CRITICAL(&evq_mux);
  taskENTER_CRITICAL(&owner_state_mux);
  heartbeat_pending = false;
  taskEXIT_CRITICAL(&owner_state_mux);
}

static int
wrq_enqueue(uint16_t ruby_handle, const uint8_t *data, uint16_t len, bool cccd)
{
  taskENTER_CRITICAL(&wrq_mux);
  if (cccd) {
    for (int slot = wrq_head; slot >= 0; slot = wrq[slot].next) {
      if (wrq[slot].ruby_handle != ruby_handle) continue;
      wrq[slot].generation++;
      wrq[slot].len = len;
      memcpy(wrq[slot].data, data, len);
      taskEXIT_CRITICAL(&wrq_mux);
      uint32_t s;
      if (picoruby_log_due(PICORUBY_LOG_CCCD_REPLACED, &s)) {
        ESP_LOGW(TAG, "CCCD value for handle %u replaced before delivery (%lu suppressed)",
                 (unsigned)ruby_handle, (unsigned long)s);
      }
      return 0;
    }
  }
  if (cccd ? wrq_count >= WRQ_DEPTH : wrq_count - wrq_cccd_queued >= WRQ_DEPTH - wrq_cccd_reserved) {
    wrq_dropped++;
    uint32_t dropped = wrq_dropped;
    taskEXIT_CRITICAL(&wrq_mux);
    uint32_t s;
    if (picoruby_log_due(PICORUBY_LOG_WRITE_QUEUE, &s)) {
      ESP_LOGW(TAG, "write queue full (depth=%d), dropping; total dropped=%lu (%lu suppressed)",
               WRQ_DEPTH, (unsigned long)dropped, (unsigned long)s);
    }
    return -1;
  }
  int slot = 0;
  while (wrq[slot].occupied) slot++;
  wrq[slot].occupied = true;
  wrq[slot].cccd = cccd;
  wrq[slot].next = -1;
  wrq[slot].ruby_handle = ruby_handle;
  wrq[slot].len = len;
  memcpy(wrq[slot].data, data, len);
  if (wrq_tail >= 0) wrq[wrq_tail].next = slot;
  else wrq_head = slot;
  wrq_tail = slot;
  wrq_count++;
  if (cccd) wrq_cccd_queued++;
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
  wrq_head = wrq_tail = -1;
  wrq_count = 0;
  wrq_cccd_queued = 0;
  for (int i = 0; i < WRQ_DEPTH; i++) wrq[i].occupied = false;
  taskEXIT_CRITICAL(&wrq_mux);
}

static void
drain_events(void)
{
  static uint8_t buf[EVQ_PKT_MAX];
  for (;;) {
    uint16_t len;
    taskENTER_CRITICAL(&evq_mux);
    if (evq == NULL || evq_count == 0) {
      taskEXIT_CRITICAL(&evq_mux);
      return;
    }
    len = evq[evq_head].len;
    memcpy(buf, evq[evq_head].data, len);
    taskEXIT_CRITICAL(&evq_mux);
    if (BLE_push_event(buf, len) != 0) return;
    taskENTER_CRITICAL(&evq_mux);
    evq_head = (evq_head + 1) % EVQ_DEPTH;
    evq_count--;
    taskEXIT_CRITICAL(&evq_mux);
  }
}

static void
drain_writes(void)
{
  static uint8_t buf[WRQ_PKT_MAX];
  for (;;) {
    uint16_t handle;
    uint16_t len;
    uint32_t slot_generation;
    int slot;
    taskENTER_CRITICAL(&wrq_mux);
    if (wrq_head < 0) {
      taskEXIT_CRITICAL(&wrq_mux);
      return;
    }
    slot = wrq_head;
    handle = wrq[slot].ruby_handle;
    len = wrq[slot].len;
    memcpy(buf, wrq[slot].data, len);
    slot_generation = wrq[slot].generation;
    taskEXIT_CRITICAL(&wrq_mux);
    if (BLE_write_data(handle, buf, len) != 0) {
      uint32_t s;
      if (picoruby_log_due(PICORUBY_LOG_WRITE_DELIVERY, &s)) {
        ESP_LOGW(TAG, "write delivery to the VM failed: handle=%u len=%u (%lu suppressed)",
                 (unsigned)handle, (unsigned)len, (unsigned long)s);
      }
      return;
    }
    taskENTER_CRITICAL(&wrq_mux);
    if (wrq[slot].generation == slot_generation) {
      wrq_head = wrq[slot].next;
      if (wrq_head < 0) wrq_tail = -1;
      wrq[slot].occupied = false;
      wrq_count--;
      if (wrq[slot].cccd) wrq_cccd_queued--;
    }
    taskEXIT_CRITICAL(&wrq_mux);
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

static void
halting_timer_cb(void *arg)
{
  (void)arg;
  picoruby_ble_halting_watchdog_fire();
}

void
picoruby_nimble_halting_watchdog_start(void)
{
  esp_err_t err = esp_timer_start_once(halting_timer, HALTING_WATCHDOG_US);
  if (err != ESP_OK) ESP_LOGE(TAG, "halting watchdog start failed: %d", err);
}

void
picoruby_nimble_halting_watchdog_stop(void)
{
  if (halting_timer) esp_timer_stop(halting_timer);
}

void
picoruby_nimble_pump(void)
{
  picoruby_log_flush();
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
  drain_writes();
}

void
picoruby_nimble_heartbeat_enable(bool enable)
{
  if (heartbeat_timer == NULL) return;
  if (!enable) {
    esp_timer_stop(heartbeat_timer);
    return;
  }
  taskENTER_CRITICAL(&owner_state_mux);
  bool ready = synced && !host_stop_pending;
  taskEXIT_CRITICAL(&owner_state_mux);
  if (ready) {
    esp_err_t err = esp_timer_start_periodic(heartbeat_timer, HEARTBEAT_PERIOD_US);
    if (err != ESP_OK) ESP_LOGE(TAG, "heartbeat timer start failed: %d", err);
  }
}

static void
heartbeat_force_stop(void)
{
  esp_timer_stop(heartbeat_timer);
}

static void
on_sync(void)
{
  taskENTER_CRITICAL(&owner_state_mux);
  bool allow_sync = !cleanup_blocked && !host_stop_pending && (!lifecycle_busy || !started);
  taskEXIT_CRITICAL(&owner_state_mux);
  if (!allow_sync) return;
  int rc = ble_hs_util_ensure_addr(0);
  if (rc != 0) ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
  uint8_t addr_type = BLE_OWN_ADDR_PUBLIC;
  if (rc == 0) {
    rc = ble_hs_id_infer_auto(0, &addr_type);
    if (rc != 0) ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
  }
  taskENTER_CRITICAL(&owner_state_mux);
  bool give_sync = !cleanup_blocked && !host_stop_pending && (!lifecycle_busy || !started);
  if (give_sync) {
    if (rc == 0) own_addr_type = addr_type;
    synced = rc == 0;
  }
  taskEXIT_CRITICAL(&owner_state_mux);
  if (give_sync && rc == 0) picoruby_ble_connection_reopen();
  if (give_sync) xSemaphoreGive(sync_sem);
}

static void
on_reset(int reason)
{
  picoruby_ble_connection_close();
  picoruby_ble_known_links_clear();
  taskENTER_CRITICAL(&owner_state_mux);
  synced = false;
  taskEXIT_CRITICAL(&owner_state_mux);
  ESP_LOGW(TAG, "NimBLE host reset (reason %d)", reason);
}

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
    struct ble_npl_event *event = ble_npl_eventq_get(queue, BLE_NPL_TIME_FOREVER);
    if (run_host_event(event)) break;
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
    esp_err_t err = esp_timer_create(&hargs, &heartbeat_timer);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "heartbeat timer creation failed: %d", err);
      return -1;
    }
  }
  if (halting_timer == NULL) {
    const esp_timer_create_args_t wargs = {
      .callback = halting_timer_cb,
      .name = "prb_ble_halt",
    };
    esp_err_t err = esp_timer_create(&wargs, &halting_timer);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "halting timer creation failed: %d", err);
      return -1;
    }
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

static int
stop_host(bool issue_stop)
{
  if (issue_stop) {
    picoruby_ble_connection_close();
    heartbeat_force_stop();
    picoruby_nimble_halting_watchdog_stop();
    int adv_rc = ble_gap_adv_stop();
    if (adv_rc != 0 && adv_rc != BLE_HS_EALREADY) {
      ESP_LOGW(TAG, "advertising stop at host stop failed: %d", adv_rc);
    }
    int scan_rc = ble_gap_disc_cancel();
    if (scan_rc != 0 && scan_rc != BLE_HS_EALREADY) {
      ESP_LOGW(TAG, "scan cancel at host stop failed: %d", scan_rc);
    }
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
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &host_exit_event);
  }
  if (xSemaphoreTake(host_done_sem, HOST_DONE_TIMEOUT_TICKS) != pdTRUE) {
    taskENTER_CRITICAL(&owner_state_mux);
    lifecycle_busy = false;
    taskEXIT_CRITICAL(&owner_state_mux);
    return -1;
  }
  ble_npl_event_deinit(&host_exit_event);
  int rc = nimble_port_deinit();
  taskENTER_CRITICAL(&owner_state_mux);
  lifecycle_busy = false;
  synced = false;
  if (rc != ESP_OK) {
    cleanup_blocked = true;
  } else {
    started = false;
    host_stop_pending = false;
  }
  taskEXIT_CRITICAL(&owner_state_mux);
  return rc;
}

int
picoruby_nimble_start(picoruby_nimble_setup_fn setup)
{
  taskENTER_CRITICAL(&owner_state_mux);
  lifecycle_busy = true;
  taskEXIT_CRITICAL(&owner_state_mux);
  if (reserve_event_storage() != 0) {
    ESP_LOGE(TAG, "event queue storage allocation failed");
    return finish_start(-1);
  }

  esp_err_t err = nvs_flash_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_flash_init failed: %d", err);
    return finish_start(-1);
  }

  esp_err_t port_err = nimble_port_init();
  if (port_err != ESP_OK) {
    ESP_LOGE(TAG, "nimble_port_init failed: %d", port_err);
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

  int setup_rc = setup ? setup() : 0;
  if (setup_rc != 0) {
    ESP_LOGE(TAG, "GATT setup failed: %d", setup_rc);
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
  picoruby_nimble_reset_events();
  picoruby_nimble_reset_writes();

  if (sync_sem == NULL) sync_sem = xSemaphoreCreateBinary();
  if (sync_sem == NULL) {
    ESP_LOGE(TAG, "sync semaphore allocation failed");
    if (nimble_port_deinit() != ESP_OK) {
      esp_system_abort("NimBLE startup cleanup failed");
    }
    return finish_start(-1);
  }
  if (host_done_sem == NULL) host_done_sem = xSemaphoreCreateBinary();
  if (host_done_sem == NULL) {
    ESP_LOGE(TAG, "host-done semaphore allocation failed");
    if (nimble_port_deinit() != ESP_OK) {
      esp_system_abort("NimBLE startup cleanup failed");
    }
    return finish_start(-1);
  }
  ble_npl_event_init(&host_exit_event, host_exit_event_cb, NULL);
  (void)xSemaphoreTake(sync_sem, 0);
  taskENTER_CRITICAL(&evq_mux);
  control_fault = 0;
  taskEXIT_CRITICAL(&evq_mux);
  if (xTaskCreatePinnedToCore(host_task, "nimble_host", NIMBLE_HS_STACK_SIZE,
                              NULL, (configMAX_PRIORITIES - 4), NULL, NIMBLE_CORE) != pdPASS) {
    ESP_LOGE(TAG, "host task creation failed");
    ble_npl_event_deinit(&host_exit_event);
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
  if (!sync_ok) started = true;
  taskEXIT_CRITICAL(&owner_state_mux);
  if (!sync_ok) {
    int stop_rc = stop_host(true);
    ESP_LOGE(TAG, "NimBLE did not sync; stop_host returned %d", stop_rc);
    return -1;
  }

  return finish_start(0);
}

int
picoruby_nimble_stop(void)
{
  taskENTER_CRITICAL(&owner_state_mux);
  if (!started) {
    taskEXIT_CRITICAL(&owner_state_mux);
    return 0;
  }
  lifecycle_busy = true;
  bool issue_stop = !host_stop_pending;
  taskEXIT_CRITICAL(&owner_state_mux);
  return stop_host(issue_stop);
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
  bool value = started && synced && !host_stop_pending && !lifecycle_busy;
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
