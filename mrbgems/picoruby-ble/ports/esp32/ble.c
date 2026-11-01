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
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

#include "ble_common.h"
#include "nimble_owner.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

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

#define HCI_STATE_WORKING 2

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
#error "BLE event 容量と SDK の MTU 最大値が一致しない"
#endif

#define RDM_SLOTS 16
#define RDM_MAX PICORUBY_BLE_READ_MIRROR_MAX

static enum BLE_role_t role = BLE_ROLE_NONE;
// VM側のON要求を記録する。NimBLE/controllerの実稼働状態ではない。
static bool power_on_requested = false;

static uint8_t  rdm_buf[RDM_SLOTS][RDM_MAX];
static uint16_t rdm_len[RDM_SLOTS];
static bool     rdm_valid[RDM_SLOTS];
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
static struct ble_gatt_chr_def chr_slots[MAX_CHR_SLOTS];
static struct ble_gatt_dsc_def dsc_slots[MAX_DSC_SLOTS];
static ble_uuid_any_t uuid_pool[MAX_UUIDS];
static int svc_count, chr_slot_count, dsc_slot_count, uuid_count;
static bool have_services = false;

static uint8_t *owned_profile_data = NULL;

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

static int
gatt_access_cb(uint16_t conn, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
  (void)attr_handle;
  if (!picoruby_ble_connection_gate_enter()) return BLE_ATT_ERR_UNLIKELY;
  bool matching = picoruby_ble_connection_matches(conn);
  picoruby_ble_connection_gate_exit();
  if (!matching) return BLE_ATT_ERR_UNLIKELY;
  attr_map_t *e = (attr_map_t *)arg;
  if (e == NULL) return BLE_ATT_ERR_UNLIKELY;

  switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
    case BLE_GATT_ACCESS_OP_READ_DSC: {
      if (!(e->att_flags & BLOB_FLAG_READ)) return BLE_ATT_ERR_READ_NOT_PERMITTED;
      static uint8_t rd_tmp[RDM_MAX]; // static is safe: this callback has only one caller task
      const uint8_t *data = e->static_value;
      uint16_t len = e->static_len;
      if (e->mirror_slot >= 0) {
        uint16_t n;
        bool valid;
        taskENTER_CRITICAL(&rdm_mux);
        n = rdm_len[e->mirror_slot];
        valid = rdm_valid[e->mirror_slot];
        if (valid && n > RDM_MAX) {
          taskEXIT_CRITICAL(&rdm_mux);
          return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (n > 0) memcpy(rd_tmp, rdm_buf[e->mirror_slot], n);
        taskEXIT_CRITICAL(&rdm_mux);
        if (valid) {
          data = rd_tmp;
          len = n;
        } else {
          data = NULL;
          len = 0;
        }
      }
      if (data == NULL || len == 0) return 0;
      return os_mbuf_append(ctxt->om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES; // can allocate, so called after the critical section exits
    }
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
    case BLE_GATT_ACCESS_OP_WRITE_DSC: {
      if (!(e->att_flags & BLOB_FLAG_DYNAMIC) ||
          !(e->att_flags & (BLOB_FLAG_WRITE | BLOB_FLAG_WRITE_NO_RSP))) {
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
      }
      static uint8_t buf[256];
      uint16_t out_len = 0;
      if (OS_MBUF_PKTLEN(ctxt->om) > sizeof(buf)) {
        picoruby_nimble_record_oversized_event();
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      }
      if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &out_len) != 0) {
        picoruby_nimble_record_copy_failure();
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

/* VM専用。呼出元がString型・長さ上限を検査済み。入力のpointerを保持しない。 */
void
picoruby_ble_publish_read_value(uint16_t ruby_handle, const uint8_t *data, uint16_t size)
{
  for (int i = 0; i < attr_map_count; i++) {
    attr_map_t *e = &attr_map[i];
    if (e->ruby_handle != ruby_handle || e->mirror_slot < 0) continue;
    taskENTER_CRITICAL(&rdm_mux);
    if (size > 0) memcpy(rdm_buf[e->mirror_slot], data, size);
    rdm_len[e->mirror_slot] = size;
    rdm_valid[e->mirror_slot] = true;
    taskEXIT_CRITICAL(&rdm_mux);
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
    taskENTER_CRITICAL(&rdm_mux);
    if (n <= RDM_MAX) memcpy(rdm_buf[e->mirror_slot], tmp, n);
    rdm_len[e->mirror_slot] = n;
    rdm_valid[e->mirror_slot] = true;
    taskEXIT_CRITICAL(&rdm_mux);
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
  return f;
}

/* Packed ATT permissions are distinct from characteristic declaration bits. */
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

static ble_gatt_chr_flags
profile_chr_security(uint16_t flags)
{
  unsigned rd = profile_read_level(flags), wr = profile_write_level(flags);
  ble_gatt_chr_flags out = 0;
  if (rd >= 1) out |= BLE_GATT_CHR_F_READ_ENC;
  if (rd >= 2) out |= BLE_GATT_CHR_F_READ_AUTHEN;
  if (rd >= 3) out |= BLE_GATT_CHR_F_READ_AUTHOR;
  if (wr >= 1) out |= BLE_GATT_CHR_F_WRITE_ENC;
  if (wr >= 2) out |= BLE_GATT_CHR_F_WRITE_AUTHEN;
  if (wr >= 3) out |= BLE_GATT_CHR_F_WRITE_AUTHOR;
  return out;
}

static uint8_t
profile_dsc_flags(uint16_t flags)
{
  unsigned rd = profile_read_level(flags), wr = profile_write_level(flags);
  uint8_t out = 0;
  if (flags & BLOB_FLAG_READ) out |= BLE_ATT_F_READ;
  if (flags & (BLOB_FLAG_WRITE | BLOB_FLAG_WRITE_NO_RSP)) out |= BLE_ATT_F_WRITE;
  if (rd >= 1) out |= BLE_ATT_F_READ_ENC;
  if (rd >= 2) out |= BLE_ATT_F_READ_AUTHEN;
  if (rd >= 3) out |= BLE_ATT_F_READ_AUTHOR;
  if (wr >= 1) out |= BLE_ATT_F_WRITE_ENC;
  if (wr >= 2) out |= BLE_ATT_F_WRITE_AUTHEN;
  if (wr >= 3) out |= BLE_ATT_F_WRITE_AUTHOR;
  return out;
}

static ble_gatt_chr_flags
profile_cccd_security(uint16_t flags)
{
  ble_gatt_chr_flags out = 0;
#if defined(BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC) && \
    defined(BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN) && \
    defined(BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR)
  unsigned wr = profile_write_level(flags);
  if (wr >= 1) out |= BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC;
  if (wr >= 2) out |= BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN;
  if (wr >= 3) out |= BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR;
#else
  (void)flags; /* Older public API has only global CCCD permissions. */
#endif
  return out;
}

/* Candidate policy, not an assertion of compatibility: reject requirements
 * that these public definition fields cannot preserve, before stopping the
 * old registration. Request and command writes reach one SDK callback. */
static int
profile_permissions_supported(uint16_t flags)
{
  unsigned key = profile_min_key_size(flags);
  uint16_t writes = flags & (BLOB_FLAG_WRITE | BLOB_FLAG_WRITE_NO_RSP);
  if (key && key < 7) return -1;
  if ((flags & BLOB_FLAG_READ) && (flags & BLOB_FLAG_READ_SC)) return -1;
  if (writes && (flags & BLOB_FLAG_WRITE_SC)) return -1;
  if ((flags & BLOB_FLAG_SIGNED_WRITE) && !writes) return -1;
  if (writes && writes != (BLOB_FLAG_WRITE | BLOB_FLAG_WRITE_NO_RSP)) return -1;
  return 0;
}

static int
profile_cccd_supported(uint16_t flags)
{
  if (!(flags & BLOB_FLAG_DYNAMIC) || profile_permissions_supported(flags) < 0) return -1;
  /* NimBLE registers automatic CCCDs with READ|WRITE and min_key_size=0;
   * only its write security requirements have per-characteristic fields. */
  if ((flags & BLOB_FLAG_OPERATIONS) != BLOB_FLAG_OPERATIONS ||
      profile_read_level(flags) ||
      (profile_min_key_size(flags) && profile_write_level(flags))) return -1;
#if !defined(BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC) || \
    !defined(BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN) || \
    !defined(BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR)
  if (profile_write_level(flags)) return -1;
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
  return 0;
}

/* Classify only reserved record types. Bluetooth-base128 aliases have the
 * same GATT meaning as their 16-bit UUIDs; retain the original UUID bytes
 * everywhere else (including declaration/value matching and SDK UUID data). */
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
    case 0x2802: /* Include-service declaration. */
    case GATT_CHARACTERISTIC_UUID:
    case GATT_CCCD_UUID:
      return u16;
    default:
      return 0;
  }
}

/* Read-only preflight: do not discard a running registration for a malformed
 * replacement. Count the same definition, terminator, map and mirror slots as
 * the translator before it can mutate global state or call the SDK. */
static int
validate_att_topology(const uint8_t *db, size_t db_len)
{
  if (BLE_profile_extent(db, db_len, &db_len) < 0) return -1;
  if (db == NULL) return 0;
  int services = 0, chars = 0, descs = 0, maps = 0, uuids = 0, mirrors = 0;
  bool in_char = false, has_descs = false, has_cccd = false;
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
    if (handle == 0 || handle <= previous_handle) return -1;
    previous_handle = handle;

    /* A declaration must be followed by its exact value, even if the value
     * UUID happens to equal a declaration UUID. Never treat a mismatch as a
     * descriptor or silently abandon it at a new service / terminator. */
    if (expected_handle) {
      if (handle != expected_handle || uuid_len != expected_uuid_len ||
          memcmp(uuid, expected_uuid, uuid_len) != 0) return -1;
      if (profile_permissions_supported(flags) < 0 ||
          (flags & ~props & (BLOB_FLAG_OPERATIONS | BLOB_FLAG_SIGNED_WRITE))) return -1;
      expected_handle = 0;
      if ((flags & (BLOB_FLAG_DYNAMIC | BLOB_FLAG_READ)) ==
          (BLOB_FLAG_DYNAMIC | BLOB_FLAG_READ)) mirrors++;
    } else if (u16 == GATT_PRIMARY_SERVICE_UUID || u16 == GATT_SECONDARY_SERVICE_UUID) {
      if (value_len != 2 && value_len != 4 && value_len != 16) return -1;
      if ((flags & BLOB_FLAG_OPERATIONS) != BLOB_FLAG_READ ||
          (flags & (BLOB_FLAG_READ_LEVEL | BLOB_FLAG_WRITE_LEVEL | BLOB_FLAG_KEY_SIZE |
                    BLOB_FLAG_READ_SC | BLOB_FLAG_WRITE_SC | BLOB_FLAG_SIGNED_WRITE))) return -1;
      if (has_descs) descs++; /* zero descriptor terminator */
      services++;
      uuids++;
      chars++; /* each service requires a zero characteristic terminator */
      in_char = has_descs = has_cccd = false;
    } else if (u16 == GATT_CHARACTERISTIC_UUID) {
      if (services == 0 || (value_len != 5 && value_len != 19)) return -1;
      if (has_descs) descs++;
      has_descs = has_cccd = false;
      if ((flags & BLOB_FLAG_OPERATIONS) != BLOB_FLAG_READ ||
          (flags & (BLOB_FLAG_READ_LEVEL | BLOB_FLAG_WRITE_LEVEL | BLOB_FLAG_KEY_SIZE |
                    BLOB_FLAG_READ_SC | BLOB_FLAG_WRITE_SC | BLOB_FLAG_SIGNED_WRITE))) return -1;
      props = value[0];
      expected_handle = get_le16(value + 1);
      if (expected_handle <= handle) return -1;
      expected_uuid = value + 3;
      expected_uuid_len = value_len - 3;
      in_char = true;
      chars++;
      maps++;
      uuids++;
    } else {
      if (!in_char) return -1;
      /* Include-service declarations have no translation in this adapter.
       * Ordinary descriptors, including vendor UUIDs, remain supported. */
      if (u16 == 0x2802) return -1;
      if (u16 == GATT_CCCD_UUID) {
        /* NimBLE supplies this descriptor from NOTIFY / INDICATE. Only the
         * single, initially disabled two-byte form can be coalesced exactly. */
        if (has_cccd || !(props & 0x30) || value_len != 2 ||
            value[0] != 0 || value[1] != 0) return -1;
        if (profile_cccd_supported(flags) < 0) return -1;
        has_cccd = true;
      } else {
        if (profile_permissions_supported(flags) < 0) return -1;
        has_descs = true;
        descs++;
        maps++;
        uuids++;
        if ((flags & (BLOB_FLAG_DYNAMIC | BLOB_FLAG_READ)) ==
            (BLOB_FLAG_DYNAMIC | BLOB_FLAG_READ)) mirrors++;
      }
    }
    if (services > MAX_SVCS || chars > MAX_CHR_SLOTS ||
        descs > MAX_DSC_SLOTS || maps > MAX_ATTRS ||
        uuids > MAX_UUIDS || mirrors > RDM_SLOTS) return -1;
  }
  if (expected_handle) return -1;
  if (has_descs) descs++;
  return descs <= MAX_DSC_SLOTS ? 0 : -1;
}

static int
build_att_db(const uint8_t *db, size_t db_len)
{
  if (BLE_profile_extent(db, db_len, &db_len) < 0) return -1;
  attr_map_count = 0;
  rdm_used = 0;
  svc_count = chr_slot_count = dsc_slot_count = uuid_count = 0;
  memset(svc_defs, 0, sizeof(svc_defs));
  memset(chr_slots, 0, sizeof(chr_slots));
  memset(dsc_slots, 0, sizeof(dsc_slots));

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
      cur_chr->flags |= profile_chr_security(flags);
      cur_chr->min_key_size = profile_min_key_size(flags);
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
      dsc->min_key_size = profile_min_key_size(flags);
      dsc->access_cb = gatt_access_cb;
      dsc->arg = m;
      if (dsc->uuid == NULL) return -1;
    }
  }

  if (cur_chr && cur_chr->descriptors && alloc_dsc() == NULL) return -1;
  if (cur_svc && alloc_chr() == NULL) return -1;
  have_services = (svc_count > 0);
  return 0;
}

