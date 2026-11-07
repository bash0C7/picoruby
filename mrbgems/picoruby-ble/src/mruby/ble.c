#include "mruby.h"
#include "mruby/presym.h"
#include "mruby/string.h"
#include "mruby/hash.h"
#include "mruby/array.h"
#include "mruby/data.h"
#include "mruby/error.h"
#include "task.h"

static mrb_state *_mrb = NULL;
static mrb_value registered_ble;
static bool ble_registered = false;
static mrb_value write_values;
static mrb_value read_values;
static mrb_value event_queue;
static uint8_t pending_event_count;
/* C-heap copy of the GATT profile. BTstack keeps a raw pointer to it
 * indefinitely, which is why it is not owned by a Ruby object. */
static uint8_t *profile_buf = NULL;

#define BLE_MAX_PENDING_EVENTS 16

typedef struct {
  mrb_value values[4];
  int first;
  int count;
} ble_root_staging_t;

static mrb_value
mrb_ble_stage_roots(mrb_state *mrb, void *userdata)
{
  ble_root_staging_t *staging = (ble_root_staging_t *)userdata;
  while (staging->first + staging->count < 4) {
    mrb_gc_register(mrb, staging->values[staging->first + staging->count]);
    staging->count++;
  }
  return mrb_nil_value();
}

static void
mrb_ble_unstage_roots(mrb_state *mrb, ble_root_staging_t *staging)
{
  while (staging->count > 0) {
    staging->count--;
    mrb_gc_unregister(mrb, staging->values[staging->first + staging->count]);
  }
}

bool
mrb_ble_active_owner(mrb_state *mrb, mrb_value self)
{
  return ble_registered && _mrb == mrb &&
         mrb_obj_eq(mrb, registered_ble, self) &&
         mrb_true_p(mrb_iv_get(mrb, self, MRB_IVSYM(ble_initialized)));
}

void
mrb_ble_check_active_owner(mrb_state *mrb, mrb_value self)
{
  if (!mrb_ble_active_owner(mrb, self)) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "BLE instance is not the active initialized owner");
  }
}

static mrb_value
mrb_check_registration(mrb_state *mrb, mrb_value self)
{
  mrb_value expected;
  mrb_get_args(mrb, "o", &expected);
  mrb_ble_check_active_owner(mrb, self);
  if (!mrb_obj_eq(mrb, expected, event_queue)) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "BLE start owner belongs to an earlier initialization");
  }
  return mrb_nil_value();
}

static mrb_value
mrb_power_control_registration(mrb_state *mrb, mrb_value self)
{
  mrb_value expected;
  mrb_int mode;
  mrb_get_args(mrb, "oi", &expected, &mode);
  if (mode < 0 || mode > 1) mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid BLE power mode");
  if (!mrb_ble_active_owner(mrb, self) || !mrb_obj_eq(mrb, expected, event_queue)) {
    return mrb_false_value();
  }
  if (mode != 0 && !BLE_ready()) mrb_raise(mrb, E_RUNTIME_ERROR, "BLE init failed");
  if (BLE_hci_power_control((uint8_t)mode) != 0) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "BLE power control failed");
  }
  return mrb_true_value();
}

static bool
ble_event_queue_has_capacity(void)
{
  if (_mrb == NULL || !mrb_data_p(event_queue)) return false;
  return pending_event_count < BLE_MAX_PENDING_EVENTS;
}

int
BLE_push_event(uint8_t *data, uint16_t size)
{
  if (!ble_event_queue_has_capacity()) return -1;
  int result = -1;
  int ai = mrb_gc_arena_save(_mrb);
  mrb_value event = mrb_str_new(_mrb, (const char *)data, size);
  if (mrb_task_queue_push(_mrb, event_queue, event) == MRB_TASK_QUEUE_PUSH_OK) {
    pending_event_count++;
    result = 0;
  }
  mrb_gc_arena_restore(_mrb, ai);
  return result;
}

