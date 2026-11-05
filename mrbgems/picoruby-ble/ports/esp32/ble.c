#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/ble.h"
#include "../../include/ble_peripheral.h"

#include "host/ble_hs.h"
#include "syscfg/syscfg.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

#include "ble_common.h"
#include "nimble_owner.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *BLE_TAG = "prb_ble_evq";

#define EVT_BTSTACK_STATE 0x60
#define EVT_DISCONNECTION_COMPLETE 0x05
#define EVT_LE_META 0x3E
#define EVT_ATT_MTU_EXCHANGE_COMPLETE 0xB5
#define EVT_GAP_ADVERTISING_REPORT 0xDA
#define EVT_GATT_QUERY_COMPLETE 0xA0
#define EVT_GATT_SERVICE_QUERY_RESULT 0xA1
#define EVT_GATT_CHARACTERISTIC_QUERY_RESULT 0xA2
#define EVT_GATT_ALL_DESCRIPTORS_QUERY_RESULT 0xA4
#define EVT_GATT_CHARACTERISTIC_VALUE_QUERY_RESULT 0xA5
#define EVT_GATT_NOTIFICATION 0xA7
#define EVT_GATT_INDICATION 0xA8

#define HCI_STATE_OFF 0
#define HCI_STATE_INITIALIZING 1
#define HCI_STATE_WORKING 2
#define HCI_STATE_HALTING 3
#define HCI_STATE_SLEEPING 4
#define HCI_STATE_FALLING_ASLEEP 5

#define ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER 0x02
#define ERROR_CODE_MEMORY_CAPACITY_EXCEEDED 0x07
#define ERROR_CODE_UNSPECIFIED_ERROR 0x1F
#define GATT_CLIENT_BUSY 0x94
#define GATT_CLIENT_IN_WRONG_STATE 0x95
#define GATT_CLIENT_VALUE_TOO_LONG 0x97
#define ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH 0x0D
#define ATT_ERROR_UNLIKELY_ERROR 0x0E
#define ATT_ERROR_HCI_DISCONNECT_RECEIVED 0x1F
#define ATT_ERROR_TIMEOUT 0x7F

#define BLOB_FLAG_READ 0x0002
#define BLOB_FLAG_WRITE_NO_RSP 0x0004
#define BLOB_FLAG_WRITE 0x0008
#define BLOB_FLAG_DYNAMIC 0x0100
#define BLOB_FLAG_LONG_UUID 0x0200
#define BLOB_FLAG_READ_SC 0x0020
#define BLOB_FLAG_WRITE_SC 0x0080
#define BLOB_FLAG_SIGNED_WRITE 0x0040
#define BLOB_FLAG_READ_LEVEL 0x0c00
#define BLOB_FLAG_WRITE_LEVEL 0x0011
#define BLOB_FLAG_KEY_SIZE 0xf000
#define BLOB_FLAG_OPERATIONS (BLOB_FLAG_READ | BLOB_FLAG_WRITE | BLOB_FLAG_WRITE_NO_RSP)

#define GATT_PRIMARY_SERVICE_UUID 0x2800
#define GATT_SECONDARY_SERVICE_UUID 0x2801
#define GATT_CHARACTERISTIC_UUID 0x2803
#define GATT_CCCD_UUID 0x2902

#define GATT_EVENT_PAYLOAD_OFFSET 8
#define VALUE_EVENT_DATA_MAX (PICORUBY_NIMBLE_EVT_MAX - GATT_EVENT_PAYLOAD_OFFSET - 4)

#if PICORUBY_NIMBLE_EVT_MAX != GATT_EVENT_PAYLOAD_OFFSET + 4 + BLE_ATT_MTU_MAX
#error "BLE event capacity does not match the SDK maximum MTU"
#endif

#define RDM_SLOTS 16
#define RDM_MAX BLE_ATT_ATTR_MAX_LEN

static enum BLE_role_t role = BLE_ROLE_NONE;
static bool power_on_requested = false;
static uint8_t power_state = HCI_STATE_OFF;
static uint8_t central_connect_event[21];
static uintptr_t central_connect_token;
static uintptr_t central_connect_serial;

static uint8_t *rdm_pool = NULL;
static uint16_t rdm_len[RDM_SLOTS];
static bool     rdm_valid[RDM_SLOTS];
static bool     rdm_oversize[RDM_SLOTS];
static int      rdm_used = 0;
static portMUX_TYPE rdm_mux = portMUX_INITIALIZER_UNLOCKED;

static void
uuid_to_le128(const ble_uuid_any_t *u, uint8_t out[16])
{
  static const uint8_t base_le[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  };
  switch (u->u.type) {
    case BLE_UUID_TYPE_16:
      memcpy(out, base_le, 16);
      put_le16(out + 12, u->u16.value);
      break;
    case BLE_UUID_TYPE_32:
      memcpy(out, base_le, 16);
      put_le16(out + 12, u->u32.value & 0xffff);
      put_le16(out + 14, u->u32.value >> 16);
      break;
    case BLE_UUID_TYPE_128:
      memcpy(out, u->u128.value, 16);
      break;
    default:
      memset(out, 0, 16);
      break;
  }
}

#define MAX_ATTRS 48
#define MAX_SVCS 8
#define MAX_INCLUDES 8
#define MAX_CHR_SLOTS (24 + MAX_SVCS)
#define MAX_DSC_SLOTS (16 + 24)
#define MAX_UUIDS (MAX_SVCS + 24 + 16)

typedef struct {
  uint16_t ruby_handle;
  uint16_t nimble_handle;
  const uint8_t *static_value;
  uint16_t static_len;
  uint16_t cccd_ruby_handle;
  uint16_t att_flags;
  int16_t mirror_slot;
} attr_map_t;

static attr_map_t attr_map[MAX_ATTRS];
static int attr_map_count = 0;

static struct ble_gatt_svc_def svc_defs[MAX_SVCS + 1];
static const struct ble_gatt_svc_def *include_pool[MAX_INCLUDES + MAX_SVCS];
static int include_pool_used;
static struct ble_gatt_chr_def chr_slots[MAX_CHR_SLOTS];
static struct ble_gatt_dsc_def dsc_slots[MAX_DSC_SLOTS];
static ble_uuid_any_t uuid_pool[MAX_UUIDS];
static int svc_count, chr_slot_count, dsc_slot_count, uuid_count;
static bool have_services = false;

static uint8_t *owned_profile_data = NULL;
static bool conn_sc = false;

uint16_t
picoruby_ble_handle_r2n(uint16_t ruby_handle)
{
  for (int i = 0; i < attr_map_count; i++) {
    if (attr_map[i].ruby_handle == ruby_handle) return attr_map[i].nimble_handle;
  }
  return 0;
}

static attr_map_t *
find_map_by_nimble(uint16_t nimble_handle)
{
  for (int i = 0; i < attr_map_count; i++) {
    if (attr_map[i].nimble_handle == nimble_handle) return &attr_map[i];
  }
  return NULL;
}

static uint8_t att_security_status(uint16_t flags, bool is_read, uint16_t conn);

static uint16_t terminating_handle[MYNEWT_VAL(BLE_MAX_CONNECTIONS)];
static bool terminating_discarded[MYNEWT_VAL(BLE_MAX_CONNECTIONS)];
static int terminating_count;

static bool
terminating_has(uint16_t handle)
{
  for (int i = 0; i < terminating_count; i++) {
    if (terminating_handle[i] == handle) return true;
  }
  return false;
}

static void
terminating_add(uint16_t handle)
{
  if (terminating_has(handle) || terminating_count >= MYNEWT_VAL(BLE_MAX_CONNECTIONS)) return;
  terminating_discarded[terminating_count] = false;
  terminating_handle[terminating_count++] = handle;
}

static void
terminating_discard(uint16_t handle)
{
  terminating_add(handle);
  for (int i = 0; i < terminating_count; i++) {
    if (terminating_handle[i] == handle) terminating_discarded[i] = true;
  }
}

static bool
terminating_discarded_has(uint16_t handle)
{
  for (int i = 0; i < terminating_count; i++) {
    if (terminating_handle[i] == handle) return terminating_discarded[i];
  }
  return false;
}

static bool
terminating_remove(uint16_t handle)
{
  for (int i = 0; i < terminating_count; i++) {
    if (terminating_handle[i] != handle) continue;
    int last = --terminating_count;
    terminating_handle[i] = terminating_handle[last];
    terminating_discarded[i] = terminating_discarded[last];
    return true;
  }
  return false;
}

static uint16_t known_link[MYNEWT_VAL(BLE_MAX_CONNECTIONS)];
static int known_link_count;

static void
known_link_add(uint16_t handle)
{
  for (int i = 0; i < known_link_count; i++) {
    if (known_link[i] == handle) return;
  }
  if (known_link_count >= MYNEWT_VAL(BLE_MAX_CONNECTIONS)) {
    ESP_LOGW(BLE_TAG, "link table full: handle=%u", (unsigned)handle);
    return;
  }
  known_link[known_link_count++] = handle;
}

static void
known_link_remove(uint16_t handle)
{
  for (int i = 0; i < known_link_count; i++) {
    if (known_link[i] != handle) continue;
    known_link[i] = known_link[--known_link_count];
    return;
  }
}

void
picoruby_ble_known_links_clear(void)
{
  if (!picoruby_ble_connection_gate_enter()) return;
  known_link_count = 0;
  picoruby_ble_connection_gate_exit();
}

static void
peripheral_adopt(uint16_t handle)
{
  if (role != BLE_ROLE_PERIPHERAL || handle == 0xffff || terminating_has(handle)) return;
  if (picoruby_ble_connection_handle_snapshot() != 0xffff) return;
  if (ble_gap_conn_find(handle, NULL) != 0) return;
  if (power_state == HCI_STATE_OFF || power_state == HCI_STATE_SLEEPING) {
    int rc = ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
      terminating_discard(handle);
      known_link_add(handle);
    } else if (rc != BLE_HS_ENOTCONN) {
      ESP_LOGE(BLE_TAG, "late connection termination failed: %d", rc);
      picoruby_nimble_cancel_control(PICORUBY_NIMBLE_CONTROL_CONNECTION_LOST);
    }
    return;
  }
  if (!picoruby_ble_connection_claim(handle)) return;
  known_link_add(handle);
  conn_sc = false;
  picoruby_ble_central_reset_request();
}

