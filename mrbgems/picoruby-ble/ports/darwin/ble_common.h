#ifndef BLE_COMMON_DEFINED_H_
#define BLE_COMMON_DEFINED_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool pble_take_heartbeat(void);
void pble_attach_vm(void *vm);

#ifdef __cplusplus
}
#endif

#endif /* BLE_COMMON_DEFINED_H_ */
