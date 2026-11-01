#ifdef PICORB_PLATFORM_ESP32
#include "../../ports/esp32/nimble_owner.h"
#include "../../ports/esp32/ble_common.h"
#endif

/* Keep the identity alive: a raw pointer can be reused after refcount release. */
static mrbc_value registered_ble = {.tt = MRBC_TT_NIL};
static mrbc_value write_values = {.tt = MRBC_TT_NIL};
static mrbc_value read_values = {.tt = MRBC_TT_NIL};
static mrbc_value event_queue = {.tt = MRBC_TT_NIL};
/* C-heap copy of the GATT profile. BTstack keeps a raw pointer to it
 * indefinitely, which is why it is not owned by a Ruby object. */
static uint8_t *profile_buf = NULL;

#define BLE_MAX_PENDING_EVENTS 16

bool
mrbc_ble_active_owner(mrbc_value *self)
{
  if (self->tt != MRBC_TT_OBJECT || registered_ble.tt != MRBC_TT_OBJECT ||
      self->instance != registered_ble.instance) return false;
  mrbc_value initialized = mrbc_instance_getiv(self, mrbc_str_to_symid("ble_initialized"));
  bool active = initialized.tt == MRBC_TT_TRUE;
  mrbc_decref(&initialized);
  return active;
}

bool
mrbc_ble_check_active_owner(mrbc_vm *vm, mrbc_value *self)
{
  if (mrbc_ble_active_owner(self)) return true;
  mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE instance is not the active initialized owner");
  return false;
}

static void
c_active_owner(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (argc != 0) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  SET_BOOL_RETURN(mrbc_ble_active_owner(&v[0]));
}

static void
c_check_active_owner(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (argc != 0) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  SET_NIL_RETURN();
}

static bool
mrbc_same_registration(mrbc_value *expected)
{
  return expected->tt == MRBC_TT_OBJECT && event_queue.tt == MRBC_TT_OBJECT &&
         expected->instance == event_queue.instance;
}

static void
c_check_registration(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (argc != 1 || !mrbc_same_registration(&v[1])) {
    mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE start owner belongs to an earlier initialization");
    return;
  }
  SET_NIL_RETURN();
}

static void
c_power_control_registration(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (argc != 2 || v[2].tt != MRBC_TT_INTEGER || v[2].i < 0 || v[2].i > 1) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "invalid BLE power mode");
    return;
  }
  if (!mrbc_ble_active_owner(&v[0]) || !mrbc_same_registration(&v[1])) {
    SET_FALSE_RETURN();
    return;
  }
  if (v[2].i != 0 && !BLE_ready()) {
    mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE init failed");
    return;
  }
  if (BLE_hci_power_control((uint8_t)v[2].i) != 0) {
    mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE power control failed");
    return;
  }
  SET_TRUE_RETURN();
}

/* Coupled to the pinned Task::Queue @items layout. Direct C access keeps
 * admission and push in one non-yielding call, without Ruby overrides. */
static bool
ble_event_queue_has_capacity(void)
{
  if (event_queue.tt != MRBC_TT_OBJECT) return false;
  mrbc_value items = mrbc_instance_getiv(&event_queue,
                                        mrbc_str_to_symid("@items"));
  bool has_capacity = items.tt == MRBC_TT_ARRAY &&
                      mrbc_array_size(&items) < BLE_MAX_PENDING_EVENTS;
  mrbc_decref(&items);
  return has_capacity;
}

#define NODE_BOX_SIZE 10
#define VM_REGS_SIZE 110 // can be reduced?

int
BLE_push_event(uint8_t *data, uint16_t size)
{
  if (!ble_event_queue_has_capacity()) return -1;
  int result = -1;
  mrbc_value event = mrbc_string_new(NULL, (const void *)data, size);
  switch (mrbc_task_queue_push(&event_queue, &event)) {
    case MRBC_TASK_QUEUE_PUSH_OK:
    case MRBC_TASK_QUEUE_PUSH_OK_WOKE:
      result = 0;
      break;
    default:
      break;
  }
  mrbc_decref(&event);
  return result;
}