static int
gatt_access_cb(uint16_t conn, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
  if (!picoruby_ble_connection_gate_enter()) {
    ESP_LOGW(BLE_TAG, "ATT access rejected: connection gate unavailable");
    return BLE_ATT_ERR_UNLIKELY;
  }
  peripheral_adopt(conn);
  bool matching = picoruby_ble_connection_matches(conn);
  picoruby_ble_connection_gate_exit();
  if (!matching) {
    ESP_LOGW(BLE_TAG, "ATT access rejected: no matching connection: conn=%u handle=%u op=%u",
             (unsigned)conn, (unsigned)attr_handle, (unsigned)ctxt->op);
    return BLE_ATT_ERR_UNLIKELY;
  }
  attr_map_t *e = (attr_map_t *)arg;
  if (e == NULL) {
    ESP_LOGW(BLE_TAG, "ATT access rejected: no attribute map: handle=%u op=%u",
             (unsigned)attr_handle, (unsigned)ctxt->op);
    return BLE_ATT_ERR_UNLIKELY;
  }

  switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
    case BLE_GATT_ACCESS_OP_READ_DSC: {
      if (!(e->att_flags & BLOB_FLAG_READ)) return BLE_ATT_ERR_READ_NOT_PERMITTED;
      uint8_t read_sec = att_security_status(e->att_flags, true, conn);
      if (read_sec) return read_sec;
      static uint8_t rd_tmp[RDM_MAX];
      const uint8_t *data = e->static_value;
      uint16_t len = e->static_len;
      if (e->mirror_slot >= 0) {
        uint16_t n;
        bool valid;
        bool oversize;
        taskENTER_CRITICAL(&rdm_mux);
        n = rdm_len[e->mirror_slot];
        valid = rdm_valid[e->mirror_slot];
        oversize = rdm_oversize[e->mirror_slot];
        if (n > 0) memcpy(rd_tmp, rdm_pool + (size_t)e->mirror_slot * RDM_MAX, n);
        taskEXIT_CRITICAL(&rdm_mux);
        if (oversize) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (valid) {
          data = rd_tmp;
          len = n;
        } else {
          data = NULL;
          len = 0;
        }
      }
      if (data == NULL || len == 0) return 0;
      if (os_mbuf_append(ctxt->om, data, len) == 0) return 0;
      ESP_LOGW(BLE_TAG, "read response append failed: handle=%u len=%u",
               (unsigned)e->ruby_handle, (unsigned)len);
      return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
    case BLE_GATT_ACCESS_OP_WRITE_DSC: {
      if (!(e->att_flags & BLOB_FLAG_DYNAMIC) ||
          !(e->att_flags & (BLOB_FLAG_WRITE | BLOB_FLAG_WRITE_NO_RSP))) {
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
      }
      uint8_t write_sec = att_security_status(e->att_flags, false, conn);
      if (write_sec) return write_sec;
      static uint8_t buf[PICORUBY_NIMBLE_WRITE_MAX];
      uint16_t out_len = 0;
      if (OS_MBUF_PKTLEN(ctxt->om) == 0) return 0;
      if (OS_MBUF_PKTLEN(ctxt->om) > sizeof(buf)) {
        ESP_LOGW(BLE_TAG, "write exceeds capacity: handle=%u len=%u",
                 (unsigned)e->ruby_handle, (unsigned)OS_MBUF_PKTLEN(ctxt->om));
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      }
      if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &out_len) != 0) {
        ESP_LOGW(BLE_TAG, "write mbuf copy failed: handle=%u", (unsigned)e->ruby_handle);
        return BLE_ATT_ERR_UNLIKELY;
      }
      if (picoruby_nimble_enqueue_write(e->ruby_handle, buf, out_len) != 0) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
      }
      return 0;
    }
    default:
      return BLE_ATT_ERR_UNLIKELY;
  }
}

static void
mirror_store(int slot, uint16_t ruby_handle, const uint8_t *data, uint16_t size)
{
  bool oversize = size > RDM_MAX;
  taskENTER_CRITICAL(&rdm_mux);
  if (!oversize && size > 0) memcpy(rdm_pool + (size_t)slot * RDM_MAX, data, size);
  rdm_len[slot] = oversize ? 0 : size;
  rdm_valid[slot] = true;
  bool newly_oversize = oversize && !rdm_oversize[slot];
  rdm_oversize[slot] = oversize;
  taskEXIT_CRITICAL(&rdm_mux);
  if (newly_oversize) {
    ESP_LOGW(BLE_TAG, "read value exceeds mirror capacity: handle=%u len=%u",
             (unsigned)ruby_handle, (unsigned)size);
  }
}

void
BLE_read_value_pushed(uint16_t att_handle, const uint8_t *data, uint16_t size)
{
  for (int i = 0; i < attr_map_count; i++) {
    attr_map_t *e = &attr_map[i];
    if (e->ruby_handle != att_handle || e->mirror_slot < 0) continue;
    mirror_store(e->mirror_slot, att_handle, data, size);
    return;
  }
}

void
picoruby_ble_refresh_read_mirrors(void)
{
  static uint8_t tmp[RDM_MAX];
  for (int i = 0; i < attr_map_count; i++) {
    attr_map_t *e = &attr_map[i];
    if (e->mirror_slot < 0) continue;
    BLE_read_value_t rv = { .att_handle = e->ruby_handle, .data = NULL, .size = 0 };
    if (BLE_read_data(&rv) != 0 || rv.data == NULL) continue;
    uint16_t n = rv.size;
    if (n <= RDM_MAX) memcpy(tmp, rv.data, n);
    mirror_store(e->mirror_slot, e->ruby_handle, tmp, n);
  }
}

void
picoruby_ble_gatts_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
  (void)arg;
  if (ctxt->op == BLE_GATT_REGISTER_OP_DSC && ctxt->dsc.dsc_def->arg) {
    ((attr_map_t *)ctxt->dsc.dsc_def->arg)->nimble_handle = ctxt->dsc.handle;
  }
}

static ble_gatt_chr_flags
props_to_chr_flags(uint8_t props)
{
  ble_gatt_chr_flags f = 0;
  if (props & 0x01) f |= BLE_GATT_CHR_F_BROADCAST;
  if (props & 0x02) f |= BLE_GATT_CHR_F_READ;
  if (props & 0x04) f |= BLE_GATT_CHR_F_WRITE_NO_RSP;
  if (props & 0x08) f |= BLE_GATT_CHR_F_WRITE;
  if (props & 0x10) f |= BLE_GATT_CHR_F_NOTIFY;
  if (props & 0x20) f |= BLE_GATT_CHR_F_INDICATE;
  if (props & 0x40) f |= BLE_GATT_CHR_F_AUTH_SIGN_WRITE;
  if (props & 0x80) f |= BLE_GATT_CHR_F_RELIABLE_WRITE;
  return f;
}

static unsigned
profile_read_level(uint16_t flags)
{
  return (flags & BLOB_FLAG_READ_LEVEL) >> 10;
}

static unsigned
profile_write_level(uint16_t flags)
{
  return (flags & 0x0001) | ((flags & 0x0010) >> 3);
}

static uint8_t
profile_min_key_size(uint16_t flags)
{
  unsigned encoded = (flags & BLOB_FLAG_KEY_SIZE) >> 12;
  return encoded ? (uint8_t)(encoded + 1) : 0;
}

static uint8_t
att_security_status(uint16_t flags, bool is_read, uint16_t conn)
{
  unsigned level = is_read ? profile_read_level(flags) : profile_write_level(flags);
  if (level == 0) return 0;
  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(conn, &desc) != 0) return BLE_ATT_ERR_UNLIKELY;
  uint8_t key_required = profile_min_key_size(flags);
  uint8_t key_size = desc.sec_state.encrypted ? desc.sec_state.key_size : 0;
  bool sc_missing = (flags & (is_read ? BLOB_FLAG_READ_SC : BLOB_FLAG_WRITE_SC)) &&
                    !(desc.sec_state.encrypted && conn_sc);
  if (level == 3) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
  if (level == 2 && (!desc.sec_state.authenticated || sc_missing)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
  if (key_required && (key_size == 0 || sc_missing)) return BLE_ATT_ERR_INSUFFICIENT_ENC;
  if (key_required > key_size) return BLE_ATT_ERR_INSUFFICIENT_KEY_SZ;
  return 0;
}

static uint8_t
profile_dsc_flags(uint16_t flags)
{
  uint8_t out = 0;
  if (flags & BLOB_FLAG_READ) out |= BLE_ATT_F_READ;
  if (flags & (BLOB_FLAG_WRITE | BLOB_FLAG_WRITE_NO_RSP)) out |= BLE_ATT_F_WRITE;
  return out;
}

static unsigned
profile_effective_level(unsigned level, uint8_t key)
{
  return level >= 2 ? level : (level == 1 && key ? 1 : 0);
}

static ble_gatt_chr_flags
profile_cccd_security(uint16_t flags)
{
  ble_gatt_chr_flags out = 0;
#if defined(BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC) && \
    defined(BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN) && \
    defined(BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR)
  unsigned wr = profile_effective_level(profile_write_level(flags), profile_min_key_size(flags));
  if (wr >= 1) out |= BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC;
  if (wr >= 2) out |= BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN;
  if (wr >= 3) out |= BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR;
#else
  (void)flags;
#endif
  return out;
}

static uint16_t profile_reject_handle;
static const char *profile_reject_reason;
#define PROFILE_REJECT(h, why) do { profile_reject_handle = (h); profile_reject_reason = (why); return -1; } while (0)

static int
profile_permissions_supported(uint16_t flags)
{
  uint16_t writes = flags & (BLOB_FLAG_WRITE | BLOB_FLAG_WRITE_NO_RSP);
  if ((flags & BLOB_FLAG_SIGNED_WRITE) && !writes) return -1;
  return 0;
}

static int
profile_cccd_supported(uint16_t flags)
{
  if (!(flags & BLOB_FLAG_DYNAMIC) || profile_permissions_supported(flags) < 0) return -1;
  if ((flags & BLOB_FLAG_OPERATIONS) != BLOB_FLAG_OPERATIONS ||
      profile_effective_level(profile_read_level(flags), profile_min_key_size(flags))) return -1;
#if !defined(BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC) || \
    !defined(BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN) || \
    !defined(BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR)
  if (profile_effective_level(profile_write_level(flags), profile_min_key_size(flags))) return -1;
#endif
  return 0;
}

static const ble_uuid_t *
alloc_uuid(const uint8_t *buf, uint16_t len)
{
  if (uuid_count >= MAX_UUIDS) return NULL;
  ble_uuid_any_t *u = &uuid_pool[uuid_count];
  if (ble_uuid_init_from_buf(u, buf, len) != 0) return NULL;
  uuid_count++;
  return &u->u;
}