/* Preflight leaves the prior registration intact on input rejection. A
 * defensive build failure must never expose a partly constructed database. */
static int
parse_att_db(const uint8_t *db, size_t db_len)
{
  if (validate_att_topology(db, db_len) < 0) return -1;
  if (db == NULL) return -1; /* no-database init does not invoke the builder */
  if (build_att_db(db, db_len) == 0) return 0;
  attr_map_count = rdm_used = 0;
  svc_count = chr_slot_count = dsc_slot_count = uuid_count = 0;
  have_services = false;
  memset(attr_map, 0, sizeof(attr_map));
  memset(svc_defs, 0, sizeof(svc_defs));
  memset(chr_slots, 0, sizeof(chr_slots));
  memset(dsc_slots, 0, sizeof(dsc_slots));
  memset(uuid_pool, 0, sizeof(uuid_pool));
  memset(rdm_len, 0, sizeof(rdm_len));
  memset(rdm_valid, 0, sizeof(rdm_valid));
  return -1;
}

static void reset_characteristic_discoveries(void);

static int
register_services(void)
{
  if (picoruby_ble_peripheral_setup() != 0) return -1;
  // setup runs after SDK init has discarded the previous GATT procedures.
  reset_characteristic_discoveries();
  picoruby_ble_central_reset_request();
  if (!have_services) return 0;
  int rc = ble_gatts_count_cfg(svc_defs);
  if (rc == 0) rc = ble_gatts_add_svcs(svc_defs);
  return rc;
}

