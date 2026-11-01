#include "mruby.h"
#include "mruby/presym.h"
#include "mruby/string.h"

static mrb_value
mrb_advertise(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  if (!BLE_ready()) mrb_raise(mrb, E_RUNTIME_ERROR, "BLE init failed");
  mrb_value adv_data;
  int rc = 0;
  mrb_get_args(mrb, "o", &adv_data);
  if (mrb_nil_p(adv_data)) {
    BLE_peripheral_stop_advertise();
  } else if (mrb_string_p(adv_data)) {
    if (RSTRING_LEN(adv_data) > BLE_ADVERTISING_DATA_MAX_LEN) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "advertising data exceeds 31 bytes");
    }
    rc = BLE_peripheral_advertise((uint8_t *)RSTRING_PTR(adv_data), RSTRING_LEN(adv_data), true);
  } else {
    mrb_raise(mrb, E_TYPE_ERROR, "wrong type of adv_data");
  }
  return mrb_int_value(mrb, rc);
}

static mrb_value
mrb_notify(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  if (!BLE_ready()) mrb_raise(mrb, E_RUNTIME_ERROR, "BLE init failed");
  mrb_int att_handle;
  mrb_get_args(mrb, "i", &att_handle);
  if (att_handle < 1 || att_handle > UINT16_MAX) mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid ATT handle");
  return mrb_int_value(mrb, BLE_peripheral_notify((uint16_t)att_handle));
}

static mrb_value
mrb_request_can_send_now_event(mrb_state *mrb, mrb_value self)
{
  mrb_ble_check_active_owner(mrb, self);
  BLE_peripheral_request_can_send_now_event();
  return mrb_int_value(mrb, 0);
}

void
mrb_init_class_BLE_Peripheral(mrb_state *mrb, struct RClass *class_BLE)
{
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(peripheral_advertise), mrb_advertise, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(notify), mrb_notify, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(request_can_send_now_event), mrb_request_can_send_now_event, MRB_ARGS_NONE());
}