static attr_map_t *
alloc_map(uint16_t ruby_handle)
{
  if (ruby_handle == 0 || attr_map_count >= MAX_ATTRS) return NULL;
  for (int i = 0; i < attr_map_count; i++) {
    if (attr_map[i].ruby_handle == ruby_handle) return NULL;
  }
  attr_map_t *e = &attr_map[attr_map_count++];
  memset(e, 0, sizeof(*e));
  e->ruby_handle = ruby_handle;
  e->mirror_slot = -1;
  return e;
}

static struct ble_gatt_svc_def *
alloc_svc(void)
{
  if (svc_count >= MAX_SVCS) return NULL;
  return &svc_defs[svc_count++];
}

static struct ble_gatt_chr_def *
alloc_chr(void)
{
  if (chr_slot_count >= MAX_CHR_SLOTS) return NULL;
  return &chr_slots[chr_slot_count++];
}

static struct ble_gatt_dsc_def *
alloc_dsc(void)
{
  if (dsc_slot_count >= MAX_DSC_SLOTS) return NULL;
  return &dsc_slots[dsc_slot_count++];
}

static int
mirror_assign(attr_map_t *e, uint16_t flags)
{
  const uint16_t need = BLOB_FLAG_DYNAMIC | BLOB_FLAG_READ;
  if (e == NULL || (flags & need) != need) return 0;
  if (rdm_used >= RDM_SLOTS) {
    ESP_LOGE(BLE_TAG,
             "read mirror slots exhausted (RDM_SLOTS=%d); ruby_handle=%u cannot be initialized",
             RDM_SLOTS, (unsigned)e->ruby_handle);
    return -1;
  }
  e->mirror_slot = (int16_t)rdm_used++;
  rdm_len[e->mirror_slot] = 0;
  rdm_valid[e->mirror_slot] = false;
  rdm_oversize[e->mirror_slot] = false;
  return 0;
}

static uint16_t
profile_reserved_uuid16(const uint8_t *uuid, uint16_t uuid_len)
{
  uint16_t u16;
  if (uuid_len == 2) {
    u16 = get_le16(uuid);
  } else {
    static const uint8_t base_prefix[12] = {
      0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00,
      0x00, 0x80, 0x00, 0x10, 0x00, 0x00
    };
    if (uuid_len != 16 || memcmp(uuid, base_prefix, sizeof(base_prefix)) != 0 ||
        uuid[14] != 0 || uuid[15] != 0) return 0;
    u16 = get_le16(uuid + 12);
  }
  switch (u16) {
    case GATT_PRIMARY_SERVICE_UUID:
    case GATT_SECONDARY_SERVICE_UUID:
    case 0x2802:
    case GATT_CHARACTERISTIC_UUID:
    case GATT_CCCD_UUID:
      return u16;
    default:
      return 0;
  }
}

typedef struct {
  uint16_t decl_handle;
  uint16_t last_handle;
  uint16_t uuid_len;
  const uint8_t *uuid;
} svc_info_t;

static int
include_target(const svc_info_t *svcs, int current, const uint8_t *value, uint16_t value_len, const char **reason)
{
  uint16_t start = get_le16(value);
  uint16_t end = get_le16(value + 2);
  for (int i = 0; i < current; i++) {
    if (svcs[i].decl_handle != start) continue;
    if (end != svcs[i].last_handle) {
      *reason = "include end handle does not match the included service";
      return -1;
    }
    bool uuid16 = svcs[i].uuid_len == 2;
    if (value_len != (uuid16 ? 6 : 4)) {
      *reason = "include record length does not match the included service UUID";
      return -1;
    }
    if (uuid16 && memcmp(value + 4, svcs[i].uuid, 2) != 0) {
      *reason = "include UUID does not match the included service";
      return -1;
    }
    return i;
  }
  *reason = "include references an unknown or later service";
  return -1;
}

static int
validate_att_topology(const uint8_t *db, size_t db_len)
{
  if (BLE_profile_extent(db, db_len, &db_len) < 0) PROFILE_REJECT(0, "malformed profile blob");
  if (db == NULL) return 0;
  int services = 0, chars = 0, descs = 0, maps = 0, uuids = 0, mirrors = 0, cccds = 0, includes = 0;
  bool in_char = false, has_descs = false, has_cccd = false, after_service = false;
  svc_info_t svcs[MAX_SVCS] = { { 0 } };
  uint8_t props = 0;
  uint16_t previous_handle = 0, expected_handle = 0, expected_uuid_len = 0;
  const uint8_t *expected_uuid = NULL;
  size_t pos = 1;
  while (pos + 2 <= db_len) {
    uint16_t size = get_le16(db + pos);
    if (size == 0) break;
    uint16_t flags = get_le16(db + pos + 2);
    uint16_t handle = get_le16(db + pos + 4);
    uint16_t uuid_len = (flags & BLOB_FLAG_LONG_UUID) ? 16 : 2;
    const uint8_t *uuid = db + pos + 6;
    const uint8_t *value = uuid + uuid_len;
    uint16_t value_len = size - 6 - uuid_len;
    uint16_t u16 = profile_reserved_uuid16(uuid, uuid_len);
    pos += size;
    if (handle == 0 || handle <= previous_handle) PROFILE_REJECT(handle, "handle is zero or not increasing");
    previous_handle = handle;

    if (expected_handle) {
      if (handle != expected_handle || uuid_len != expected_uuid_len ||
          memcmp(uuid, expected_uuid, uuid_len) != 0) PROFILE_REJECT(handle, "value does not match its declaration");
      if (profile_permissions_supported(flags) < 0) PROFILE_REJECT(handle, "unsupported value permissions");
      if (flags & ~props & (BLOB_FLAG_OPERATIONS | BLOB_FLAG_SIGNED_WRITE)) PROFILE_REJECT(handle, "value flags exceed declared properties");
      expected_handle = 0;
      if ((flags & (BLOB_FLAG_DYNAMIC | BLOB_FLAG_READ)) ==
          (BLOB_FLAG_DYNAMIC | BLOB_FLAG_READ)) mirrors++;
    } else if (u16 == GATT_PRIMARY_SERVICE_UUID || u16 == GATT_SECONDARY_SERVICE_UUID) {
      if (value_len != 2 && value_len != 4 && value_len != 16) PROFILE_REJECT(handle, "bad service UUID length");
      if ((flags & BLOB_FLAG_OPERATIONS) != BLOB_FLAG_READ ||
          (flags & (BLOB_FLAG_READ_LEVEL | BLOB_FLAG_WRITE_LEVEL | BLOB_FLAG_KEY_SIZE |
                    BLOB_FLAG_READ_SC | BLOB_FLAG_WRITE_SC | BLOB_FLAG_SIGNED_WRITE))) PROFILE_REJECT(handle, "unsupported service record flags");
      if (has_descs) descs++;
      if (services < MAX_SVCS) svcs[services] = (svc_info_t){ handle, handle, value_len, value };
      services++;
      uuids++;
      chars++;
      in_char = has_descs = has_cccd = false;
      after_service = true;
    } else if (u16 == GATT_CHARACTERISTIC_UUID) {
      if (services == 0) PROFILE_REJECT(handle, "characteristic outside a service");
      if (value_len != 5 && value_len != 19) PROFILE_REJECT(handle, "bad characteristic declaration length");
      if (has_descs) descs++;
      has_descs = has_cccd = false;
      if ((flags & BLOB_FLAG_OPERATIONS) != BLOB_FLAG_READ ||
          (flags & (BLOB_FLAG_READ_LEVEL | BLOB_FLAG_WRITE_LEVEL | BLOB_FLAG_KEY_SIZE |
                    BLOB_FLAG_READ_SC | BLOB_FLAG_WRITE_SC | BLOB_FLAG_SIGNED_WRITE))) PROFILE_REJECT(handle, "unsupported declaration record flags");
      props = value[0];
      expected_handle = get_le16(value + 1);
      if (expected_handle <= handle) PROFILE_REJECT(handle, "value handle does not follow its declaration");
      expected_uuid = value + 3;
      expected_uuid_len = value_len - 3;
      in_char = true;
      after_service = false;
      chars++;
      maps++;
      uuids++;
    } else if (u16 == 0x2802) {
      if (!after_service) PROFILE_REJECT(handle, "include declaration must follow its service declaration");
      if (value_len != 4 && value_len != 6) PROFILE_REJECT(handle, "bad include declaration length");
      if ((flags & BLOB_FLAG_OPERATIONS) != BLOB_FLAG_READ ||
          (flags & (BLOB_FLAG_READ_LEVEL | BLOB_FLAG_WRITE_LEVEL | BLOB_FLAG_KEY_SIZE |
                    BLOB_FLAG_READ_SC | BLOB_FLAG_WRITE_SC | BLOB_FLAG_SIGNED_WRITE))) PROFILE_REJECT(handle, "unsupported include record flags");
      const char *include_reason = NULL;
      if (include_target(svcs, services - 1, value, value_len, &include_reason) < 0) PROFILE_REJECT(handle, include_reason);
      includes++;
    } else {
      if (!in_char) PROFILE_REJECT(handle, "descriptor outside a characteristic");
      if (u16 == GATT_CCCD_UUID) {
        if (has_cccd || !(props & 0x30) || value_len != 2 ||
            value[0] != 0 || value[1] != 0) PROFILE_REJECT(handle, "CCCD form unsupported");
        if (profile_cccd_supported(flags) < 0) PROFILE_REJECT(handle, "CCCD flags unsupported");
        has_cccd = true;
        cccds++;
      } else {
        if (profile_permissions_supported(flags) < 0) PROFILE_REJECT(handle, "unsupported descriptor permissions");
        has_descs = true;
        descs++;
        maps++;
        uuids++;
        if ((flags & (BLOB_FLAG_DYNAMIC | BLOB_FLAG_READ)) ==
            (BLOB_FLAG_DYNAMIC | BLOB_FLAG_READ)) mirrors++;
      }
    }
    if (services > 0 && services <= MAX_SVCS) svcs[services - 1].last_handle = handle;
    if (services > MAX_SVCS || includes > MAX_INCLUDES || chars > MAX_CHR_SLOTS ||
        descs > MAX_DSC_SLOTS || maps > MAX_ATTRS ||
        uuids > MAX_UUIDS || mirrors > RDM_SLOTS ||
        cccds >= PICORUBY_NIMBLE_WRITE_DEPTH) PROFILE_REJECT(handle, "profile exceeds a fixed capacity");
  }
  if (expected_handle) PROFILE_REJECT(previous_handle, "declaration without its value");
  if (has_descs) descs++;
  if (descs > MAX_DSC_SLOTS) PROFILE_REJECT(previous_handle, "descriptor slots exhausted");
  return 0;
}

