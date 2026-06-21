#ifndef CBLE_BRIDGE_H_
#define CBLE_BRIDGE_H_

/* The two generic value-table functions the peripheral backend must reach from
 * Swift (CBPeripheralManager read/write/subscribe callbacks). They are DEFINED
 * in the gem's shared src/mruby/ble.c and linked into the final binary; this
 * header only DECLARES them so the Swift target can call them. The PicoBLEDarwin
 * dylib leaves both symbols undefined (`-undefined dynamic_lookup`); the dynamic
 * linker binds them at load time from the host executable (libmruby).
 *
 * BLE_read_value_t mirrors the identical struct in the gem's include/ble.h — same
 * fields, same layout — so the pointer passed to BLE_read_data is ABI-compatible. */

#include <stdint.h>

typedef struct {
  uint16_t att_handle;
  uint8_t *data;
  uint16_t size;
} BLE_read_value_t;

int BLE_write_data(uint16_t att_handle, const uint8_t *data, uint16_t size);
int BLE_read_data(BLE_read_value_t *read_value);

#endif /* CBLE_BRIDGE_H_ */