int
BLE_heartbeat(void)
{
  if (!ble_event_queue_has_capacity()) return -1;
  mrb_state *mrb = _mrb;
  if (mrb_task_queue_push(mrb, event_queue, mrb_symbol_value(MRB_SYM(heartbeat))) ==
      MRB_TASK_QUEUE_PUSH_OK) {
    pending_event_count++;
    return 0;
  }
  return -1;
}

static mrb_value
mrb_event_popped(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  if (pending_event_count > 0) pending_event_count--;
  return mrb_nil_value();
}

static mrb_value
mrb_event_queue_cleared(mrb_state *mrb, mrb_value self)
{
  mrb_value expected = event_queue;
  mrb_get_args(mrb, "|o", &expected);
  mrb_ble_check_active_owner(mrb, self);
  if (!mrb_obj_eq(mrb, expected, event_queue)) mrb_raise(mrb, E_RUNTIME_ERROR, "BLE owner registration changed");
  if (_mrb != mrb || !mrb_data_p(event_queue)) {
    mrb_raise(mrb, E_TYPE_ERROR, "BLE event queue is not initialized");
  }
  mrb_funcall_id(mrb, event_queue, MRB_SYM(clear), 0);
  pending_event_count = 0;
  BLE_events_cleared();
  return mrb_nil_value();
}

int
BLE_write_data(uint16_t att_handle, const uint8_t *data, uint16_t size)
{
  if (att_handle == 0 || size == 0 || _mrb == NULL || mrb_hash_p(write_values) == false) {
    return -1;
  }
  mrb_value key = mrb_int_value(_mrb, att_handle);
  int ai = mrb_gc_arena_save(_mrb);
  mrb_value write_value = mrb_str_new(_mrb, (const char *)data, size);
  mrb_value queue = mrb_hash_get(_mrb, write_values, key);
  if (!mrb_array_p(queue)) {
    queue = mrb_ary_new_capa(_mrb, 4);
    mrb_hash_set(_mrb, write_values, key, queue);
  }
  mrb_ary_push(_mrb, queue, write_value);
  mrb_gc_arena_restore(_mrb, ai);
  return 0;
}

int
BLE_read_data(BLE_read_value_t *read_value)
{
  if (_mrb == NULL || mrb_hash_p(read_values) == false) return -1;
  mrb_value value = mrb_hash_get(_mrb, read_values, mrb_int_value(_mrb, read_value->att_handle));
  if (mrb_string_p(value) == false) return -1;
  if (RSTRING_LEN(value) > UINT16_MAX) return -1;
  read_value->data = (uint8_t *)RSTRING_PTR(value);
  read_value->size = (uint16_t)RSTRING_LEN(value);
  return 0;
}


static mrb_value
mrb_pop_write_value(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  mrb_int handle;
  mrb_get_args(mrb, "i", &handle);
  if (handle <= 0 || handle > UINT16_MAX) return mrb_nil_value();
  mrb_value key = mrb_int_value(mrb, handle);
  mrb_value queue = mrb_hash_get(mrb, write_values, key);
  if (!mrb_array_p(queue) || RARRAY_LEN(queue) == 0) {
    return mrb_nil_value();
  }
  return mrb_ary_shift(mrb, queue);
}

static mrb_value
mrb_push_read_value(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  mrb_int handle;
  mrb_value read_value;
  mrb_get_args(mrb, "iS", &handle, &read_value);
  if (handle < 1 || handle > UINT16_MAX) mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid ATT handle");
  if (RSTRING_LEN(read_value) > UINT16_MAX) mrb_raise(mrb, E_ARGUMENT_ERROR, "BLE read value exceeds 65535 bytes");
  mrb_hash_set(mrb, read_values, mrb_int_value(mrb, handle), read_value);
  BLE_read_value_pushed((uint16_t)handle, (const uint8_t *)RSTRING_PTR(read_value), (uint16_t)RSTRING_LEN(read_value));
  return read_value;
}