static uint16_t
profile_layout_mismatch(const uint8_t *db, size_t db_len)
{
  if (db == NULL) return 0;
  uint16_t next = 1, cccd_slot = 0, value_handle = 0, expected_value = 0;
  bool pending = false;
  uint8_t props = 0;
  size_t pos = 1;
  while (pos + 2 <= db_len) {
    uint16_t size = get_le16(db + pos);
    if (size == 0) break;
    uint16_t flags = get_le16(db + pos + 2);
    uint16_t handle = get_le16(db + pos + 4);
    uint16_t uuid_len = (flags & BLOB_FLAG_LONG_UUID) ? 16 : 2;
    const uint8_t *uuid = db + pos + 6;
    const uint8_t *value = uuid + uuid_len;
    uint16_t value_len = size - 6 - uuid_len;
    uint16_t u16 = profile_reserved_uuid16(uuid, uuid_len);
    pos += size;
    if (uuid_len == 16 && u16) return handle;
    if (expected_value) {
      if (handle != next) return handle;
      expected_value = 0;
      next++;
      if (props & 0x30) {
        pending = true;
        value_handle = handle;
        cccd_slot = next++;
      }
    } else if (u16 == GATT_CCCD_UUID) {
      if (!pending || handle != cccd_slot) return handle;
      pending = false;
    } else {
      if (handle != next) return handle;
      if (pending) return value_handle;
      next++;
      if ((u16 == GATT_PRIMARY_SERVICE_UUID || u16 == GATT_SECONDARY_SERVICE_UUID) && value_len == 4) return handle;
      if (u16 == GATT_CHARACTERISTIC_UUID) {
        props = value[0];
        expected_value = get_le16(value + 1);
      }
    }
  }
  return pending ? value_handle : 0;
}

static int
build_att_db(const uint8_t *db, size_t db_len)
{
  if (BLE_profile_extent(db, db_len, &db_len) < 0) return -1;
  attr_map_count = 0;
  rdm_used = 0;
  svc_count = chr_slot_count = dsc_slot_count = uuid_count = 0;
  include_pool_used = 0;
  memset(svc_defs, 0, sizeof(svc_defs));
  memset(include_pool, 0, sizeof(include_pool));
  memset(chr_slots, 0, sizeof(chr_slots));
  memset(dsc_slots, 0, sizeof(dsc_slots));

  svc_info_t svcs[MAX_SVCS] = { { 0 } };
  bool include_open = false;
  struct ble_gatt_svc_def *cur_svc = NULL;
  struct ble_gatt_chr_def *cur_chr = NULL;
  attr_map_t *cur_chr_map = NULL;
  uint16_t expected_value_handle = 0;
  const uint8_t *cur_chr_uuid = NULL;
  uint16_t cur_chr_uuid_len = 0;

  size_t pos = 1;
  while (pos + 2 <= db_len) {
    uint16_t entry_size = get_le16(db + pos);
    if (entry_size == 0) break;
    if (entry_size < 8 || entry_size > db_len - pos) return -1;
    uint16_t flags = get_le16(db + pos + 2);
    uint16_t handle = get_le16(db + pos + 4);
    uint16_t uuid_len = (flags & BLOB_FLAG_LONG_UUID) ? 16 : 2;
    if (entry_size < 6 + uuid_len) {
      return -1;
    }
    const uint8_t *uuid = db + pos + 6;
    const uint8_t *value = uuid + uuid_len;
    uint16_t value_len = entry_size - 6 - uuid_len;
    uint16_t u16 = profile_reserved_uuid16(uuid, uuid_len);
    pos += entry_size;

    if (expected_value_handle) {
      if (!cur_chr_map || handle != expected_value_handle || uuid_len != cur_chr_uuid_len ||
          memcmp(uuid, cur_chr_uuid, uuid_len) != 0) return -1;
      cur_chr_map->att_flags = flags;
      cur_chr_map->static_value = value;
      cur_chr_map->static_len = value_len;
      if (mirror_assign(cur_chr_map, flags) != 0) return -1;
      expected_value_handle = 0;
    } else if (u16 == GATT_PRIMARY_SERVICE_UUID || u16 == GATT_SECONDARY_SERVICE_UUID) {
      if (cur_chr && cur_chr->descriptors && alloc_dsc() == NULL) return -1;
      cur_chr = NULL;
      cur_chr_map = NULL;
      if (cur_svc && alloc_chr() == NULL) return -1;
      cur_svc = alloc_svc();
      if (cur_svc == NULL) return -1;
      cur_svc->type = (u16 == GATT_PRIMARY_SERVICE_UUID)
                        ? BLE_GATT_SVC_TYPE_PRIMARY : BLE_GATT_SVC_TYPE_SECONDARY;
      cur_svc->uuid = alloc_uuid(value, value_len);
      cur_svc->characteristics = &chr_slots[chr_slot_count];
      if (cur_svc->uuid == NULL) return -1;
      if (include_open) {
        include_pool_used++;
        include_open = false;
      }
      svcs[svc_count - 1] = (svc_info_t){ handle, handle, value_len, value };
    } else if (u16 == 0x2802 && cur_svc && !cur_chr) {
      const char *include_reason = NULL;
      if (value_len < 4) return -1;
      int target = include_target(svcs, svc_count - 1, value, value_len, &include_reason);
      if (target < 0 || include_pool_used >= MAX_INCLUDES + MAX_SVCS - 1) return -1;
      if (!include_open) {
        cur_svc->includes = &include_pool[include_pool_used];
        include_open = true;
      }
      include_pool[include_pool_used++] = &svc_defs[target];
    } else if (u16 == GATT_CHARACTERISTIC_UUID && cur_svc) {
      if (cur_chr && cur_chr->descriptors && alloc_dsc() == NULL) return -1;
      if (value_len < 3) return -1;
      uint8_t props = value[0];
      expected_value_handle = get_le16(value + 1);
      cur_chr_uuid = value + 3;
      cur_chr_uuid_len = value_len - 3;
      cur_chr = alloc_chr();
      if (cur_chr == NULL) return -1;
      cur_chr->uuid = alloc_uuid(value + 3, value_len - 3);
      cur_chr->access_cb = gatt_access_cb;
      cur_chr->flags = props_to_chr_flags(props);
      cur_chr_map = alloc_map(expected_value_handle);
      if (cur_chr->uuid == NULL || cur_chr_map == NULL) return -1;
      cur_chr->arg = cur_chr_map;
      cur_chr->val_handle = &cur_chr_map->nimble_handle;
    } else if (u16 == GATT_CCCD_UUID && cur_chr_map) {
      cur_chr->flags |= profile_cccd_security(flags);
      cur_chr_map->cccd_ruby_handle = handle;
    } else if (cur_chr) {
      struct ble_gatt_dsc_def *dsc = alloc_dsc();
      attr_map_t *m = alloc_map(handle);
      if (dsc == NULL || m == NULL) return -1;
      m->att_flags = flags;
      m->static_value = value;
      m->static_len = value_len;
      if (mirror_assign(m, flags) != 0) return -1;
      if (cur_chr->descriptors == NULL) cur_chr->descriptors = dsc;
      dsc->uuid = alloc_uuid(uuid, uuid_len);
      dsc->att_flags = profile_dsc_flags(flags);
      dsc->access_cb = gatt_access_cb;
      dsc->arg = m;
      if (dsc->uuid == NULL) return -1;
    }
    if (svc_count > 0) svcs[svc_count - 1].last_handle = handle;
  }

  if (cur_chr && cur_chr->descriptors && alloc_dsc() == NULL) return -1;
  if (cur_svc && alloc_chr() == NULL) return -1;
  have_services = (svc_count > 0);
  return 0;
}

static int
parse_att_db(const uint8_t *db, size_t db_len)
{
  if (validate_att_topology(db, db_len) < 0) return -1;
  if (db == NULL) return -1;
  if (build_att_db(db, db_len) == 0) return 0;
  attr_map_count = rdm_used = 0;
  svc_count = chr_slot_count = dsc_slot_count = uuid_count = 0;
  include_pool_used = 0;
  have_services = false;
  memset(attr_map, 0, sizeof(attr_map));
  memset(svc_defs, 0, sizeof(svc_defs));
  memset(include_pool, 0, sizeof(include_pool));
  memset(chr_slots, 0, sizeof(chr_slots));
  memset(dsc_slots, 0, sizeof(dsc_slots));
  memset(uuid_pool, 0, sizeof(uuid_pool));
  memset(rdm_len, 0, sizeof(rdm_len));
  memset(rdm_valid, 0, sizeof(rdm_valid));
  memset(rdm_oversize, 0, sizeof(rdm_oversize));
  return -1;
}

static void reset_characteristic_discoveries(void);

static int
register_services(void)
{
  if (picoruby_ble_peripheral_setup() != 0) return -1;
  reset_characteristic_discoveries();
  picoruby_ble_central_reset_request();
  picoruby_ble_central_scan_reset();
  power_state = HCI_STATE_OFF;
  if (!have_services) return 0;
  int rc = ble_gatts_count_cfg(svc_defs);
  if (rc == 0) rc = ble_gatts_add_svcs(svc_defs);
  return rc;
}

static void
enqueue_control_event(const uint8_t *pkt, uint16_t len)
{
  if (picoruby_nimble_enqueue_event(pkt, len, PICORUBY_NIMBLE_EVENT_CONTROL) != PICORUBY_NIMBLE_EVENT_ACCEPTED) {
    picoruby_nimble_cancel_control(PICORUBY_NIMBLE_CONTROL_WORK_LOST);
  }
}

static void
synth_state(uint8_t state)
{
  uint8_t p[3] = { EVT_BTSTACK_STATE, 1, state };
  enqueue_control_event(p, sizeof(p));
}

bool
picoruby_ble_power_working(void)
{
  return power_state == HCI_STATE_WORKING;
}

bool
picoruby_ble_role_peripheral(void)
{
  return role == BLE_ROLE_PERIPHERAL;
}

