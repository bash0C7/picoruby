#include <stdint.h>
#include <stdbool.h>
#include "mruby.h"
#include "mruby/error.h"
#include "../../include/ble.h"
#include "ble_common.h"
#include "PicoBLEDarwin-Swift.h"
#include "../../../picoruby-machine/include/hal.h"

#if !defined(MRB_USE_TASK_SCHEDULER)
#error "picoruby-ble on Darwin needs MRB_USE_TASK_SCHEDULER: the CoreBluetooth event pump rides the scheduler-service layer"
#endif

#define PUMP_EVT_MAX 512

static uint8_t held[PUMP_EVT_MAX];
static uint16_t held_len;
static bool heartbeat_owed;

static mrb_value
pump_body(mrb_state *mrb, void *ud)
{
  (void)mrb;
  (void)ud;
  for (;;) {
    if (held_len == 0) {
      int32_t n = pble_drain_one(held, (int32_t)sizeof(held));
      if (n <= 0) break;
      held_len = (uint16_t)n;
    }
    if (BLE_push_event(held, held_len) != 0) break;
    held_len = 0;
  }
  if (pble_take_heartbeat()) heartbeat_owed = true;
  if (heartbeat_owed && BLE_heartbeat() == 0) heartbeat_owed = false;
  return mrb_nil_value();
}

static void
pump_service(mrb_state *mrb, void *ud)
{
  (void)ud;
  mrb_bool error = FALSE;
  int ai = mrb_gc_arena_save(mrb);
  mrb_protect_error(mrb, pump_body, NULL, &error);
  mrb_gc_arena_restore(mrb, ai);
}

void
pble_attach_vm(void *vm)
{
  held_len = 0;
  heartbeat_owed = false;
  picorb_scheduler_service_add((mrb_state *)vm, pump_service, NULL);
}