static mrb_value
mrb__init(mrb_state *mrb, mrb_value self)
{
  mrb_value profile;
  mrb_get_args(mrb, "o", &profile);
  if (ble_registered && _mrb != mrb) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "BLE belongs to another VM");
  }
  mrb_value new_event_queue = mrb_iv_get(mrb, self, MRB_IVSYM(event_queue));
  mrb_value new_write_values = mrb_iv_get(mrb, self, MRB_IVSYM(write_values));
  mrb_value new_read_values = mrb_iv_get(mrb, self, MRB_IVSYM(read_values));
  if (!mrb_data_p(new_event_queue) || !mrb_hash_p(new_write_values) || !mrb_hash_p(new_read_values)) {
    mrb_raise(mrb, E_TYPE_ERROR, "BLE._init: instance stores not initialized");
  }

  if (!mrb_string_p(profile) && !mrb_nil_p(profile)) {
    mrb_raise(mrb, E_TYPE_ERROR, "BLE._init: wrong argument type");
  }
  int ble_role;
  mrb_value role = mrb_iv_get(mrb, self, MRB_IVSYM(role));
  if (!mrb_symbol_p(role)) {
    mrb_raise(mrb, E_TYPE_ERROR, "BLE._init: wrong role type");
  }
  switch (mrb_symbol(role)) {
    case MRB_SYM(central):
      ble_role = BLE_ROLE_CENTRAL;
      break;
    case MRB_SYM(peripheral):
      ble_role = BLE_ROLE_PERIPHERAL;
      break;
    case MRB_SYM(observer):
      ble_role = BLE_ROLE_OBSERVER;
      break;
    case MRB_SYM(broadcaster):
      ble_role = BLE_ROLE_BROADCASTER;
      break;
    default:
      mrb_raise(mrb, E_TYPE_ERROR, "BLE._init: wrong role type");
  }

  uint8_t *new_profile = NULL;
  size_t new_profile_len = 0;
  if (ble_role == BLE_ROLE_PERIPHERAL && mrb_string_p(profile)) {
    new_profile_len = (size_t)RSTRING_LEN(profile);
    if (BLE_profile_extent((const uint8_t *)RSTRING_PTR(profile), new_profile_len, NULL) < 0) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "BLE._init: invalid profile");
    }
    new_profile = (uint8_t *)mrb_malloc(mrb, new_profile_len);
    memcpy(new_profile, RSTRING_PTR(profile), new_profile_len);
  }
  mrb_value prev_ble = registered_ble;
  mrb_value prev_event_queue = event_queue;
  mrb_value prev_write_values = write_values;
  mrb_value prev_read_values = read_values;
  bool release_graphs = ble_registered;
  bool same_instance = ble_registered && mrb_obj_eq(mrb, prev_ble, self);
  bool release_prev = ble_registered && !same_instance;
  ble_root_staging_t staging = {
    .values = { self, new_event_queue, new_write_values, new_read_values },
    .first = same_instance ? 1 : 0,
    .count = 0
  };
  mrb_bool root_error = false;
  mrb_value root_result = mrb_protect_error(mrb, mrb_ble_stage_roots, &staging, &root_error);
  if (root_error) {
    mrb_ble_unstage_roots(mrb, &staging);
    if (new_profile) mrb_free(mrb, new_profile);
    mrb_exc_raise(mrb, root_result);
  }
  if (BLE_init(new_profile, new_profile_len, ble_role) < 0) {
    mrb_ble_unstage_roots(mrb, &staging);
    if (new_profile) mrb_free(mrb, new_profile);
    mrb_raise(mrb, E_RUNTIME_ERROR, "BLE init failed");
  }
  /* BLE_init() replaces the ATT database for every role (possibly
   * with no database), so the previous profile is unreferenced now
   * and must be released even when the new role has none. */
  if (profile_buf) mrb_free(mrb, profile_buf);
  profile_buf = new_profile;

  registered_ble = self;
  ble_registered = true;

  _mrb = mrb;
  event_queue = new_event_queue;
  pending_event_count = 0;
  write_values = new_write_values;
  read_values = new_read_values;
  BLE_vm_attached(mrb);

  if (release_graphs) {
    mrb_gc_unregister(mrb, prev_event_queue);
    mrb_gc_unregister(mrb, prev_write_values);
    mrb_gc_unregister(mrb, prev_read_values);
  }
  if (release_prev) {
    mrb_gc_unregister(mrb, prev_ble);
  }
  return self;
}