static void
gatt_event_header(uint8_t *p, uint8_t type, uint8_t len, uint16_t conn)
{
  p[0] = type;
  p[1] = len;
  put_le16(p + 2, conn);
  memset(p + 4, 0, GATT_EVENT_PAYLOAD_OFFSET - 4);
}

static void
synth_query_complete(uint16_t conn, uint8_t status)
{
  uint8_t p[GATT_EVENT_PAYLOAD_OFFSET + 1];
  gatt_event_header(p, EVT_GATT_QUERY_COMPLETE, sizeof(p) - 2, conn);
  p[GATT_EVENT_PAYLOAD_OFFSET] = status;
  enqueue_control_event(p, sizeof(p));
}

static uint8_t
status_to_u8(uint16_t status)
{
  if (status == 0 || status == BLE_HS_EDONE) return 0;
  if (status < BLE_HS_ERR_ATT_BASE) return picoruby_ble_request_status(status);
  return (status & 0xff) ? (uint8_t)status : 0xff;
}

static void
build_le_connection_complete(uint8_t *p, uint16_t handle, uint8_t status, const struct ble_gap_conn_desc *desc)
{
  memset(p, 0, 21);
  p[0] = EVT_LE_META;
  p[1] = 19;
  p[2] = 0x01;
  p[3] = status;
  put_le16(p + 4, handle);
  if (desc != NULL) {
    p[6] = (desc->role == BLE_GAP_ROLE_MASTER) ? 0 : 1;
    p[7] = desc->peer_id_addr.type;
    memcpy(p + 8, desc->peer_id_addr.val, 6);
    put_le16(p + 14, desc->conn_itvl);
    put_le16(p + 16, desc->conn_latency);
    put_le16(p + 18, desc->supervision_timeout);
  }
}

static void
synth_le_connection_complete(uint16_t handle, uint8_t status)
{
  uint8_t p[21];
  build_le_connection_complete(p, handle, status, NULL);
  enqueue_control_event(p, sizeof(p));
}

static void
synth_disconnection_complete(uint16_t handle, uint8_t reason)
{
  uint8_t p[6] = { EVT_DISCONNECTION_COMPLETE, 4, 0, 0, 0, reason };
  put_le16(p + 3, handle);
  enqueue_control_event(p, sizeof(p));
}

static void
synth_mtu_exchange_complete(uint16_t conn, uint16_t mtu)
{
  uint8_t p[6] = { EVT_ATT_MTU_EXCHANGE_COMPLETE, 4, 0, 0, 0, 0 };
  put_le16(p + 2, conn);
  put_le16(p + 4, mtu);
  picoruby_nimble_enqueue_event(p, sizeof(p), PICORUBY_NIMBLE_EVENT_DATA);
}

static void
synth_advertising_report(const struct ble_gap_disc_desc *d)
{
  if (d->length_data > 31) {
    ESP_LOGW(BLE_TAG, "advertising report exceeds 31 bytes: %u", (unsigned)d->length_data);
    return;
  }
  uint8_t p[14 + 31];
  uint8_t dlen = d->length_data;
  p[0] = EVT_GAP_ADVERTISING_REPORT;
  p[2] = d->event_type;
  p[3] = d->addr.type;
  memcpy(p + 4, d->addr.val, 6);
  p[10] = (uint8_t)d->rssi;
  p[11] = dlen;
  if (dlen) memcpy(p + 12, d->data, dlen);
  uint16_t total_len = 12 + dlen;
  if (total_len < 14) {
    memset(p + total_len, 0, 14 - total_len);
    total_len = 14;
  }
  p[1] = (uint8_t)(total_len - 2);
  picoruby_nimble_enqueue_event(p, total_len, PICORUBY_NIMBLE_EVENT_DATA);
}

static int
central_mtu_cb(uint16_t conn, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
  (void)mtu;
  if (error != NULL && error->status != 0) {
    ESP_LOGW(BLE_TAG, "MTU exchange failed: conn=%u status=%u", (unsigned)conn, (unsigned)error->status);
  }
  if (central_connect_token != 0 && (uintptr_t)arg == central_connect_token &&
      picoruby_ble_connection_matches(conn)) {
    central_connect_token = 0;
    enqueue_control_event(central_connect_event, sizeof(central_connect_event));
  }
  return 0;
}

int
picoruby_ble_gap_event(struct ble_gap_event *event, void *arg)
{
  (void)arg;
  int rc = 0;
  if (!picoruby_ble_connection_gate_enter()) return 0;
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == BLE_HS_EAGAIN) {
        known_link_remove(event->connect.conn_handle);
      } else if (event->connect.conn_handle != BLE_HS_CONN_HANDLE_NONE &&
                 ble_gap_conn_find(event->connect.conn_handle, NULL) == 0) {
        known_link_add(event->connect.conn_handle);
      }
      if (role == BLE_ROLE_PERIPHERAL && picoruby_ble_connection_matches(event->connect.conn_handle)) {
        if (event->connect.status == BLE_HS_EAGAIN ||
            ble_gap_conn_find(event->connect.conn_handle, NULL) != 0) {
          picoruby_ble_connection_release(event->connect.conn_handle);
          conn_sc = false;
          picoruby_ble_central_reset_request();
          synth_disconnection_complete(event->connect.conn_handle,
                                       terminating_remove(event->connect.conn_handle) ? BLE_ERR_CONN_TERM_LOCAL : BLE_ERR_CONN_ESTABLISHMENT);
          picoruby_ble_peripheral_rearm_adv();
        }
        break;
      }
      if (event->connect.status == 0 ||
          (role == BLE_ROLE_PERIPHERAL && event->connect.status != BLE_HS_EAGAIN &&
           ble_gap_conn_find(event->connect.conn_handle, NULL) == 0)) {
        if (power_state != HCI_STATE_WORKING || terminating_has(event->connect.conn_handle) ||
            !picoruby_ble_connection_claim(event->connect.conn_handle)) {
          int rc = ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
          if (rc == 0 || rc == BLE_HS_EALREADY) {
            terminating_add(event->connect.conn_handle);
          } else if (rc != BLE_HS_ENOTCONN) {
            ESP_LOGE(BLE_TAG, "extra connection termination failed: %d", rc);
            picoruby_nimble_cancel_control(PICORUBY_NIMBLE_CONTROL_CONNECTION_LOST);
          }
          break;
        }
        conn_sc = false;
        picoruby_ble_central_reset_request();
        if (role == BLE_ROLE_CENTRAL) {
          struct ble_gap_conn_desc desc;
          build_le_connection_complete(central_connect_event, event->connect.conn_handle, 0,
                                       ble_gap_conn_find(event->connect.conn_handle, &desc) == 0 ? &desc : NULL);
          central_connect_token = ++central_connect_serial;
          if (ble_gattc_exchange_mtu(event->connect.conn_handle, central_mtu_cb, (void *)central_connect_token) != 0) {
            central_connect_token = 0;
            enqueue_control_event(central_connect_event, sizeof(central_connect_event));
          }
          picoruby_ble_central_resume_after_connect();
        }
      } else if (role == BLE_ROLE_CENTRAL) {
        bool was_pending = picoruby_ble_connection_connect_pending();
        picoruby_ble_connection_end_connect();
        if (was_pending && event->connect.status == BLE_HS_EAPP) {
          synth_disconnection_complete(0xffff, BLE_ERR_CONN_TERM_LOCAL);
        } else if (was_pending) {
          uint8_t status = (uint8_t)event->connect.status;
          synth_le_connection_complete(0, status ? status : 0xff);
        }
        picoruby_ble_central_resume_after_connect();
      } else if (role == BLE_ROLE_PERIPHERAL) {
        if (event->connect.status == BLE_HS_EAGAIN) {
          bool discarded = terminating_discarded_has(event->connect.conn_handle);
          bool terminated = terminating_remove(event->connect.conn_handle);
          if (power_state != HCI_STATE_OFF && !discarded) {
            synth_disconnection_complete(event->connect.conn_handle,
                                         terminated ? BLE_ERR_CONN_TERM_LOCAL : BLE_ERR_CONN_ESTABLISHMENT);
          }
        }
        picoruby_ble_peripheral_rearm_adv();
      } else if (role == BLE_ROLE_BROADCASTER) {
        picoruby_ble_peripheral_rearm_adv();
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      known_link_remove(event->disconnect.conn.conn_handle);
      terminating_remove(event->disconnect.conn.conn_handle);
      if (!picoruby_ble_connection_release(event->disconnect.conn.conn_handle)) {
        if (role == BLE_ROLE_CENTRAL && picoruby_ble_central_connect_lost(&event->disconnect.conn)) {
          uint8_t p[21];
          build_le_connection_complete(p, event->disconnect.conn.conn_handle, 0, &event->disconnect.conn);
          enqueue_control_event(p, sizeof(p));
          synth_disconnection_complete(event->disconnect.conn.conn_handle,
                                       (uint8_t)event->disconnect.reason);
          picoruby_ble_central_resume_after_connect();
        }
        break;
      }
      conn_sc = false;
      picoruby_ble_central_reset_request();
      if (role == BLE_ROLE_PERIPHERAL || role == BLE_ROLE_CENTRAL) {
        synth_disconnection_complete(event->disconnect.conn.conn_handle,
                                     (uint8_t)event->disconnect.reason);
      }
      if (role == BLE_ROLE_PERIPHERAL) {
        picoruby_ble_peripheral_rearm_adv();
      }
      break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      if (role == BLE_ROLE_PERIPHERAL || role == BLE_ROLE_BROADCASTER) {
        picoruby_ble_peripheral_rearm_adv();
      }
      break;
    case BLE_GAP_EVENT_MTU:
      peripheral_adopt(event->mtu.conn_handle);
      if (role == BLE_ROLE_PERIPHERAL && picoruby_ble_connection_matches(event->mtu.conn_handle)) {
        synth_mtu_exchange_complete(event->mtu.conn_handle, event->mtu.value);
      }
      break;
    case BLE_GAP_EVENT_SUBSCRIBE: {
      if (event->subscribe.reason == BLE_GAP_SUBSCRIBE_REASON_TERM) break;
      peripheral_adopt(event->subscribe.conn_handle);
      if (role != BLE_ROLE_PERIPHERAL || !picoruby_ble_connection_matches(event->subscribe.conn_handle)) break;
      attr_map_t *e = find_map_by_nimble(event->subscribe.attr_handle);
      if (e && e->cccd_ruby_handle) {
        uint8_t v[2] = { 0x00, 0x00 };
        if (event->subscribe.cur_notify) v[0] |= 0x01;
        if (event->subscribe.cur_indicate) v[0] |= 0x02;
        picoruby_nimble_enqueue_cccd(e->cccd_ruby_handle, v, 2);
      }
      break;
    }
    case BLE_GAP_EVENT_ENC_CHANGE:
      if (event->enc_change.status != 0) {
        ESP_LOGW(BLE_TAG, "encryption change failed: conn=%u status=%d",
                 (unsigned)event->enc_change.conn_handle, event->enc_change.status);
      }
      peripheral_adopt(event->enc_change.conn_handle);
      if (picoruby_ble_connection_matches(event->enc_change.conn_handle)) {
        conn_sc = false;
        struct ble_gap_conn_desc desc;
        if (event->enc_change.status == 0 && ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
          struct ble_store_key_sec key = { .peer_addr = desc.peer_id_addr };
          struct ble_store_value_sec bond;
          conn_sc = ble_store_read_peer_sec(&key, &bond) == 0 && bond.ltk_present && bond.sc;
        }
      }
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      struct ble_gap_conn_desc desc;
      peripheral_adopt(event->repeat_pairing.conn_handle);
      if (picoruby_ble_connection_matches(event->repeat_pairing.conn_handle) &&
          ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
        ble_store_util_delete_peer(&desc.peer_id_addr);
        rc = BLE_GAP_REPEAT_PAIRING_RETRY;
      }
      break;
    }
    case BLE_GAP_EVENT_DISC:
      if (role == BLE_ROLE_CENTRAL || role == BLE_ROLE_OBSERVER) {
        synth_advertising_report(&event->disc);
      }
      break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
      if (role != BLE_ROLE_CENTRAL || !picoruby_ble_connection_matches(event->notify_rx.conn_handle)) break;
      uint16_t vlen = OS_MBUF_PKTLEN(event->notify_rx.om);
      if (vlen > VALUE_EVENT_DATA_MAX) {
        ESP_LOGW(BLE_TAG, "notification exceeds event capacity: %u", (unsigned)vlen);
        break;
      }
      uint8_t p[GATT_EVENT_PAYLOAD_OFFSET + 4 + VALUE_EVENT_DATA_MAX];
      gatt_event_header(p, event->notify_rx.indication ? EVT_GATT_INDICATION : EVT_GATT_NOTIFICATION,
                        (uint8_t)(GATT_EVENT_PAYLOAD_OFFSET + 2 + vlen), event->notify_rx.conn_handle);
      put_le16(p + GATT_EVENT_PAYLOAD_OFFSET, event->notify_rx.attr_handle);
      put_le16(p + GATT_EVENT_PAYLOAD_OFFSET + 2, vlen);
      if (os_mbuf_copydata(event->notify_rx.om, 0, vlen, p + GATT_EVENT_PAYLOAD_OFFSET + 4) != 0) {
        ESP_LOGW(BLE_TAG, "notification mbuf copy failed");
        break;
      }
      picoruby_nimble_enqueue_event(p, GATT_EVENT_PAYLOAD_OFFSET + 4 + vlen, PICORUBY_NIMBLE_EVENT_DATA);
      break;
    }
    default:
      break;
  }
  picoruby_ble_connection_gate_exit();
  return rc;
}

