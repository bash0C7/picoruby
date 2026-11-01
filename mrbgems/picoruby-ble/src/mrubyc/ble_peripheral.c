#include "mrubyc.h"

static void
c_advertise(mrbc_vm *vm, mrbc_value *v, int argc)
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
  if (GET_TT_ARG(1) != MRBC_TT_STRING && GET_TT_ARG(1) != MRBC_TT_NIL) {
    mrbc_raise(vm, MRBC_CLASS(TypeError), "advertising data must be a String or nil");
    return;
  }
  mrbc_value adv_data = GET_ARG(1);
  int rc = 0;
  if (adv_data.tt == MRBC_TT_NIL) {
    BLE_peripheral_stop_advertise();
  } else {
    if (adv_data.string->size > BLE_ADVERTISING_DATA_MAX_LEN) {
      mrbc_raise(vm, MRBC_CLASS(ArgumentError), "advertising data exceeds 31 bytes");
      return;
    }
    rc = BLE_peripheral_advertise(adv_data.string->data, adv_data.string->size, true);
  }
  SET_INT_RETURN(rc);
}

static void
c_notify(mrbc_vm *vm, mrbc_value *v, int argc)
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
  if (GET_INT_ARG(1) < 1 || GET_INT_ARG(1) > UINT16_MAX) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "invalid ATT handle");
    return;
  }
  int status = BLE_peripheral_notify((uint16_t)GET_INT_ARG(1));
  SET_INT_RETURN(status);
}

static void
c_request_can_send_now_event(mrbc_vm *vm, mrbc_value *v, int argc)
{
  if (!mrbc_ble_check_active_owner(vm, &v[0])) return;
  if (argc != 0) {
    mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
    return;
  }
  BLE_peripheral_request_can_send_now_event();
  SET_INT_RETURN(0);
}

void
mrbc_init_class_BLE_Peripheral(mrbc_vm *vm, mrbc_class *class_BLE)
{
  mrbc_define_method(vm, class_BLE, "peripheral_advertise", c_advertise);
  mrbc_define_method(vm, class_BLE, "notify", c_notify);
  mrbc_define_method(vm, class_BLE, "request_can_send_now_event", c_request_can_send_now_event);
}

