#ifndef PICORUBY_NIMBLE_OWNER_H_
#define PICORUBY_NIMBLE_OWNER_H_

#include <stdint.h>
#include <stdbool.h>

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

typedef struct {
  uint32_t full;
  uint32_t too_large;
  uint32_t not_ready;
  bool saturated;
  bool copy_failed;
  bool write_rejected;
} picoruby_nimble_event_rejections_t;

typedef enum {
  PICORUBY_NIMBLE_EVENT_DATA,
  PICORUBY_NIMBLE_EVENT_CONTROL,
} picoruby_nimble_event_class_t;

picoruby_nimble_event_result_t picoruby_nimble_enqueue_event(const uint8_t *pkt, uint16_t len, picoruby_nimble_event_class_t event_class);
// Process-wide non-destructive snapshot. out is nonnull. Not cleared by reset/start/stop.
void picoruby_nimble_get_event_rejections(picoruby_nimble_event_rejections_t *out);
#define PICORUBY_NIMBLE_EVENT_REJECTION_SNAPSHOT_SIZE 13
#define PICORUBY_NIMBLE_REJECTION_SATURATED 0x01
#define PICORUBY_NIMBLE_REJECTION_COPY_FAILED 0x02
#define PICORUBY_NIMBLE_REJECTION_WRITE_REJECTED 0x04
// Copies the three cumulative LE values and the flags. Bit 0 of the last byte is saturation, bit 1 is copy failure.
// Bit 2 is a rejection in native write input validation. The flags do not represent counts or session attribution.
// out must be at least 13 bytes. Not cleared by reset either.
void picoruby_nimble_copy_event_rejections(uint8_t *out);
void picoruby_nimble_record_oversized_event(void);
void picoruby_nimble_record_copy_failure(void);
#define PICORUBY_NIMBLE_HOST_RESET_SNAPSHOT_SIZE 5
// Process-cumulative LE count and saturation flag. out must be at least 5 bytes. Not cleared by new/start/successful sync.
void picoruby_nimble_copy_host_reset_snapshot(uint8_t *out);
void picoruby_nimble_reset_events(void);
#define PICORUBY_NIMBLE_WRITE_MAX 512
#define PICORUBY_NIMBLE_WRITE_DEPTH 16
/* VM only, non-reentrant, no concurrent consumer from peek to commit. The copy is the caller's native area. */
typedef struct {
  uint32_t queue_generation;
  uint32_t slot_generation;
  uint16_t ruby_handle;
  int16_t slot;
} picoruby_nimble_write_ticket_t;
int picoruby_nimble_enqueue_write(uint16_t ruby_handle, const uint8_t *data, uint16_t len);
int picoruby_nimble_enqueue_cccd(uint16_t ruby_handle, const uint8_t *data, uint16_t len);
void picoruby_nimble_reserve_cccd_writes(int count);
int picoruby_nimble_write_pending(uint16_t ruby_handle);
bool picoruby_nimble_peek_write(uint16_t ruby_handle, uint8_t *copy, uint16_t *len,
                               picoruby_nimble_write_ticket_t *ticket);
bool picoruby_nimble_commit_write(const picoruby_nimble_write_ticket_t *ticket);
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