int
BLE_heartbeat(void)
{
  if (!ble_event_queue_has_capacity()) return -1;
  mrbc_value event = mrbc_symbol_value(mrbc_str_to_symid("heartbeat"));
  switch (mrbc_task_queue_push(&event_queue, &event)) {
    case MRBC_TASK_QUEUE_PUSH_OK:
    case MRBC_TASK_QUEUE_PUSH_OK_WOKE:
      return 0;
    default:
      return -1;
  }
}

static void
c_event_popped(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (argc != 0) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  /* Compatibility hook: capacity follows the queue, not delayed pop calls. */
  SET_NIL_RETURN();
}

static void
c_event_queue_cleared(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (argc > 1) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  if (argc == 1 && !mrbc_same_registration(&v[1])) {
    mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE owner registration changed");
    return;
  }
  if (event_queue.tt != MRBC_TT_OBJECT) {
    mrbc_raise(vm, MRBC_CLASS(TypeError), "BLE event queue is not initialized");
    return;
  }
  mrbc_value items = mrbc_instance_getiv(&event_queue,
                                        mrbc_str_to_symid("@items"));
  if (items.tt != MRBC_TT_ARRAY) {
    mrbc_decref(&items);
    mrbc_raise(vm, MRBC_CLASS(TypeError), "BLE event queue items must be an Array");
    return;
  }
  /* Clear and native reset stay in one C send; no Ruby clear override. */
  mrbc_array_clear(&items);
  mrbc_decref(&items);
#ifdef PICORB_PLATFORM_ESP32
  picoruby_nimble_reset_events();
#endif
  SET_NIL_RETURN();
}

int
BLE_write_data(uint16_t att_handle, const uint8_t *data, uint16_t size)
{
#ifdef PICORB_PLATFORM_ESP32
  return picoruby_nimble_enqueue_write(att_handle, data, size);
#else
  if (att_handle == 0 || size == 0 || write_values.tt == MRBC_TT_NIL) {
    return -1;
  }
  mrbc_value key = mrbc_integer_value(att_handle);
  mrbc_value write_value = mrbc_string_new(NULL, (const void *)data, size);
  mrbc_value queue = mrbc_hash_get(&write_values, &key);
  if (queue.tt != MRBC_TT_ARRAY) {
    queue = mrbc_array_new(NULL, 4);
    mrbc_hash_set(&write_values, &key, &queue);
  }
  mrbc_array_push(&queue, &write_value);
  return 0;
#endif
}

int
BLE_write_pending(uint16_t att_handle)
{
#ifdef PICORB_PLATFORM_ESP32
  return picoruby_nimble_write_pending(att_handle);
#else
  if (write_values.tt != MRBC_TT_HASH) return 0;
  mrbc_value key = mrbc_integer_value(att_handle);
  mrbc_value queue = mrbc_hash_get(&write_values, &key);
  if (queue.tt != MRBC_TT_ARRAY) return 0;
  return mrbc_array_size(&queue);
#endif
}

int
BLE_read_data(BLE_read_value_t *read_value)
{
  if (read_values.tt == MRBC_TT_NIL) {
    return -1;
  }
  mrbc_value value = mrbc_hash_get(&read_values, &mrbc_integer_value(read_value->att_handle));
  if (value.tt != MRBC_TT_STRING) return -1;
  if ((size_t)value.string->size > UINT16_MAX) return -1;
  read_value->data = value.string->data;
  read_value->size = value.string->size;
  return 0;
}


