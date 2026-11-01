#include "mruby.h"
#include "mruby/presym.h"
#include "mruby/string.h"

static mrb_value
mrb_discover_primary_services(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  if (!BLE_ready()) mrb_raise(mrb, E_RUNTIME_ERROR, "BLE init failed");
  mrb_int conn_handle;
  mrb_get_args(mrb, "i", &conn_handle);
  if (conn_handle < 0 || conn_handle > 0xfffe) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "BLE argument out of range");
  }
  uint8_t res = BLE_discover_primary_services((uint16_t)conn_handle);
  return mrb_int_value(mrb, res);
}

static mrb_value
mrb_discover_characteristics_for_service(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  if (!BLE_ready()) mrb_raise(mrb, E_RUNTIME_ERROR, "BLE init failed");
  mrb_int conn_handle, start_handle, end_handle;
  mrb_get_args(mrb, "iii", &conn_handle, &start_handle, &end_handle);
  if (conn_handle < 0 || conn_handle > 0xfffe || start_handle < 1 || start_handle > 0xffff || end_handle < 1 || end_handle > 0xffff) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "BLE argument out of range");
  }

  uint8_t res = BLE_discover_characteristics_for_service(
    (uint16_t)conn_handle,
    (uint16_t)start_handle,
    (uint16_t)end_handle
  );
  return mrb_int_value(mrb, res);
}

static mrb_value
mrb_read_value_of_characteristic_using_value_handle(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  if (!BLE_ready()) mrb_raise(mrb, E_RUNTIME_ERROR, "BLE init failed");
  mrb_int conn_handle, value_handle;
  mrb_get_args(mrb, "ii", &conn_handle, &value_handle);
  if (conn_handle < 0 || conn_handle > 0xfffe || value_handle < 1 || value_handle > 0xffff) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "BLE argument out of range");
  }

  uint8_t res = BLE_read_value_of_characteristic_using_value_handle(
    (uint16_t)conn_handle,
    (uint16_t)value_handle
  );
  return mrb_int_value(mrb, res);
}

static mrb_value
mrb_discover_characteristic_descriptors(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  if (!BLE_ready()) mrb_raise(mrb, E_RUNTIME_ERROR, "BLE init failed");
  mrb_int conn_handle, value_handle, end_handle;
  mrb_get_args(mrb, "iii", &conn_handle, &value_handle, &end_handle);
  if (conn_handle < 0 || conn_handle > 0xfffe || value_handle < 1 || value_handle > 0xffff || end_handle < 1 || end_handle > 0xffff) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "BLE argument out of range");
  }

  uint8_t res = BLE_discover_characteristic_descriptors(
    (uint16_t)conn_handle,
    (uint16_t)value_handle,
    (uint16_t)end_handle
  );
  return mrb_int_value(mrb, res);
}

static mrb_value
mrb_write_value_of_characteristic_without_response(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  if (!BLE_ready()) mrb_raise(mrb, E_RUNTIME_ERROR, "BLE init failed");
  mrb_int conn_handle, value_handle;
  mrb_value data;
  mrb_get_args(mrb, "iiS", &conn_handle, &value_handle, &data);
  if (conn_handle < 0 || conn_handle > 0xfffe || value_handle < 1 || value_handle > 0xffff) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "BLE argument out of range");
  }
  if (RSTRING_LEN(data) > UINT16_MAX) mrb_raise(mrb, E_ARGUMENT_ERROR, "BLE write value exceeds 65535 bytes");
  uint8_t res = BLE_write_value_of_characteristic_without_response(
    (uint16_t)conn_handle,
    (uint16_t)value_handle,
    (const uint8_t *)RSTRING_PTR(data),
    (uint16_t)RSTRING_LEN(data)
  );
  return mrb_int_value(mrb, res);
}

