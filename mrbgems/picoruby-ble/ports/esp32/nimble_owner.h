#ifndef PICORUBY_NIMBLE_OWNER_H_
#define PICORUBY_NIMBLE_OWNER_H_

#include <stdint.h>
#include <stdbool.h>

#include "../../include/ble.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*picoruby_nimble_setup_fn)(void);

#define PICORUBY_NIMBLE_EVT_MAX 539

int picoruby_nimble_start(picoruby_nimble_setup_fn setup);
int picoruby_nimble_stop(void);
bool picoruby_nimble_started(void);
bool picoruby_nimble_ready(void);
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

typedef enum {
  PICORUBY_LOG_ATT_GATE,
  PICORUBY_LOG_ATT_NO_LINK,
  PICORUBY_LOG_ATT_NO_MAP,
  PICORUBY_LOG_ATT_SECURITY_LINK,
  PICORUBY_LOG_READ_APPEND,
  PICORUBY_LOG_WRITE_SIZE,
  PICORUBY_LOG_WRITE_COPY,
  PICORUBY_LOG_LATE_TERMINATE,
  PICORUBY_LOG_ADV_REPORT,
  PICORUBY_LOG_NOTIFY_RX,
  PICORUBY_LOG_ENC_CHANGE,
  PICORUBY_LOG_BOND_READ,
  PICORUBY_LOG_REPEAT_PAIRING,
  PICORUBY_LOG_ADV_START,
  PICORUBY_LOG_NOTIFY,
  PICORUBY_LOG_EVENT_QUEUE,
  PICORUBY_LOG_EVENT_NOT_READY,
  PICORUBY_LOG_WRITE_QUEUE,
  PICORUBY_LOG_CCCD_REPLACED,
  PICORUBY_LOG_WRITE_DELIVERY,
  PICORUBY_LOG_SITE_COUNT,
} picoruby_log_site_t;

bool picoruby_log_due(picoruby_log_site_t site, uint32_t *suppressed);
void picoruby_log_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* PICORUBY_NIMBLE_OWNER_H_ */