static void
c_pop_write_value(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (argc != 1) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  if (GET_TT_ARG(1) != MRBC_TT_INTEGER) {
    mrbc_raise(vm, MRBC_CLASS(TypeError), "BLE handle must be an Integer");
    return;
  }
  mrbc_int_t handle = GET_INT_ARG(1);
  if (handle <= 0 || handle > UINT16_MAX) {
    SET_NIL_RETURN();
    return;
  }
#ifdef PICORB_PLATFORM_ESP32
  uint8_t copy[PICORUBY_NIMBLE_WRITE_MAX];
  uint16_t len;
  picoruby_nimble_write_ticket_t ticket;
  if (!picoruby_nimble_peek_write((uint16_t)handle, copy, &len, &ticket)) {
    SET_NIL_RETURN();
    return;
  }
  /* pinned constructorは失敗時fatal。正常帰還より前にnative recordを消さない。 */
  mrbc_value value = mrbc_string_new(vm, copy, len);
  if (picoruby_nimble_commit_write(&ticket)) SET_RETURN(value);
  else {
    mrbc_decref(&value);
    SET_NIL_RETURN();
  }
#else
  mrbc_value key = GET_ARG(1);
  mrbc_value queue = mrbc_hash_get(&write_values, &key);
  if (queue.tt != MRBC_TT_ARRAY || mrbc_array_size(&queue) == 0) {
    SET_NIL_RETURN();
    return;
  }
  mrbc_value value = mrbc_array_shift(&queue);
  SET_RETURN(value);
#endif
}

static void
c_push_read_value(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (argc != 2) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  if (GET_TT_ARG(1) != MRBC_TT_INTEGER) {
    mrbc_raise(vm, MRBC_CLASS(TypeError), "BLE handle must be an Integer");
    return;
  }
  mrbc_value handle = GET_ARG(1);
  if (handle.i < 1 || handle.i > UINT16_MAX) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "invalid ATT handle");
    return;
  }
  mrbc_value read_value = GET_ARG(2);
  if (read_value.tt != MRBC_TT_STRING) {
    mrbc_raise(vm, MRBC_CLASS(TypeError), "BLE read value must be a String");
    return;
  }
  if ((size_t)read_value.string->size > UINT16_MAX) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "BLE read value exceeds 65535 bytes");
    return;
  }
#ifdef PICORB_PLATFORM_ESP32
  if (read_value.string->size > PICORUBY_BLE_READ_MIRROR_MAX) {
    mrbc_raise(vm, MRBC_CLASS(RangeError), "BLE read value exceeds mirror capacity");
    return;
  }
#endif
  mrbc_incref(&read_value);  // for hash ownership
  mrbc_hash_set(&read_values, &handle, &read_value);
#ifdef PICORB_PLATFORM_ESP32
  if (handle.tt == MRBC_TT_INTEGER && handle.i > 0 && handle.i <= UINT16_MAX) {
    picoruby_ble_publish_read_value((uint16_t)handle.i, read_value.string->data,
                                   (uint16_t)read_value.string->size);
  }
#endif
  mrbc_incref(&read_value);  // for return value
  SET_RETURN(read_value);
}