static void
power_set_heartbeat(bool on)
{
  if (power_on_requested == on) return;
  power_on_requested = on;
  picoruby_nimble_heartbeat_enable(on);
}

typedef struct {
  uint16_t handle[MYNEWT_VAL(BLE_MAX_CONNECTIONS)];
  int count;
} known_links_t;

static void
list_known_links(known_links_t *links)
{
  links->count = 0;
  if (!ble_hs_is_enabled()) return;
  links->count = known_link_count;
  memcpy(links->handle, known_link, sizeof(known_link));
}

static int
power_terminate_connection(void)
{
  known_links_t links;
  list_known_links(&links);
  uint16_t claimed = picoruby_ble_connection_handle_snapshot();
  bool claimed_listed = false;
  for (int i = 0; i < links.count; i++) {
    if (links.handle[i] == claimed) claimed_listed = true;
  }
  if (central_connect_token != 0) {
    central_connect_token = 0;
    uint16_t held = (uint16_t)(central_connect_event[4] | (central_connect_event[5] << 8));
    if (claimed_listed && held == claimed) {
      enqueue_control_event(central_connect_event, sizeof(central_connect_event));
    }
  }
  if (claimed != 0xffff && !claimed_listed) picoruby_ble_connection_forget(claimed);
  for (int i = 0; i < links.count; i++) {
    peripheral_adopt(links.handle[i]);
  }
  for (int i = 0; i < links.count; i++) {
    int rc = ble_gap_terminate(links.handle[i], BLE_ERR_REM_USER_CONN_TERM);
    if (rc == 0 || rc == BLE_HS_EALREADY) terminating_add(links.handle[i]);
    if (rc == BLE_HS_ENOTCONN) known_link_remove(links.handle[i]);
    if (rc == 0 || rc == BLE_HS_EALREADY || rc == BLE_HS_ENOTCONN) continue;
    if (picoruby_nimble_control_fault() & PICORUBY_NIMBLE_CONTROL_CONNECTION_LOST) continue;
    ESP_LOGE(BLE_TAG, "connection termination failed: %d", rc);
    picoruby_nimble_cancel_control(PICORUBY_NIMBLE_CONTROL_CONNECTION_LOST);
  }
  return links.count;
}

static void
power_finish_off(void)
{
  picoruby_nimble_halting_watchdog_stop();
  power_state = HCI_STATE_OFF;
  synth_state(HCI_STATE_OFF);
}

static void
power_halting_step(void)
{
  if (power_state != HCI_STATE_HALTING) return;
  if (power_terminate_connection() > 0) return;
  if (picoruby_ble_connection_handle_snapshot() != 0xffff) return;
  if (picoruby_ble_connection_connect_pending()) return;
  if (picoruby_ble_connection_release_deferred_record()) {
    synth_disconnection_complete(0xffff, BLE_ERR_CONN_TERM_LOCAL);
  }
  power_finish_off();
}

void
picoruby_ble_halting_watchdog_fire(void)
{
  if (!picoruby_ble_connection_gate_enter()) return;
  if (power_state == HCI_STATE_HALTING) {
    uint16_t claimed = picoruby_ble_connection_handle_snapshot();
    if (claimed != 0xffff) {
      picoruby_ble_connection_forget(claimed);
      terminating_discard(claimed);
      synth_disconnection_complete(claimed, BLE_ERR_CONN_TERM_LOCAL);
    }
    if (picoruby_ble_connection_connect_pending()) {
      picoruby_ble_connection_end_connect();
      synth_disconnection_complete(0xffff, BLE_ERR_CONN_TERM_LOCAL);
    }
    if (picoruby_ble_connection_release_deferred_record()) {
      synth_disconnection_complete(0xffff, BLE_ERR_CONN_TERM_LOCAL);
    }
    power_finish_off();
  }
  picoruby_ble_connection_gate_exit();
}

void
picoruby_ble_reconcile_connections(void)
{
  if (role == BLE_ROLE_CENTRAL && picoruby_ble_central_reconcile_connect()) {
    ESP_LOGW(BLE_TAG, "connection lost: pending connect ended without a CONNECT event");
    synth_le_connection_complete(0, status_to_u8(BLE_HS_ENOTCONN));
    picoruby_nimble_cancel_control(PICORUBY_NIMBLE_CONTROL_CONNECTION_LOST);
    picoruby_ble_central_resume_after_connect();
  }
  power_halting_step();
}

