/* Apple/Darwin port — central role entry points (include/ble_central.h).
 * Delegates to the PicoBLEDarwin Swift backend (CBCentralManager). */
#include <stdint.h>

#include "../../include/ble_central.h"
#include "ble_common.h"
#include "PicoBLEDarwin-Swift.h"

void
BLE_central_set_scan_params(uint8_t scan_type, uint16_t scan_interval, uint16_t scan_window, uint8_t scanning_filter_policy)
{
  /* CoreBluetooth has no interval/window/filter knobs at this layer. */
  (void)scan_type; (void)scan_interval; (void)scan_window; (void)scanning_filter_policy;
}

void
BLE_central_start_scan(void)
{
  pble_start_scan();
}

void
BLE_central_stop_scan(void)
{
  pble_stop_scan();
}

uint8_t
BLE_central_gap_connect(const uint8_t *addr, uint8_t addr_type)
{
  return (uint8_t)pble_connect(addr, addr_type);
}
