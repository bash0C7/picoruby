/* Declarations-only C target. CBLEBridge contributes no symbols of its own; it
 * exists so the Swift target can import the declarations of BLE_read_data /
 * BLE_write_data (defined in the gem's shared src/mruby/ble.c, bound at load
 * time via dynamic_lookup). This translation unit is intentionally empty. */
#include "CBLEBridge.h"
