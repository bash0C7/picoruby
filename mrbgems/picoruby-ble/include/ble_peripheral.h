#ifndef BLE_PERIPHERAL_DEFINED_H_
#define BLE_PERIPHERAL_DEFINED_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_ADVERTISING_DATA_MAX_LEN 31
// Returns 0 after command submission; nonzero means the request was rejected.
int BLE_peripheral_advertise(uint8_t *adv_data, uint8_t adv_data_len, bool connectable);
void BLE_peripheral_stop_advertise(void);
int BLE_peripheral_notify(uint16_t att_handle);
void BLE_peripheral_request_can_send_now_event(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_PERIPHERAL_DEFINED_H_ */

