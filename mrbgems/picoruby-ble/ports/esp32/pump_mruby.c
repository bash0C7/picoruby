#include "nimble_owner.h"
#include "../../../picoruby-machine/include/hal.h"
#include "mruby/error.h"
#include "esp_log.h"

static const char *TAG = "prb_ble_pump";
static bool pump_exception_logged = false;

static mrb_value
pump_body(mrb_state *mrb, void *ud)
{
  (void)mrb;
  (void)ud;
  picoruby_nimble_pump();
  return mrb_nil_value();
}

static void
pump_service(mrb_state *mrb, void *ud)
{
  (void)ud;
  mrb_bool error = FALSE;
  int ai = mrb_gc_arena_save(mrb);
  mrb_value result = mrb_protect_error(mrb, pump_body, NULL, &error);
  if (!error) {
    pump_exception_logged = false;
  } else if (!pump_exception_logged) {
    pump_exception_logged = true;
    ESP_LOGW(TAG, "pump raised %s", mrb_obj_classname(mrb, result));
  }
  mrb_gc_arena_restore(mrb, ai);
}

void
picoruby_nimble_attach_vm(void *vm)
{
  picorb_scheduler_service_add((mrb_state *)vm, pump_service, NULL);
}