int
BLE_init(const uint8_t *profile_data, size_t profile_len, int ble_role)
{
  size_t len;
  if (BLE_profile_extent(profile_data, profile_len, &len) < 0) {
    ESP_LOGE(BLE_TAG, "profile blob is malformed");
    return -1;
  }
  if (validate_att_topology(profile_data, len) < 0) {
    ESP_LOGE(BLE_TAG, "profile rejected at handle %u: %s",
             (unsigned)profile_reject_handle, profile_reject_reason);
    return -1;
  }
  if (picoruby_nimble_cleanup_blocked()) {
    ESP_LOGE(BLE_TAG, "init refused: previous NimBLE cleanup is blocked");
    return -1;
  }
  uint8_t *replacement = NULL;
  if (profile_data) {
    replacement = (uint8_t *)malloc(len);
    if (replacement == NULL) {
      ESP_LOGE(BLE_TAG, "profile copy allocation failed");
      return -1;
    }
    memcpy(replacement, profile_data, len);
  }
  if (picoruby_nimble_started() && picoruby_nimble_stop() != 0) {
    free(replacement);
    ESP_LOGE(BLE_TAG, "NimBLE stop failed");
    return -1;
  }
  role = (enum BLE_role_t)ble_role;
  power_on_requested = false;
  picoruby_ble_connection_close();
  terminating_count = 0;
  known_link_count = 0;
  central_connect_token = 0;
  have_services = false;
  attr_map_count = 0;
  rdm_used = 0;
  heap_caps_free(rdm_pool);
  rdm_pool = NULL;

  free(owned_profile_data);
  owned_profile_data = replacement;
  if (replacement && parse_att_db(replacement, len) < 0) {
    free(owned_profile_data);
    owned_profile_data = NULL;
    ESP_LOGE(BLE_TAG, "profile translation failed");
    return -1;
  }
  int cccds = 0;
  for (int i = 0; i < attr_map_count; i++) {
    if (attr_map[i].cccd_ruby_handle) cccds++;
  }
  picoruby_nimble_reserve_cccd_writes(cccds);
  uint16_t mismatch = profile_layout_mismatch(replacement, len);
  if (mismatch) {
    ESP_LOGW(BLE_TAG, "GATT layout differs from profile at handle %u", (unsigned)mismatch);
  }
  int rc = picoruby_nimble_start(register_services);
  if (rc != 0) {
    ESP_LOGE(BLE_TAG, "NimBLE start failed: %d", rc);
    attr_map_count = rdm_used = 0;
    have_services = false;
    return rc;
  }
  if (rdm_used > 0) {
    size_t pool_bytes = (size_t)rdm_used * RDM_MAX;
    rdm_pool = heap_caps_malloc(pool_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (rdm_pool == NULL) {
      ESP_LOGE(BLE_TAG, "read mirror pool allocation failed: %u bytes", (unsigned)pool_bytes);
      if (picoruby_nimble_stop() != 0) ESP_LOGE(BLE_TAG, "NimBLE stop failed");
      attr_map_count = rdm_used = 0;
      have_services = false;
      return -1;
    }
  }
  return 0;
}

static void
power_suspend_requests(void)
{
  switch (role) {
    case BLE_ROLE_PERIPHERAL:
    case BLE_ROLE_BROADCASTER:
      picoruby_ble_peripheral_suspend_adv();
      break;
    case BLE_ROLE_CENTRAL:
    case BLE_ROLE_OBSERVER:
      picoruby_ble_central_suspend_scan();
      break;
    default:
      break;
  }
}

static void
power_resume_requests(void)
{
  switch (role) {
    case BLE_ROLE_PERIPHERAL:
    case BLE_ROLE_BROADCASTER:
      picoruby_ble_peripheral_resume_adv();
      break;
    case BLE_ROLE_CENTRAL:
    case BLE_ROLE_OBSERVER:
      picoruby_ble_central_resume_scan();
      break;
    default:
      break;
  }
}

int
BLE_hci_power_control(uint8_t power_mode)
{
  if (power_mode > 2) return BLE_POWER_CONTROL_INVALID;
  if (!picoruby_nimble_ready() && (power_mode != 0 || !picoruby_nimble_started())) {
    power_state = HCI_STATE_OFF;
    power_on_requested = false;
    picoruby_nimble_halting_watchdog_stop();
    picoruby_nimble_heartbeat_enable(false);
    return power_mode == 0 ? 0 : BLE_POWER_CONTROL_NOT_READY;
  }
  if (!picoruby_ble_connection_gate_enter()) return -1;
  switch (power_mode) {
    case 1:
      if (power_state == HCI_STATE_WORKING) {
        synth_state(HCI_STATE_WORKING);
        break;
      }
      synth_state(HCI_STATE_INITIALIZING);
      if (power_state == HCI_STATE_HALTING) picoruby_nimble_halting_watchdog_stop();
      picoruby_ble_connection_clear_deferred();
      power_state = HCI_STATE_WORKING;
      power_set_heartbeat(true);
      synth_state(HCI_STATE_WORKING);
      power_resume_requests();
      break;
    case 0:
      if (power_state == HCI_STATE_OFF || power_state == HCI_STATE_HALTING) {
        synth_state(power_state);
        break;
      }
      power_set_heartbeat(false);
      power_state = HCI_STATE_HALTING;
      synth_state(HCI_STATE_HALTING);
      picoruby_nimble_halting_watchdog_start();
      power_suspend_requests();
      if (picoruby_ble_connection_connect_pending() && ble_hs_is_enabled()) {
        int rc = ble_gap_conn_cancel();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
          ESP_LOGE(BLE_TAG, "connect cancel failed: %d", rc);
          picoruby_nimble_cancel_control(PICORUBY_NIMBLE_CONTROL_CONNECTION_LOST);
        }
      }
      power_halting_step();
      break;
    case 2:
      if (power_state == HCI_STATE_OFF || power_state == HCI_STATE_SLEEPING) {
        synth_state(power_state);
        break;
      }
      power_set_heartbeat(false);
      if (power_state == HCI_STATE_HALTING) picoruby_nimble_halting_watchdog_stop();
      power_state = HCI_STATE_FALLING_ASLEEP;
      synth_state(HCI_STATE_FALLING_ASLEEP);
      power_terminate_connection();
      picoruby_ble_connection_release_deferred_record();
      power_state = HCI_STATE_SLEEPING;
      synth_state(HCI_STATE_SLEEPING);
      break;
  }
  picoruby_ble_connection_gate_exit();
  return 0;
}

int
BLE_gap_local_bd_addr(uint8_t *local_addr)
{
  if (!picoruby_nimble_ready()) return -1;
  uint8_t own_type = picoruby_nimble_own_addr_type();
  uint8_t id_type;
  switch (own_type) {
    case BLE_OWN_ADDR_PUBLIC: id_type = BLE_ADDR_PUBLIC; break;
    case BLE_OWN_ADDR_RANDOM: id_type = BLE_ADDR_RANDOM; break;
    default: return -1;
  }
  uint8_t le[6];
  int rc = ble_hs_id_copy_addr(id_type, le, NULL);
  if (rc != 0) {
    ESP_LOGW(BLE_TAG, "local address unavailable: %d", rc);
    return -1;
  }
  for (int i = 0; i < 6; i++) local_addr[i] = le[5 - i];
  return 0;
}

int
picoruby_ble_att_payload_status(uint16_t handle, uint16_t size)
{
  uint16_t mtu = ble_att_mtu(handle);
  if (mtu == 0) return BLE_HS_ENOTCONN;
  if (mtu < 3 || size > mtu - 3u) return BLE_HS_EMSGSIZE;
  return 0;
}

typedef struct {
  bool active;
  bool have_prev;
  uint16_t conn_handle;
  uint16_t end_handle;
  struct ble_gatt_chr prev;
} characteristic_discovery_t;

static characteristic_discovery_t chr_discoveries[MYNEWT_VAL(BLE_MAX_CONNECTIONS)];
static portMUX_TYPE chr_disc_mux = portMUX_INITIALIZER_UNLOCKED;

enum gatt_request_state {
  GATT_REQUEST_IDLE,
  GATT_REQUEST_PENDING,
  GATT_REQUEST_WAIT_DISCONNECT
};
static enum gatt_request_state request_state;
static uint16_t request_conn;

void
picoruby_ble_central_reset_request(void)
{
  request_state = GATT_REQUEST_IDLE;
  request_conn = 0xffff;
}

static int
gatt_request_begin(uint16_t conn)
{
  if (request_state != GATT_REQUEST_IDLE) return BLE_HS_EBUSY;
  request_conn = conn;
  request_state = GATT_REQUEST_PENDING;
  return 0;
}

static void
gatt_request_complete(uint16_t conn, uint16_t status)
{
  if (request_state != GATT_REQUEST_PENDING || request_conn != conn) return;
  if (status == BLE_HS_ETIMEOUT) {
    request_state = GATT_REQUEST_WAIT_DISCONNECT;
    return;
  }
  request_state = GATT_REQUEST_IDLE;
}

static void
gatt_request_returned(uint16_t conn, int rc)
{
  if (rc == 0 || request_state != GATT_REQUEST_PENDING || request_conn != conn) return;
  request_state = GATT_REQUEST_IDLE;
}

static void
reset_characteristic_discoveries(void)
{
  taskENTER_CRITICAL(&chr_disc_mux);
  for (int i = 0; i < MYNEWT_VAL(BLE_MAX_CONNECTIONS); i++) {
    chr_discoveries[i].active = false;
  }
  taskEXIT_CRITICAL(&chr_disc_mux);
}

static uint8_t
att_status(int status)
{
  if (status == 0 || status == BLE_HS_EDONE) return 0;
  if (status > BLE_HS_ERR_ATT_BASE && status < BLE_HS_ERR_HCI_BASE) return (uint8_t)(status - BLE_HS_ERR_ATT_BASE);
  if (status == BLE_HS_ENOTCONN) return ATT_ERROR_HCI_DISCONNECT_RECEIVED;
  if (status == BLE_HS_ETIMEOUT) return ATT_ERROR_TIMEOUT;
  ESP_LOGW(BLE_TAG, "GATT procedure failed: %d", status);
  return ATT_ERROR_UNLIKELY_ERROR;
}

uint8_t
picoruby_ble_status_code(int rc)
{
  if (rc == 0) return 0;
  if (rc == BLE_HS_ENOTCONN) return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
  if (rc == BLE_HS_ENOMEM) return ERROR_CODE_MEMORY_CAPACITY_EXCEEDED;
  return ERROR_CODE_UNSPECIFIED_ERROR;
}

uint8_t
picoruby_ble_request_status(int rc)
{
  uint8_t status = picoruby_ble_status_code(rc);
  if (status == ERROR_CODE_UNSPECIFIED_ERROR) ESP_LOGW(BLE_TAG, "BLE request failed: %d", rc);
  return status;
}

static int
disc_svc_cb(uint16_t conn, const struct ble_gatt_error *error,
            const struct ble_gatt_svc *service, void *arg)
{
  (void)arg;
  if (error->status == 0 && service) {
    uint8_t p[GATT_EVENT_PAYLOAD_OFFSET + 20];
    gatt_event_header(p, EVT_GATT_SERVICE_QUERY_RESULT, sizeof(p) - 2, conn);
    put_le16(p + GATT_EVENT_PAYLOAD_OFFSET, service->start_handle);
    put_le16(p + GATT_EVENT_PAYLOAD_OFFSET + 2, service->end_handle);
    uuid_to_le128(&service->uuid, p + GATT_EVENT_PAYLOAD_OFFSET + 4);
    enqueue_control_event(p, sizeof(p));
  } else {
    gatt_request_complete(conn, error->status);
    synth_query_complete(conn, att_status(error->status));
  }
  return 0;
}

static void
emit_characteristic(uint16_t conn, const struct ble_gatt_chr *c, uint16_t end_handle)
{
  uint8_t p[GATT_EVENT_PAYLOAD_OFFSET + 24];
  gatt_event_header(p, EVT_GATT_CHARACTERISTIC_QUERY_RESULT, sizeof(p) - 2, conn);
  put_le16(p + GATT_EVENT_PAYLOAD_OFFSET, c->def_handle);
  put_le16(p + GATT_EVENT_PAYLOAD_OFFSET + 2, c->val_handle);
  put_le16(p + GATT_EVENT_PAYLOAD_OFFSET + 4, end_handle);
  put_le16(p + GATT_EVENT_PAYLOAD_OFFSET + 6, c->properties);
  uuid_to_le128(&c->uuid, p + GATT_EVENT_PAYLOAD_OFFSET + 8);
  enqueue_control_event(p, sizeof(p));
}

static int
disc_chr_cb(uint16_t conn, const struct ble_gatt_error *error,
            const struct ble_gatt_chr *chr, void *arg)
{
  characteristic_discovery_t *discovery = arg;
  if (error->status == 0 && chr) {
    if (discovery->have_prev) {
      emit_characteristic(conn, &discovery->prev, chr->def_handle - 1);
    }
    discovery->prev = *chr;
    discovery->have_prev = true;
  } else {
    if (discovery->have_prev && (error->status == 0 || error->status == BLE_HS_EDONE)) {
      emit_characteristic(conn, &discovery->prev, discovery->end_handle);
    }
    discovery->have_prev = false;
    taskENTER_CRITICAL(&chr_disc_mux);
    discovery->active = false;
    taskEXIT_CRITICAL(&chr_disc_mux);
    gatt_request_complete(conn, error->status);
    synth_query_complete(conn, att_status(error->status));
  }
  return 0;
}

