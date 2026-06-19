/* Apple/Darwin port — peripheral/broadcaster ABI-completeness stubs
 * (include/ble_peripheral.h). CoreBluetooth is
 * central/GATT-client only; there is no CBPeripheralManager backend, so these
 * are no-ops. Present so the shared src/*ble_peripheral*.c / *broadcaster*.c
 * link in a central/observer build. */
#include <stdint.h>
#include <stdbool.h>

#include "../../include/ble_peripheral.h"
#include "ble_common.h"

void
BLE_peripheral_advertise(uint8_t *adv_data, uint8_t adv_data_len, bool connectable)
{
  (void)adv_data; (void)adv_data_len; (void)connectable;
}

void
BLE_peripheral_stop_advertise(void)
{
}

void
BLE_peripheral_notify(uint16_t att_handle)
{
  (void)att_handle;
}

void
BLE_peripheral_request_can_send_now_event(void)
{
}
