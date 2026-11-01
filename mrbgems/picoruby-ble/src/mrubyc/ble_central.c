#include "mrubyc.h"

static void
c_discover_primary_services(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (!BLE_ready()) {
    mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE init failed");
    return;
  }
  if (argc != 1) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  if (GET_TT_ARG(1) != MRBC_TT_INTEGER) {
    mrbc_raise(vm, MRBC_CLASS(TypeError), "wrong type of arguments");
    return;
  }
  if (GET_INT_ARG(1) < 0 || GET_INT_ARG(1) > 0xfffe) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "BLE argument out of range");
    return;
  }
  uint8_t res = BLE_discover_primary_services((uint16_t)GET_INT_ARG(1));
  SET_INT_RETURN(res);
}

static void
c_discover_characteristics_for_service(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (!BLE_ready()) {
    mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE init failed");
    return;
  }
  if (argc != 3) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  if (GET_TT_ARG(1) != MRBC_TT_INTEGER || GET_TT_ARG(2) != MRBC_TT_INTEGER || GET_TT_ARG(3) != MRBC_TT_INTEGER) {
    mrbc_raise(vm, MRBC_CLASS(TypeError), "wrong type of arguments");
    return;
  }
  if (GET_INT_ARG(1) < 0 || GET_INT_ARG(1) > 0xfffe || GET_INT_ARG(2) < 1 || GET_INT_ARG(2) > 0xffff || GET_INT_ARG(3) < 1 || GET_INT_ARG(3) > 0xffff) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "BLE argument out of range");
    return;
  }
  uint8_t res = BLE_discover_characteristics_for_service(
    (uint16_t)GET_INT_ARG(1),
    (uint16_t)GET_INT_ARG(2),
    (uint16_t)GET_INT_ARG(3)
  );
  SET_INT_RETURN(res);
}

static void
c_read_value_of_characteristic_using_value_handle(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (!BLE_ready()) {
    mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE init failed");
    return;
  }
  if (argc != 2) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  if (GET_TT_ARG(1) != MRBC_TT_INTEGER || GET_TT_ARG(2) != MRBC_TT_INTEGER) {
    mrbc_raise(vm, MRBC_CLASS(TypeError), "wrong type of arguments");
    return;
  }
  if (GET_INT_ARG(1) < 0 || GET_INT_ARG(1) > 0xfffe || GET_INT_ARG(2) < 1 || GET_INT_ARG(2) > 0xffff) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "BLE argument out of range");
    return;
  }
  uint8_t res = BLE_read_value_of_characteristic_using_value_handle(
    (uint16_t)GET_INT_ARG(1),
    (uint16_t)GET_INT_ARG(2)
  );
  SET_INT_RETURN(res);
}

static void
c_discover_characteristic_descriptors(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (!BLE_ready()) {
    mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE init failed");
    return;
  }
  if (argc != 3) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  if (GET_TT_ARG(1) != MRBC_TT_INTEGER || GET_TT_ARG(2) != MRBC_TT_INTEGER || GET_TT_ARG(3) != MRBC_TT_INTEGER) {
    mrbc_raise(vm, MRBC_CLASS(TypeError), "wrong type of arguments");
    return;
  }
  if (GET_INT_ARG(1) < 0 || GET_INT_ARG(1) > 0xfffe || GET_INT_ARG(2) < 1 || GET_INT_ARG(2) > 0xffff || GET_INT_ARG(3) < 1 || GET_INT_ARG(3) > 0xffff) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "BLE argument out of range");
    return;
  }
  uint8_t res = BLE_discover_characteristic_descriptors(
    (uint16_t)GET_INT_ARG(1),
    (uint16_t)GET_INT_ARG(2),
    (uint16_t)GET_INT_ARG(3)
  );
  SET_INT_RETURN(res);
}

static void
c_write_value_of_characteristic_without_response(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (!BLE_ready()) {
    mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE init failed");
    return;
  }
  if (argc != 3) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  if (GET_TT_ARG(1) != MRBC_TT_INTEGER || GET_TT_ARG(2) != MRBC_TT_INTEGER || GET_TT_ARG(3) != MRBC_TT_STRING) {
    mrbc_raise(vm, MRBC_CLASS(TypeError), "wrong type of arguments");
    return;
  }
  if (GET_INT_ARG(1) < 0 || GET_INT_ARG(1) > 0xfffe || GET_INT_ARG(2) < 1 || GET_INT_ARG(2) > 0xffff) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "BLE argument out of range");
    return;
  }
  if ((size_t)v[3].string->size > UINT16_MAX) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "BLE write value exceeds 65535 bytes");
    return;
  }
  uint8_t res = BLE_write_value_of_characteristic_without_response(
    (uint16_t)GET_INT_ARG(1),
    (uint16_t)GET_INT_ARG(2),
    (const uint8_t *)v[3].string->data,
    (uint16_t)v[3].string->size
  );
  SET_INT_RETURN(res);
}