static mrb_value
mrb_control_fault(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  mrb_get_args(mrb, "");
  return mrb_int_value(mrb, BLE_control_fault());
}

static mrb_value
mrb_native_ready(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  return mrb_bool_value(BLE_ready());
}

static mrb_value
mrb_hci_power_control(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  mrb_int power_mode;
  mrb_get_args(mrb, "i", &power_mode);
  if (power_mode < 0 || power_mode > 2) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid BLE power mode");
  }
  if (power_mode != 0 && !BLE_ready()) mrb_raise(mrb, E_RUNTIME_ERROR, "BLE init failed");
  int result = BLE_hci_power_control((uint8_t)power_mode);
  if (result != 0) mrb_raise(mrb, E_RUNTIME_ERROR, "BLE power control failed");
  return mrb_int_value(mrb, 0);
}

static mrb_value
mrb_gap_local_bd_addr(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  if (!BLE_ready()) mrb_raise(mrb, E_RUNTIME_ERROR, "BLE init failed");
  uint8_t addr[6];
  if (BLE_gap_local_bd_addr(addr) != 0) mrb_raise(mrb, E_RUNTIME_ERROR, "BLE local address unavailable");
  return mrb_str_new(mrb, (const char *)addr, 6);
}

void
mrb_picoruby_ble_gem_init(mrb_state* mrb)
{
  struct RClass *class_BLE = mrb_define_class_id(mrb, MRB_SYM(BLE), mrb->object_class);

  mrb_define_private_method_id(mrb, class_BLE, MRB_SYM(_native_ready), mrb_native_ready, MRB_ARGS_NONE());
  mrb_define_private_method_id(mrb, class_BLE, MRB_SYM(_init), mrb__init, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(hci_power_control), mrb_hci_power_control, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(gap_local_bd_addr), mrb_gap_local_bd_addr, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(pop_write_value), mrb_pop_write_value, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(push_read_value), mrb_push_read_value, MRB_ARGS_REQ(2));
  mrb_define_private_method_id(mrb, class_BLE, MRB_SYM(_control_fault), mrb_control_fault, MRB_ARGS_NONE());
  mrb_define_private_method_id(mrb, class_BLE, MRB_SYM(_check_registration), mrb_check_registration, MRB_ARGS_REQ(1));
  mrb_define_private_method_id(mrb, class_BLE, MRB_SYM(_power_control_registration), mrb_power_control_registration, MRB_ARGS_REQ(2));
  mrb_define_private_method_id(mrb, class_BLE, MRB_SYM(_event_popped), mrb_event_popped, MRB_ARGS_NONE());
  mrb_define_private_method_id(mrb, class_BLE, MRB_SYM(_event_queue_cleared), mrb_event_queue_cleared, MRB_ARGS_OPT(1));
  mrb_init_class_BLE_Peripheral(mrb, class_BLE);
  mrb_init_class_BLE_Broadcaster(mrb, class_BLE);
  mrb_init_class_BLE_Central(mrb, class_BLE);
}

void
mrb_picoruby_ble_gem_final(mrb_state* mrb)
{
  if (profile_buf) {
    mrb_free(mrb, profile_buf);
    profile_buf = NULL;
  }
  ble_registered = false;
  _mrb = NULL;
}