static int
disc_dsc_cb(uint16_t conn, const struct ble_gatt_error *error,
            uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
  (void)chr_val_handle;
  (void)arg;
  if (error->status == 0 && dsc) {
    uint8_t p[GATT_EVENT_PAYLOAD_OFFSET + 18];
    gatt_event_header(p, EVT_GATT_ALL_DESCRIPTORS_QUERY_RESULT, sizeof(p) - 2, conn);
    put_le16(p + GATT_EVENT_PAYLOAD_OFFSET, dsc->handle);
    uuid_to_le128(&dsc->uuid, p + GATT_EVENT_PAYLOAD_OFFSET + 2);
    enqueue_control_event(p, sizeof(p));
  } else {
    gatt_request_complete(conn, error->status);
    synth_query_complete(conn, att_status(error->status));
  }
  return 0;
}

static int
read_attr_cb(uint16_t conn, const struct ble_gatt_error *error,
             struct ble_gatt_attr *attr, void *arg)
{
  (void)arg;
  gatt_request_complete(conn, error->status);
  if (error->status == 0 && attr && attr->om) {
    uint16_t vlen = OS_MBUF_PKTLEN(attr->om);
    if (vlen > VALUE_EVENT_DATA_MAX) {
      synth_query_complete(conn, att_status(BLE_HS_EMSGSIZE));
      return 0;
    }
    uint8_t p[GATT_EVENT_PAYLOAD_OFFSET + 4 + VALUE_EVENT_DATA_MAX];
    gatt_event_header(p, EVT_GATT_CHARACTERISTIC_VALUE_QUERY_RESULT,
                      (uint8_t)(GATT_EVENT_PAYLOAD_OFFSET + 2 + vlen), conn);
    put_le16(p + GATT_EVENT_PAYLOAD_OFFSET, attr->handle);
    put_le16(p + GATT_EVENT_PAYLOAD_OFFSET + 2, vlen);
    if (os_mbuf_copydata(attr->om, 0, vlen, p + GATT_EVENT_PAYLOAD_OFFSET + 4) != 0) {
      synth_query_complete(conn, att_status(BLE_HS_EINVAL));
      return 0;
    }
    enqueue_control_event(p, GATT_EVENT_PAYLOAD_OFFSET + 4 + vlen);
  }
  synth_query_complete(conn, att_status(error->status));
  return 0;
}

static int
write_attr_cb(uint16_t conn, const struct ble_gatt_error *error,
              struct ble_gatt_attr *attr, void *arg)
{
  (void)attr;
  (void)arg;
  gatt_request_complete(conn, error->status);
  synth_query_complete(conn, att_status(error->status));
  return 0;
}

static uint8_t
BLE_discover_primary_services_on_connection(uint16_t conn_handle)
{
  if (gatt_request_begin(conn_handle) != 0) return GATT_CLIENT_IN_WRONG_STATE;
  int rc = ble_gattc_disc_all_svcs(conn_handle, disc_svc_cb, NULL);
  gatt_request_returned(conn_handle, rc);
  return picoruby_ble_request_status(rc);
}

static uint8_t
BLE_discover_characteristics_for_service_on_connection(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle)
{
  if (gatt_request_begin(conn_handle) != 0) return GATT_CLIENT_IN_WRONG_STATE;
  characteristic_discovery_t *discovery = NULL;
  taskENTER_CRITICAL(&chr_disc_mux);
  for (int i = 0; i < MYNEWT_VAL(BLE_MAX_CONNECTIONS); i++) {
    characteristic_discovery_t *slot = &chr_discoveries[i];
    if (slot->active && slot->conn_handle == conn_handle) {
      taskEXIT_CRITICAL(&chr_disc_mux);
      gatt_request_complete(conn_handle, 0);
      return GATT_CLIENT_IN_WRONG_STATE;
    }
    if (!slot->active && discovery == NULL) discovery = slot;
  }
  if (discovery == NULL) {
    taskEXIT_CRITICAL(&chr_disc_mux);
    gatt_request_complete(conn_handle, 0);
    return GATT_CLIENT_IN_WRONG_STATE;
  }
  discovery->conn_handle = conn_handle;
  discovery->end_handle = end_handle;
  discovery->have_prev = false;
  discovery->active = true;
  taskEXIT_CRITICAL(&chr_disc_mux);

  int rc = ble_gattc_disc_all_chrs(conn_handle, start_handle, end_handle,
                             disc_chr_cb, discovery);
  if (rc != 0) {
    taskENTER_CRITICAL(&chr_disc_mux);
    discovery->active = false;
    taskEXIT_CRITICAL(&chr_disc_mux);
  }
  gatt_request_returned(conn_handle, rc);
  return picoruby_ble_request_status(rc);
}

static uint8_t
BLE_read_value_of_characteristic_using_value_handle_on_connection(uint16_t conn_handle, uint16_t value_handle)
{
  if (gatt_request_begin(conn_handle) != 0) return GATT_CLIENT_IN_WRONG_STATE;
  int rc = ble_gattc_read(conn_handle, value_handle, read_attr_cb, NULL);
  gatt_request_returned(conn_handle, rc);
  return picoruby_ble_request_status(rc);
}

static uint8_t
BLE_discover_characteristic_descriptors_on_connection(uint16_t conn_handle, uint16_t value_handle, uint16_t end_handle)
{
  if (gatt_request_begin(conn_handle) != 0) return GATT_CLIENT_IN_WRONG_STATE;
  int rc = ble_gattc_disc_all_dscs(conn_handle, value_handle, end_handle, disc_dsc_cb, NULL);
  gatt_request_returned(conn_handle, rc);
  return picoruby_ble_request_status(rc);
}

static uint8_t
BLE_write_value_of_characteristic_without_response_on_connection(uint16_t conn_handle, uint16_t value_handle, const uint8_t *data, uint16_t size)
{
  int rc = picoruby_ble_att_payload_status(conn_handle, size);
  if (rc == BLE_HS_EMSGSIZE) return GATT_CLIENT_VALUE_TOO_LONG;
  if (rc == 0) rc = ble_gattc_write_no_rsp_flat(conn_handle, value_handle, data, size);
  if (rc == BLE_HS_ENOMEM) return GATT_CLIENT_BUSY;
  return picoruby_ble_request_status(rc);
}

static uint8_t
BLE_write_characteristic_descriptor_using_descriptor_handle_on_connection(uint16_t conn_handle, uint16_t descriptor_handle, const uint8_t *data, uint16_t size)
{
  if (gatt_request_begin(conn_handle) != 0) return GATT_CLIENT_IN_WRONG_STATE;
  int rc = picoruby_ble_att_payload_status(conn_handle, size);
  if (rc == BLE_HS_EMSGSIZE) {
    gatt_request_complete(conn_handle, 0);
    synth_query_complete(conn_handle, ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH);
    return 0;
  }
  if (rc == 0) rc = ble_gattc_write_flat(conn_handle, descriptor_handle, data, size, write_attr_cb, NULL);
  gatt_request_returned(conn_handle, rc);
  return picoruby_ble_request_status(rc);
}

uint8_t
BLE_discover_primary_services(uint16_t conn_handle)
{
  if (!picoruby_ble_connection_gate_enter()) return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
  if (!picoruby_ble_connection_matches(conn_handle)) {
    picoruby_ble_connection_gate_exit();
    return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
  }
  uint8_t rc = BLE_discover_primary_services_on_connection(conn_handle);
  picoruby_ble_connection_gate_exit();
  return rc;
}

uint8_t
BLE_discover_characteristics_for_service(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle)
{
  if (!picoruby_ble_connection_gate_enter()) return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
  if (!picoruby_ble_connection_matches(conn_handle)) {
    picoruby_ble_connection_gate_exit();
    return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
  }
  uint8_t rc = BLE_discover_characteristics_for_service_on_connection(conn_handle, start_handle, end_handle);
  picoruby_ble_connection_gate_exit();
  return rc;
}

uint8_t
BLE_read_value_of_characteristic_using_value_handle(uint16_t conn_handle, uint16_t value_handle)
{
  if (!picoruby_ble_connection_gate_enter()) return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
  if (!picoruby_ble_connection_matches(conn_handle)) {
    picoruby_ble_connection_gate_exit();
    return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
  }
  uint8_t rc = BLE_read_value_of_characteristic_using_value_handle_on_connection(conn_handle, value_handle);
  picoruby_ble_connection_gate_exit();
  return rc;
}

uint8_t
BLE_discover_characteristic_descriptors(uint16_t conn_handle, uint16_t value_handle, uint16_t end_handle)
{
  if (!picoruby_ble_connection_gate_enter()) return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
  if (!picoruby_ble_connection_matches(conn_handle)) {
    picoruby_ble_connection_gate_exit();
    return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
  }
  uint8_t rc = BLE_discover_characteristic_descriptors_on_connection(conn_handle, value_handle, end_handle);
  picoruby_ble_connection_gate_exit();
  return rc;
}

uint8_t
BLE_write_value_of_characteristic_without_response(uint16_t conn_handle, uint16_t value_handle, const uint8_t *data, uint16_t size)
{
  if (!picoruby_ble_connection_gate_enter()) return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
  if (!picoruby_ble_connection_matches(conn_handle)) {
    picoruby_ble_connection_gate_exit();
    return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
  }
  uint8_t rc = BLE_write_value_of_characteristic_without_response_on_connection(conn_handle, value_handle, data, size);
  picoruby_ble_connection_gate_exit();
  return rc;
}

uint8_t
BLE_write_characteristic_descriptor_using_descriptor_handle(uint16_t conn_handle, uint16_t descriptor_handle, const uint8_t *data, uint16_t size)
{
  if (!picoruby_ble_connection_gate_enter()) return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
  if (!picoruby_ble_connection_matches(conn_handle)) {
    picoruby_ble_connection_gate_exit();
    return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
  }
  uint8_t rc = BLE_write_characteristic_descriptor_using_descriptor_handle_on_connection(conn_handle, descriptor_handle, data, size);
  picoruby_ble_connection_gate_exit();
  return rc;
}

bool
BLE_ready(void)
{
  return picoruby_nimble_ready();
}

int
BLE_control_fault(void)
{
  return picoruby_nimble_control_fault();
}

void
BLE_vm_attached(void *vm)
{
  picoruby_nimble_attach_vm(vm);
}

void
BLE_events_cleared(void)
{
  picoruby_nimble_reset_events();
}