static void
c_write_characteristic_descriptor_using_descriptor_handle(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (!BLE_ready()) {
    mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE init failed");
    return;
  }
  if (argc != 3) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  if (GET_TT_ARG(1) != MRBC_TT_INTEGER || GET_TT_ARG(2) != MRBC_TT_INTEGER || GET_TT_ARG(3) != MRBC_TT_STRING) {
    mrbc_raise(vm, MRBC_CLASS(TypeError), "wrong type of arguments");
    return;
  }
  if (GET_INT_ARG(1) < 0 || GET_INT_ARG(1) > 0xfffe || GET_INT_ARG(2) < 1 || GET_INT_ARG(2) > 0xffff) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "BLE argument out of range");
    return;
  }
  if ((size_t)v[3].string->size > UINT16_MAX) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "BLE write value exceeds 65535 bytes");
    return;
  }
  uint8_t res = BLE_write_characteristic_descriptor_using_descriptor_handle(
    (uint16_t)GET_INT_ARG(1),
    (uint16_t)GET_INT_ARG(2),
    (const uint8_t *)v[3].string->data,
    (uint16_t)v[3].string->size
  );
  SET_INT_RETURN(res);
}

/*
 * @param [Symbol] scan_type. :passive or :active
 * @param [Integer] scan_interval. 4..4000 (unit: 0.625ms)
 * @param [Integer] scan_window. 4..4000 (unit: 0.625ms)
 * @param [Symbol] scan_filter_policy (not implemented)
 * @return void
 */
static void
c_set_scan_params(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (argc != 3) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  if (GET_TT_ARG(1) != MRBC_TT_SYMBOL ||
      GET_TT_ARG(2) != MRBC_TT_INTEGER ||
      GET_TT_ARG(3) != MRBC_TT_INTEGER) {
    mrbc_raise(vm, MRBC_CLASS(TypeError), "wrong type of arguments");
    return;
  }
  if (GET_INT_ARG(2) < 0 || GET_INT_ARG(2) > UINT16_MAX || GET_INT_ARG(3) < 0 || GET_INT_ARG(3) > UINT16_MAX) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "BLE argument out of range");
    return;
  }
  uint8_t scan_type;
  if (mrbc_str_to_symid("passive") == v[1].i) {
    scan_type = 0;
  } else if (mrbc_str_to_symid("active") == v[1].i) {
    scan_type = 1;
  } else {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong parameter of scan_type");
    return;
  }
  uint8_t scanning_filter_policy = 0; // TODO 1: all from whitelist
  BLE_central_set_scan_params(scan_type, (uint16_t)GET_INT_ARG(2), (uint16_t)GET_INT_ARG(3), scanning_filter_policy);
  SET_INT_RETURN(0);
}

static void
c_start_scan(mrbc_vm *vm, mrbc_value *v, int argc)
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
  SET_INT_RETURN(BLE_central_start_scan());
}

static void
c_stop_scan(mrbc_vm *vm, mrbc_value *v, int argc)
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
  SET_INT_RETURN(BLE_central_stop_scan());
}

static void
c_gap_connect(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (!BLE_ready()) {
    mrbc_raise(vm, MRBC_CLASS(RuntimeError), "BLE init failed");
    return;
  }
  if (argc != 2) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  if (GET_TT_ARG(1) != MRBC_TT_STRING || GET_TT_ARG(2) != MRBC_TT_INTEGER) {
    mrbc_raise(vm, MRBC_CLASS(TypeError), "wrong type of arguments");
    return;
  }
  if (GET_INT_ARG(2) < 0 || GET_INT_ARG(2) > 0x3) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "BLE argument out of range");
    return;
  }
  if (GET_ARG(1).string->size != 6) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "address must be 6 bytes");
    return;
  }
  uint8_t res = BLE_central_gap_connect(GET_STRING_ARG(1), GET_INT_ARG(2));
  SET_INT_RETURN(res);
}

void
mrbc_init_class_BLE_Central(mrbc_vm *vm, mrbc_class *class_BLE)
{
  mrbc_define_method(vm, class_BLE, "set_scan_params", c_set_scan_params);
  mrbc_define_method(vm, class_BLE, "start_scan", c_start_scan);
  mrbc_define_method(vm, class_BLE, "stop_scan", c_stop_scan);
  mrbc_define_method(vm, class_BLE, "gap_connect", c_gap_connect);
  mrbc_define_method(vm, class_BLE, "discover_primary_services", c_discover_primary_services);
  mrbc_define_method(vm, class_BLE, "discover_characteristics_for_service", c_discover_characteristics_for_service);
  mrbc_define_method(vm, class_BLE, "read_value_of_characteristic_using_value_handle", c_read_value_of_characteristic_using_value_handle);
  mrbc_define_method(vm, class_BLE, "discover_characteristic_descriptors", c_discover_characteristic_descriptors);
  mrbc_define_method(vm, class_BLE, "write_value_of_characteristic_without_response", c_write_value_of_characteristic_without_response);
  mrbc_define_method(vm, class_BLE, "write_characteristic_descriptor_using_descriptor_handle", c_write_characteristic_descriptor_using_descriptor_handle);
}
