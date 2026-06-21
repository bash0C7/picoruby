/* Apple/Darwin port — peripheral role (include/ble_peripheral.h). Delegates to the
 * PicoBLEDarwin Swift backend (CBPeripheralManager). Brings the port to role-parity
 * with rp2040: a GATT server driven from Ruby. The value tables stay generic —
 * advertise/notify pass through the same BLE_read_data the rp2040 port uses; the
 * Swift backend's reads/writes are mirrored to/from those tables on the VM thread
 * (see PicoBLEPeripheral.pump, driven from pble_drain_one). */
#include <stdint.h>
#include <stdbool.h>

#include "../../include/ble.h"
#include "../../include/ble_peripheral.h"
#include "ble_common.h"

#include "PicoBLEDarwin-Swift.h"

void
BLE_peripheral_advertise(uint8_t *adv_data, uint8_t adv_data_len, bool connectable)
{
  /* CoreBluetooth advertises connectable whenever a GATT server is published; the
   * connectable flag has no separate knob at this layer. */
  (void)connectable;
  pble_peripheral_advertise(adv_data, (uint16_t)adv_data_len);
}

void
BLE_peripheral_stop_advertise(void)
{
  pble_peripheral_stop_advertise();
}

void
BLE_peripheral_notify(uint16_t att_handle)
{
  /* Read the current value from the generic table (VM thread — safe) and hand the
   * bytes to the Swift backend, mirroring rp2040's BLE_read_data + att_server_notify. */
  BLE_read_value_t read_value = { .att_handle = att_handle, .data = NULL, .size = 0 };
  if (BLE_read_data(&read_value) < 0) return;
  if (read_value.size == 0) return;
  pble_peripheral_notify(att_handle, read_value.data, read_value.size);
}

void
BLE_peripheral_request_can_send_now_event(void)
{
  pble_peripheral_request_can_send_now();
}