static void
c__init(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (argc != 1) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  if (GET_TT_ARG(1) != MRBC_TT_STRING && GET_TT_ARG(1) != MRBC_TT_NIL) {
    mrbc_raise(vm, MRBC_CLASS(TypeError), "BLE._init: wrong argument type");
    return;
  }
  int ble_role;
  mrbc_value role = mrbc_instance_getiv(&v[0], mrbc_str_to_symid("role"));
  if (role.tt != MRBC_TT_SYMBOL) {
    mrbc_decref(&role);
    mrbc_raise(vm, MRBC_CLASS(TypeError), "BLE._init: wrong role type");
    return;
  }
  int role_symid = role.sym_id;
  if (role_symid == mrbc_str_to_symid("central")) {
    ble_role = BLE_ROLE_CENTRAL;
  } else if (role_symid == mrbc_str_to_symid("peripheral")) {
    ble_role = BLE_ROLE_PERIPHERAL;
  } else if (role_symid == mrbc_str_to_symid("observer")) {
    ble_role = BLE_ROLE_OBSERVER;
  } else if (role_symid == mrbc_str_to_symid("broadcaster")) {
    ble_role = BLE_ROLE_BROADCASTER;
  } else {
    mrbc_raise(vm, MRBC_CLASS(TypeError), "BLE._init: wrong role type");
    return;
  }

  uint8_t *new_profile = NULL;
  size_t new_profile_len = 0;
  if (ble_role == BLE_ROLE_PERIPHERAL && GET_TT_ARG(1) == MRBC_TT_STRING) {
    /* Copy the profile to the C heap. BTstack keeps a raw pointer to
     * it, so a refcounted Ruby string is the wrong owner (the old
     * code leaked one copy per BLE.new). */
    new_profile_len = (size_t)v[1].string->size;
    if (new_profile_len == 0 || BLE_profile_extent((const uint8_t *)v[1].string->data, new_profile_len, NULL) < 0) {
      mrbc_raise(vm, MRBC_CLASS(ArgumentError), "BLE._init: invalid profile");
      return;
    }
    new_profile = mrbc_raw_alloc(new_profile_len);
    if (new_profile == NULL) {
      mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE._init: no memory");
      return;
    }
    memcpy(new_profile, v[1].string->data, new_profile_len);
  }
  /* Take references to the new instance's objects, then release the
   * previous instance's ones (mrbc_instance_getiv increfs). The
   * swap order keeps the statics valid at every point. */
  mrbc_value new_event_queue = mrbc_instance_getiv(&v[0], mrbc_str_to_symid("event_queue"));
  mrbc_value new_write_values = mrbc_instance_getiv(&v[0], mrbc_str_to_symid("write_values"));
  mrbc_value new_read_values = mrbc_instance_getiv(&v[0], mrbc_str_to_symid("read_values"));
  if (new_event_queue.tt != MRBC_TT_OBJECT || new_write_values.tt != MRBC_TT_HASH || new_read_values.tt != MRBC_TT_HASH) {
    if (new_profile) mrbc_raw_free(new_profile);
    /* The value hashes are created in ble.rb; a wrong type here
     * would crash the callbacks later. Release the getiv refs. */
    mrbc_decref(&new_event_queue);
    mrbc_decref(&new_write_values);
    mrbc_decref(&new_read_values);
    mrbc_raise(vm, MRBC_CLASS(TypeError), "BLE._init: value hashes not initialized");
    return;
  }
  mrbc_value new_ble = v[0];
  mrbc_incref(&new_ble);
  Machine_tud_task();
  if (BLE_init(new_profile, new_profile_len, ble_role) < 0) {
    if (new_profile) mrbc_raw_free(new_profile);
    mrbc_decref(&new_ble);
    mrbc_decref(&new_event_queue);
    mrbc_decref(&new_write_values);
    mrbc_decref(&new_read_values);
    mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE init failed");
    return;
  }
  /* BLE_init() replaces the ATT database for every role (possibly
   * with no database), so the previous profile is unreferenced now
   * and must be released even when the new role has none. */
  if (profile_buf) mrbc_raw_free(profile_buf);
  profile_buf = new_profile;

  /* Native success is the ownership commit point. Failed init keeps the old owner. */
  mrbc_value prev_ble = registered_ble;
  registered_ble = new_ble;
  mrbc_value prev_event_queue = event_queue;
  mrbc_value prev_write_values = write_values;
  mrbc_value prev_read_values = read_values;
  event_queue = new_event_queue;
  write_values = new_write_values;
  read_values = new_read_values;
  mrbc_decref(&prev_event_queue);
  mrbc_decref(&prev_write_values);
  mrbc_decref(&prev_read_values);
  mrbc_decref(&prev_ble);

  Machine_tud_task();
}

static void
c_control_fault(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (argc != 0) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
#ifdef PICORB_PLATFORM_ESP32
  SET_INT_RETURN(picoruby_nimble_control_fault());
#else
  SET_NIL_RETURN();
#endif
}

static void
c_event_rejection_snapshot(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (argc != 0) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
#ifdef PICORB_PLATFORM_ESP32
  uint8_t bytes[PICORUBY_NIMBLE_EVENT_REJECTION_SNAPSHOT_SIZE];
  picoruby_nimble_copy_event_rejections(bytes);
  mrbc_value snapshot = mrbc_string_new(vm, bytes, sizeof(bytes));
  SET_RETURN(snapshot);
#else
  SET_NIL_RETURN();
#endif
}