static mrb_value
mrb_write_characteristic_descriptor_using_descriptor_handle(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  if (!BLE_ready()) mrb_raise(mrb, E_RUNTIME_ERROR, "BLE init failed");
  mrb_int conn_handle, descriptor_handle;
  mrb_value data;
  mrb_get_args(mrb, "iiS", &conn_handle, &descriptor_handle, &data);
  if (conn_handle < 0 || conn_handle > 0xfffe || descriptor_handle < 1 || descriptor_handle > 0xffff) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "BLE argument out of range");
  }
  if (RSTRING_LEN(data) > UINT16_MAX) mrb_raise(mrb, E_ARGUMENT_ERROR, "BLE write value exceeds 65535 bytes");
  uint8_t res = BLE_write_characteristic_descriptor_using_descriptor_handle(
    (uint16_t)conn_handle,
    (uint16_t)descriptor_handle,
    (const uint8_t *)RSTRING_PTR(data),
    (uint16_t)RSTRING_LEN(data)
  );
  return mrb_int_value(mrb, res);
}

/*
 * @param [Symbol] scan_type. :passive or :active
 * @param [Integer] scan_interval. 4..4000 (unit: 0.625ms)
 * @param [Integer] scan_window. 4..4000 (unit: 0.625ms)
 * @param [Symbol] scan_filter_policy (not implemented)
 * @return void
 */
static mrb_value
mrb_set_scan_params(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  mrb_value scan_type;
  mrb_int scan_interval, scan_window, scan_type_num;
  mrb_get_args(mrb, "oii", &scan_type, &scan_interval, &scan_window);
  if (scan_interval < 0 || scan_interval > UINT16_MAX || scan_window < 0 || scan_window > UINT16_MAX) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "BLE argument out of range");
  }

  if (!mrb_symbol_p(scan_type)) {
    mrb_raise(mrb, E_TYPE_ERROR, "wrong type of scan_type");
  } else if (mrb_symbol(scan_type) == MRB_SYM(active)) {
    scan_type_num = 1;
  } else if (mrb_symbol(scan_type) == MRB_SYM(passive)) {
    scan_type_num = 0;
  } else {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "wrong type of scan_type");
  }
  uint8_t scanning_filter_policy = 0; // TODO 1: all from whitelist
  BLE_central_set_scan_params(scan_type_num, (uint16_t)scan_interval, (uint16_t)scan_window, scanning_filter_policy);
  return mrb_int_value(mrb, 0);
}

static mrb_value
mrb_start_scan(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  if (!BLE_ready()) mrb_raise(mrb, E_RUNTIME_ERROR, "BLE init failed");
  return mrb_int_value(mrb, BLE_central_start_scan());
}

static mrb_value
mrb_stop_scan(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  if (!BLE_ready()) mrb_raise(mrb, E_RUNTIME_ERROR, "BLE init failed");
  return mrb_int_value(mrb, BLE_central_stop_scan());
}

static mrb_value
mrb_gap_connect(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  if (!BLE_ready()) mrb_raise(mrb, E_RUNTIME_ERROR, "BLE init failed");
  mrb_value addr;
  mrb_int addr_type;
  mrb_get_args(mrb, "Si", &addr, &addr_type);
  if (addr_type < 0 || addr_type > 0x3) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "BLE argument out of range");
  }
  if (RSTRING_LEN(addr) != 6) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "address must be 6 bytes");
  }
  uint8_t res = BLE_central_gap_connect((const uint8_t *)RSTRING_PTR(addr), (uint8_t)addr_type);
  return mrb_int_value(mrb, res);
}

void
mrb_init_class_BLE_Central(mrb_state *mrb, struct RClass *class_BLE)
{
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(set_scan_params), mrb_set_scan_params, MRB_ARGS_REQ(3));
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(start_scan), mrb_start_scan, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(stop_scan), mrb_stop_scan, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(gap_connect), mrb_gap_connect, MRB_ARGS_REQ(2));
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(discover_primary_services), mrb_discover_primary_services, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(discover_characteristics_for_service), mrb_discover_characteristics_for_service, MRB_ARGS_REQ(3));
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(read_value_of_characteristic_using_value_handle), mrb_read_value_of_characteristic_using_value_handle, MRB_ARGS_REQ(2));
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(discover_characteristic_descriptors), mrb_discover_characteristic_descriptors, MRB_ARGS_REQ(3));
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(write_value_of_characteristic_without_response), mrb_write_value_of_characteristic_without_response, MRB_ARGS_REQ(3));
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(write_characteristic_descriptor_using_descriptor_handle), mrb_write_characteristic_descriptor_using_descriptor_handle, MRB_ARGS_REQ(3));
}

