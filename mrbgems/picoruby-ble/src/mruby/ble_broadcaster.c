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
    rc = BLE_peripheral_advertise((uint8_t *)RSTRING_PTR(adv_data), RSTRING_LEN(adv_data), false);
  } else {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "advertise data must be a string");
  }
  return mrb_int_value(mrb, rc);
}

void
mrb_init_class_BLE_Broadcaster(mrb_state *mrb, struct RClass *class_BLE)
{
  mrb_define_method_id(mrb, class_BLE, MRB_SYM(broadcaster_advertise), mrb_advertise, MRB_ARGS_REQ(1));
}