static void
c_host_reset_snapshot(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (argc != 0) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
#ifdef PICORB_PLATFORM_ESP32
  uint8_t bytes[PICORUBY_NIMBLE_HOST_RESET_SNAPSHOT_SIZE];
  picoruby_nimble_copy_host_reset_snapshot(bytes);
  mrbc_value snapshot = mrbc_string_new(vm, bytes, sizeof(bytes));
  SET_RETURN(snapshot);
#else
  SET_NIL_RETURN();
#endif
}

static void
c_advertising_failure_snapshot(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (argc != 0) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
#ifdef PICORB_PLATFORM_ESP32
  uint8_t bytes[PICORUBY_BLE_ADVERTISING_FAILURE_SNAPSHOT_SIZE];
  picoruby_ble_copy_advertising_failure_snapshot(bytes);
  mrbc_value snapshot = mrbc_string_new(vm, bytes, sizeof(bytes));
  SET_RETURN(snapshot);
#else
  SET_NIL_RETURN();
#endif
}

static void
c_native_ready(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (argc != 0) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  SET_BOOL_RETURN(BLE_ready());
}

static void
c_hci_power_control(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (argc != 1) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  if (GET_TT_ARG(1) != MRBC_TT_INTEGER) {
    mrbc_raise(vm, MRBC_CLASS(TypeError), "wrong type of arguments");
    return;
  }
  mrbc_int_t power_mode = GET_INT_ARG(1);
  if (power_mode < 0 || power_mode > 2) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "invalid BLE power mode");
    return;
  }
  if (power_mode != 0 && !BLE_ready()) {
    mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE init failed");
    return;
  }
  int result = BLE_hci_power_control((uint8_t)power_mode);
  if (result == BLE_POWER_CONTROL_UNSUPPORTED) {
    mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE power mode is unsupported");
    return;
  }
  if (result != 0) {
    mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE power control failed");
    return;
  }
  SET_INT_RETURN(0);
}

static void
c_gap_local_bd_addr(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (!BLE_ready()) {
    mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE init failed");
    return;
  }
  if (argc != 0) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  uint8_t addr[6];
  if (BLE_gap_local_bd_addr(addr) != 0) {
    mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE local address unavailable");
    return;
  }
  mrbc_value str = mrbc_string_new(vm, (const void *)addr, 6);
  SET_RETURN(str);
}

void
mrbc_ble_init(mrbc_vm *vm)
{
  mrbc_class *class_BLE = mrbc_define_class(vm, "BLE", mrbc_class_object);
  mrbc_define_method(vm, class_BLE, "_native_ready", c_native_ready);
  mrbc_define_method(vm, class_BLE, "_active_owner?", c_active_owner);
  mrbc_define_method(vm, class_BLE, "_check_active_owner", c_check_active_owner);
  mrbc_define_method(vm, class_BLE, "_init", c__init);
  mrbc_define_method(vm, class_BLE, "hci_power_control", c_hci_power_control);
  mrbc_define_method(vm, class_BLE, "gap_local_bd_addr", c_gap_local_bd_addr);
  mrbc_define_method(vm, class_BLE, "pop_write_value", c_pop_write_value);
  mrbc_define_method(vm, class_BLE, "push_read_value", c_push_read_value);
  mrbc_define_method(vm, class_BLE, "control_fault", c_control_fault);
  mrbc_define_method(vm, class_BLE, "_event_rejection_snapshot", c_event_rejection_snapshot);
  mrbc_define_method(vm, class_BLE, "host_reset_snapshot", c_host_reset_snapshot);
  mrbc_define_method(vm, class_BLE, "advertising_failure_snapshot", c_advertising_failure_snapshot);
  mrbc_define_method(vm, class_BLE, "_check_registration", c_check_registration);
  mrbc_define_method(vm, class_BLE, "_power_control_registration", c_power_control_registration);
  mrbc_define_method(vm, class_BLE, "_event_popped", c_event_popped);
  mrbc_define_method(vm, class_BLE, "_event_queue_cleared", c_event_queue_cleared);
  mrbc_init_class_BLE_Peripheral(vm, class_BLE);
  mrbc_init_class_BLE_Broadcaster(vm, class_BLE);
  mrbc_init_class_BLE_Central(vm, class_BLE);
}
