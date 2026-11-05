#ifndef PICORUBY_NIMBLE_OWNER_H_
#define PICORUBY_NIMBLE_OWNER_H_

#include <stdint.h>
#include <stdbool.h>

#include "../../include/ble.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*picoruby_nimble_setup_fn)(void);

// Fixed SDK maximum MTU 527 + existing header 12. Separate from the legal length of an attribute value.
#define PICORUBY_NIMBLE_EVT_MAX 539

int picoruby_nimble_start(picoruby_nimble_setup_fn setup);
int picoruby_nimble_stop(void);
bool picoruby_nimble_started(void);
bool picoruby_nimble_ready(void);
/* Fixed native latch; cleared only immediately before a new host starts. */
#define PICORUBY_NIMBLE_CONTROL_WORK_LOST 0x01
#define PICORUBY_NIMBLE_CONTROL_CONNECTION_LOST 0x08
void picoruby_nimble_cancel_control(uint8_t reason);
uint8_t picoruby_nimble_control_fault(void);
bool picoruby_nimble_cleanup_blocked(void);
uint8_t picoruby_nimble_own_addr_type(void);
typedef enum {
  PICORUBY_NIMBLE_EVENT_ACCEPTED = 0,
  PICORUBY_NIMBLE_EVENT_FULL = -1,
  PICORUBY_NIMBLE_EVENT_TOO_LARGE = -2,
  PICORUBY_NIMBLE_EVENT_NOT_READY = -3,
} picoruby_nimble_event_result_t;

typedef enum {
  PICORUBY_NIMBLE_EVENT_DATA,
  PICORUBY_NIMBLE_EVENT_CONTROL,
} picoruby_nimble_event_class_t;

picoruby_nimble_event_result_t picoruby_nimble_enqueue_event(const uint8_t *pkt, uint16_t len, picoruby_nimble_event_class_t event_class);
void picoruby_nimble_reset_events(void);
#define PICORUBY_NIMBLE_WRITE_MAX 512
#define PICORUBY_NIMBLE_WRITE_DEPTH 16
int picoruby_nimble_enqueue_write(uint16_t ruby_handle, const uint8_t *data, uint16_t len);
int picoruby_nimble_enqueue_cccd(uint16_t ruby_handle, const uint8_t *data, uint16_t len);
void picoruby_nimble_reserve_cccd_writes(int count);
void picoruby_nimble_reset_writes(void);
void picoruby_nimble_heartbeat_enable(bool enable);
void picoruby_nimble_halting_watchdog_start(void);
void picoruby_nimble_halting_watchdog_stop(void);
void picoruby_nimble_pump(void);
void picoruby_nimble_attach_vm(void *vm);

#ifdef __cplusplus
}
#endif

#endif /* PICORUBY_NIMBLE_OWNER_H_ */
