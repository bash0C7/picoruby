#ifndef BLE_COMMON_DEFINED_H_
#define BLE_COMMON_DEFINED_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GCD heartbeat timer -> scheduler pump: true at most once per armed
 * period, clearing the flag (defined in ports/darwin/ble.c). Called from
 * the VM thread by src/mruby/ble.c's ble_scheduler_pump. */
bool pble_take_heartbeat(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_COMMON_DEFINED_H_ */