/* Losing any required result also invalidates a later successful terminal. */
static void
enqueue_control_event(const uint8_t *pkt, uint16_t len)
{
  if (picoruby_nimble_enqueue_event(pkt, len, PICORUBY_NIMBLE_EVENT_CONTROL) != PICORUBY_NIMBLE_EVENT_ACCEPTED) {
    picoruby_nimble_cancel_control(PICORUBY_NIMBLE_CONTROL_WORK_LOST);
  }
}

static void
picoruby_ble_synth_state_working(void)
{
  uint8_t p[3] = { EVT_BTSTACK_STATE, 1, HCI_STATE_WORKING };
  enqueue_control_event(p, sizeof(p));
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
  return (status & 0xff) ? (uint8_t)status : 0xff;
}

static void
synth_le_connection_complete(uint16_t handle, uint8_t status)
{
  struct ble_gap_conn_desc desc;
  uint8_t p[21];
  memset(p, 0, sizeof(p));
  p[0] = EVT_LE_META;
  p[1] = 19;
  p[2] = 0x01;
  p[3] = status;
  put_le16(p + 4, handle);
  if (status == 0 && ble_gap_conn_find(handle, &desc) == 0) {
    p[6] = (desc.role == BLE_GAP_ROLE_MASTER) ? 0 : 1;
    p[7] = desc.peer_id_addr.type;
    memcpy(p + 8, desc.peer_id_addr.val, 6);
    put_le16(p + 14, desc.conn_itvl);
    put_le16(p + 16, desc.conn_latency);
    put_le16(p + 18, desc.supervision_timeout);
  }
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
  uint8_t p[14 + 31];
  uint8_t dlen = d->length_data > 31 ? 31 : d->length_data;
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

int
picoruby_ble_gap_event(struct ble_gap_event *event, void *arg)
{
  (void)arg;
  if (!picoruby_ble_connection_gate_enter()) return 0;
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        if (!picoruby_ble_connection_claim(event->connect.conn_handle)) {
          /* The SDK can admit more peers than this public adapter supports. */
          int rc = ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
          if (rc != 0) {
            ESP_LOGE(BLE_TAG, "extra connection termination failed: %d", rc);
            picoruby_nimble_cancel_control(PICORUBY_NIMBLE_CONTROL_CONNECTION_LOST);
          }
          break;
        }
        picoruby_ble_central_reset_request();
        if (role == BLE_ROLE_CENTRAL) {
          synth_le_connection_complete(event->connect.conn_handle, 0);
        }
      } else if (role == BLE_ROLE_CENTRAL) {
        picoruby_ble_connection_end_connect();
        uint8_t status = (uint8_t)event->connect.status;
        synth_le_connection_complete(0, status ? status : 0xff);
      } else if (role == BLE_ROLE_PERIPHERAL || role == BLE_ROLE_BROADCASTER) {
        picoruby_ble_peripheral_rearm_adv();
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      if (!picoruby_ble_connection_release(event->disconnect.conn.conn_handle)) break;
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
      if (role == BLE_ROLE_PERIPHERAL && picoruby_ble_connection_matches(event->mtu.conn_handle)) {
        synth_mtu_exchange_complete(event->mtu.conn_handle, event->mtu.value);
      }
      break;
    case BLE_GAP_EVENT_SUBSCRIBE: {
      if (role != BLE_ROLE_PERIPHERAL || !picoruby_ble_connection_matches(event->subscribe.conn_handle)) break;
      attr_map_t *e = find_map_by_nimble(event->subscribe.attr_handle);
      if (e && e->cccd_ruby_handle) {
        uint8_t v[2] = { 0x00, 0x00 };
        if (event->subscribe.cur_notify) v[0] |= 0x01;
        if (event->subscribe.cur_indicate) v[0] |= 0x02;
        if (picoruby_nimble_enqueue_write(e->cccd_ruby_handle, v, 2) != 0) {
          picoruby_nimble_cancel_control(PICORUBY_NIMBLE_CONTROL_WORK_LOST);
        }
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
        picoruby_nimble_record_oversized_event();
        ESP_LOGW(BLE_TAG, "notification exceeds event capacity: %u", (unsigned)vlen);
        break;
      }
      uint8_t p[GATT_EVENT_PAYLOAD_OFFSET + 4 + VALUE_EVENT_DATA_MAX];
      gatt_event_header(p, event->notify_rx.indication ? EVT_GATT_INDICATION : EVT_GATT_NOTIFICATION,
                        (uint8_t)(GATT_EVENT_PAYLOAD_OFFSET + 2 + vlen), event->notify_rx.conn_handle);
      put_le16(p + GATT_EVENT_PAYLOAD_OFFSET, event->notify_rx.attr_handle);
      put_le16(p + GATT_EVENT_PAYLOAD_OFFSET + 2, vlen);
      if (os_mbuf_copydata(event->notify_rx.om, 0, vlen, p + GATT_EVENT_PAYLOAD_OFFSET + 4) != 0) {
        picoruby_nimble_record_copy_failure();
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
  return 0;
}

/* The owning host loop calls this before releasing the gate after every
 * event. SDK internal retry paths can delete a connection without delivering
 * an application DISCONNECT. Missing SDK state is a fact; its HCI cause is
 * not inferred, so the synthesized reason is explicitly unspecified. */
void
picoruby_ble_reconcile_connections(void)
{
  uint16_t handle = picoruby_ble_connection_handle_snapshot();
  if (handle != 0xffff && ble_gap_conn_find(handle, NULL) == BLE_HS_ENOTCONN &&
      picoruby_ble_connection_release(handle)) {
    picoruby_ble_central_reset_request();
    synth_disconnection_complete(handle, BLE_ERR_UNSPECIFIED);
    picoruby_nimble_cancel_control(PICORUBY_NIMBLE_CONTROL_CONNECTION_LOST);
    if (role == BLE_ROLE_PERIPHERAL) picoruby_ble_peripheral_rearm_adv();
  }
  if (role == BLE_ROLE_CENTRAL && picoruby_ble_central_reconcile_connect()) {
    synth_le_connection_complete(0, status_to_u8(BLE_HS_ENOTCONN));
    picoruby_nimble_cancel_control(PICORUBY_NIMBLE_CONTROL_CONNECTION_LOST);
  }
}

int
BLE_init(const uint8_t *profile_data, size_t profile_len, int ble_role)
{
  size_t len;
  if (BLE_profile_extent(profile_data, profile_len, &len) < 0) return -1;
  if (validate_att_topology(profile_data, len) < 0) return -1;
  if (picoruby_nimble_cleanup_blocked()) return -1;
  uint8_t *replacement = NULL;
  if (profile_data) {
    replacement = (uint8_t *)malloc(len);
    if (replacement == NULL) return -1;
    memcpy(replacement, profile_data, len);
  }
  if (picoruby_nimble_started() && picoruby_nimble_stop() != 0) {
    free(replacement);
    return -1;
  }
  role = (enum BLE_role_t)ble_role;
  power_on_requested = false;
  picoruby_ble_connection_close();
  have_services = false;
  attr_map_count = 0;
  rdm_used = 0;

  free(owned_profile_data);
  owned_profile_data = replacement;
  if (replacement && parse_att_db(replacement, len) < 0) {
    free(owned_profile_data);
    owned_profile_data = NULL;
    return -1;
  }
  return picoruby_nimble_start(register_services);
}

int
BLE_hci_power_control(uint8_t power_mode)
{
  switch (power_mode) {
    case 0: /* HCI_POWER_OFF */
    case 1: /* HCI_POWER_ON */
      break;
    case 2: /* HCI_POWER_SLEEP */
      return BLE_POWER_CONTROL_UNSUPPORTED;
    default:
      return BLE_POWER_CONTROL_INVALID;
  }
  if (!picoruby_nimble_ready() && (power_mode != 0 || !picoruby_nimble_started())) {
    power_on_requested = false;
    picoruby_nimble_heartbeat_enable(false);
    return power_mode == 0 ? 0 : BLE_POWER_CONTROL_NOT_READY;
  }
  if (power_mode == 1) {
    if (!power_on_requested) {
      picoruby_nimble_heartbeat_enable(true);
      power_on_requested = true;
    }
    picoruby_ble_synth_state_working();
  } else {
    power_on_requested = false;
    picoruby_nimble_heartbeat_enable(false);
    switch (role) {
      case BLE_ROLE_PERIPHERAL:
      case BLE_ROLE_BROADCASTER:
        BLE_peripheral_stop_advertise();
        break;
      case BLE_ROLE_CENTRAL:
      case BLE_ROLE_OBSERVER:
        ble_gap_disc_cancel();
        break;
      default:
        break;
    }
  }
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
  if (rc != 0) return rc;
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

// One discovery per connection, matching the RP2040 request boundary.
static characteristic_discovery_t chr_discoveries[MYNEWT_VAL(BLE_MAX_CONNECTIONS)];
static portMUX_TYPE chr_disc_mux = portMUX_INITIALIZER_UNLOCKED;

/* Protected by the connection gate. No callback owns Ruby data or an mbuf.
 * Match RP2040's one pending request without using NimBLE's ATT busy queue. */
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
  /* The SDK invokes timeout callbacks before initiating disconnect. */
  if (status == BLE_HS_ETIMEOUT) {
    request_state = GATT_REQUEST_WAIT_DISCONNECT;
    return;
  }
  request_state = GATT_REQUEST_IDLE;
}

static void
gatt_request_returned(uint16_t conn, int rc)
{
  /* Inline terminal callbacks may already have completed or closed the request.
   * The shared gate and enqueue-only callbacks preclude a new request here. */
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
    synth_query_complete(conn, status_to_u8(error->status));
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
    if (discovery->have_prev) {
      emit_characteristic(conn, &discovery->prev, discovery->end_handle);
      discovery->have_prev = false;
    }
    // No discovery fields are accessed after making the slot reusable.
    taskENTER_CRITICAL(&chr_disc_mux);
    discovery->active = false;
    taskEXIT_CRITICAL(&chr_disc_mux);
    gatt_request_complete(conn, error->status);
    synth_query_complete(conn, status_to_u8(error->status));
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
    synth_query_complete(conn, status_to_u8(error->status));
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
      synth_query_complete(conn, status_to_u8(BLE_HS_EMSGSIZE));
      return 0;
    }
    uint8_t p[GATT_EVENT_PAYLOAD_OFFSET + 4 + VALUE_EVENT_DATA_MAX];
    gatt_event_header(p, EVT_GATT_CHARACTERISTIC_VALUE_QUERY_RESULT,
                      (uint8_t)(GATT_EVENT_PAYLOAD_OFFSET + 2 + vlen), conn);
    put_le16(p + GATT_EVENT_PAYLOAD_OFFSET, attr->handle);
    put_le16(p + GATT_EVENT_PAYLOAD_OFFSET + 2, vlen);
    if (os_mbuf_copydata(attr->om, 0, vlen, p + GATT_EVENT_PAYLOAD_OFFSET + 4) != 0) {
      // mbuf の -1 を NimBLE の引数不正へ明示変換する。
      synth_query_complete(conn, status_to_u8(BLE_HS_EINVAL));
      return 0;
    }
    enqueue_control_event(p, GATT_EVENT_PAYLOAD_OFFSET + 4 + vlen);
  }
  synth_query_complete(conn, status_to_u8(error->status));
  return 0;
}

static int
write_attr_cb(uint16_t conn, const struct ble_gatt_error *error,
              struct ble_gatt_attr *attr, void *arg)
{
  (void)attr;
  (void)arg;
  gatt_request_complete(conn, error->status);
  synth_query_complete(conn, status_to_u8(error->status));
  return 0;
}

static uint8_t
rc_to_u8(int rc)
{
  if (rc == 0) return 0;
  return (rc & 0xff) ? (uint8_t)rc : 0xff;
}

static uint8_t
BLE_discover_primary_services_on_connection(uint16_t conn_handle)
{
  int rc = gatt_request_begin(conn_handle);
  if (rc != 0) return rc_to_u8(rc);
  rc = ble_gattc_disc_all_svcs(conn_handle, disc_svc_cb, NULL);
  gatt_request_returned(conn_handle, rc);
  return rc_to_u8(rc);
}

static uint8_t
BLE_discover_characteristics_for_service_on_connection(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle)
{
  int rc = gatt_request_begin(conn_handle);
  if (rc != 0) return rc_to_u8(rc);
  characteristic_discovery_t *discovery = NULL;
  taskENTER_CRITICAL(&chr_disc_mux);
  for (int i = 0; i < MYNEWT_VAL(BLE_MAX_CONNECTIONS); i++) {
    characteristic_discovery_t *slot = &chr_discoveries[i];
    if (slot->active && slot->conn_handle == conn_handle) {
      taskEXIT_CRITICAL(&chr_disc_mux);
      gatt_request_complete(conn_handle, 0);
      return rc_to_u8(BLE_HS_EBUSY);
    }
    if (!slot->active && discovery == NULL) discovery = slot;
  }
  if (discovery == NULL) {
    taskEXIT_CRITICAL(&chr_disc_mux);
    gatt_request_complete(conn_handle, 0);
    return rc_to_u8(BLE_HS_EBUSY);
  }
  discovery->conn_handle = conn_handle;
  discovery->end_handle = end_handle;
  discovery->have_prev = false;
  discovery->active = true;
  taskEXIT_CRITICAL(&chr_disc_mux);

  rc = ble_gattc_disc_all_chrs(conn_handle, start_handle, end_handle,
                             disc_chr_cb, discovery);
  if (rc != 0) {
    taskENTER_CRITICAL(&chr_disc_mux);
    discovery->active = false;
    taskEXIT_CRITICAL(&chr_disc_mux);
  }
  gatt_request_returned(conn_handle, rc);
  return rc_to_u8(rc);
}

static uint8_t
BLE_read_value_of_characteristic_using_value_handle_on_connection(uint16_t conn_handle, uint16_t value_handle)
{
  int rc = gatt_request_begin(conn_handle);
  if (rc != 0) return rc_to_u8(rc);
  rc = ble_gattc_read(conn_handle, value_handle, read_attr_cb, NULL);
  gatt_request_returned(conn_handle, rc);
  return rc_to_u8(rc);
}

static uint8_t
BLE_discover_characteristic_descriptors_on_connection(uint16_t conn_handle, uint16_t value_handle, uint16_t end_handle)
{
  int rc = gatt_request_begin(conn_handle);
  if (rc != 0) return rc_to_u8(rc);
  rc = ble_gattc_disc_all_dscs(conn_handle, value_handle, end_handle, disc_dsc_cb, NULL);
  gatt_request_returned(conn_handle, rc);
  return rc_to_u8(rc);
}

static uint8_t
BLE_write_value_of_characteristic_without_response_on_connection(uint16_t conn_handle, uint16_t value_handle, const uint8_t *data, uint16_t size)
{
  int rc = picoruby_ble_att_payload_status(conn_handle, size);
  if (rc != 0) return rc_to_u8(rc);
  return rc_to_u8(ble_gattc_write_no_rsp_flat(conn_handle, value_handle, data, size));
}

static uint8_t
BLE_write_characteristic_descriptor_using_descriptor_handle_on_connection(uint16_t conn_handle, uint16_t descriptor_handle, const uint8_t *data, uint16_t size)
{
  int rc = picoruby_ble_att_payload_status(conn_handle, size);
  if (rc != 0) return rc_to_u8(rc);
  rc = gatt_request_begin(conn_handle);
  if (rc != 0) return rc_to_u8(rc);
  rc = ble_gattc_write_flat(conn_handle, descriptor_handle, data, size, write_attr_cb, NULL);
  gatt_request_returned(conn_handle, rc);
  return rc_to_u8(rc);
}

uint8_t
BLE_discover_primary_services(uint16_t conn_handle)
{
  if (!picoruby_ble_connection_gate_enter()) return rc_to_u8(BLE_HS_ENOTCONN);
  if (!picoruby_ble_connection_matches(conn_handle)) {
    picoruby_ble_connection_gate_exit();
    return rc_to_u8(BLE_HS_ENOTCONN);
  }
  uint8_t rc = BLE_discover_primary_services_on_connection(conn_handle);
  picoruby_ble_connection_gate_exit();
  return rc;
}

uint8_t
BLE_discover_characteristics_for_service(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle)
{
  if (!picoruby_ble_connection_gate_enter()) return rc_to_u8(BLE_HS_ENOTCONN);
  if (!picoruby_ble_connection_matches(conn_handle)) {
    picoruby_ble_connection_gate_exit();
    return rc_to_u8(BLE_HS_ENOTCONN);
  }
  uint8_t rc = BLE_discover_characteristics_for_service_on_connection(conn_handle, start_handle, end_handle);
  picoruby_ble_connection_gate_exit();
  return rc;
}

uint8_t
BLE_read_value_of_characteristic_using_value_handle(uint16_t conn_handle, uint16_t value_handle)
{
  if (!picoruby_ble_connection_gate_enter()) return rc_to_u8(BLE_HS_ENOTCONN);
  if (!picoruby_ble_connection_matches(conn_handle)) {
    picoruby_ble_connection_gate_exit();
    return rc_to_u8(BLE_HS_ENOTCONN);
  }
  uint8_t rc = BLE_read_value_of_characteristic_using_value_handle_on_connection(conn_handle, value_handle);
  picoruby_ble_connection_gate_exit();
  return rc;
}

uint8_t
BLE_discover_characteristic_descriptors(uint16_t conn_handle, uint16_t value_handle, uint16_t end_handle)
{
  if (!picoruby_ble_connection_gate_enter()) return rc_to_u8(BLE_HS_ENOTCONN);
  if (!picoruby_ble_connection_matches(conn_handle)) {
    picoruby_ble_connection_gate_exit();
    return rc_to_u8(BLE_HS_ENOTCONN);
  }
  uint8_t rc = BLE_discover_characteristic_descriptors_on_connection(conn_handle, value_handle, end_handle);
  picoruby_ble_connection_gate_exit();
  return rc;
}

uint8_t
BLE_write_value_of_characteristic_without_response(uint16_t conn_handle, uint16_t value_handle, const uint8_t *data, uint16_t size)
{
  if (!picoruby_ble_connection_gate_enter()) return rc_to_u8(BLE_HS_ENOTCONN);
  if (!picoruby_ble_connection_matches(conn_handle)) {
    picoruby_ble_connection_gate_exit();
    return rc_to_u8(BLE_HS_ENOTCONN);
  }
  uint8_t rc = BLE_write_value_of_characteristic_without_response_on_connection(conn_handle, value_handle, data, size);
  picoruby_ble_connection_gate_exit();
  return rc;
}

uint8_t
BLE_write_characteristic_descriptor_using_descriptor_handle(uint16_t conn_handle, uint16_t descriptor_handle, const uint8_t *data, uint16_t size)
{
  if (!picoruby_ble_connection_gate_enter()) return rc_to_u8(BLE_HS_ENOTCONN);
  if (!picoruby_ble_connection_matches(conn_handle)) {
    picoruby_ble_connection_gate_exit();
    return rc_to_u8(BLE_HS_ENOTCONN);
  }
  uint8_t rc = BLE_write_characteristic_descriptor_using_descriptor_handle_on_connection(conn_handle, descriptor_handle, data, size);
  picoruby_ble_connection_gate_exit();
  return rc;
}
