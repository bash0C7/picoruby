#ifndef BLE_DEFINED_H_
#define BLE_DEFINED_H_

#include <stdint.h>
#include <stdbool.h>
#include "ble_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

enum BLE_role_t {
  BLE_ROLE_NONE = 0,
  BLE_ROLE_CENTRAL,
  BLE_ROLE_PERIPHERAL,
  BLE_ROLE_BROADCASTER,
  BLE_ROLE_OBSERVER
};

#if defined(PICORB_VM_MRUBY)
#include "mruby.h"
bool mrb_ble_active_owner(mrb_state *mrb, mrb_value self);
void mrb_ble_check_active_owner(mrb_state *mrb, mrb_value self);
void mrb_init_class_BLE_Peripheral(mrb_state *mrb, struct RClass *class_BLE);
void mrb_init_class_BLE_Broadcaster(mrb_state *mrb, struct RClass *class_BLE);
void mrb_init_class_BLE_Central(mrb_state *mrb, struct RClass *class_BLE);
#elif defined(PICORB_VM_MRUBYC)
#include "mrubyc.h"
bool mrbc_ble_active_owner(mrbc_value *self);
bool mrbc_ble_check_active_owner(mrbc_vm *vm, mrbc_value *self);
void mrbc_init_class_BLE_Peripheral(mrbc_vm *vm, mrbc_class *class_BLE);
void mrbc_init_class_BLE_Broadcaster(mrbc_vm *vm, mrbc_class *class_BLE);
void mrbc_init_class_BLE_Central(mrbc_vm *vm, mrbc_class *class_BLE);
#endif

typedef struct {
  uint16_t att_handle;
  uint8_t *data;
  uint16_t size;
} BLE_read_value_t;

bool BLE_ready(void);
int BLE_control_fault(void);
void BLE_vm_attached(void *vm);
void BLE_events_cleared(void);
void BLE_read_value_pushed(uint16_t att_handle, const uint8_t *data, uint16_t size);

int BLE_init(const uint8_t *profile, size_t profile_len, int ble_role);
enum BLE_power_control_result_t {
  BLE_POWER_CONTROL_NOT_READY = -3
};
int BLE_hci_power_control(uint8_t power_mode);
int BLE_gap_local_bd_addr(uint8_t *local_addr);
int BLE_push_event(uint8_t *data, uint16_t size);
int BLE_heartbeat(void);
int BLE_write_data(uint16_t att_handle, const uint8_t *data, uint16_t size);
int BLE_read_data(BLE_read_value_t *read_value);
uint8_t BLE_discover_primary_services(uint16_t conn_handle);
uint8_t BLE_discover_characteristics_for_service(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle);
uint8_t BLE_read_value_of_characteristic_using_value_handle(uint16_t conn_handle, uint16_t value_handle);
uint8_t BLE_discover_characteristic_descriptors(uint16_t conn_handle, uint16_t value_handle, uint16_t end_handle);
uint8_t BLE_write_value_of_characteristic_without_response(uint16_t conn_handle, uint16_t value_handle, const uint8_t *data, uint16_t size);
uint8_t BLE_write_characteristic_descriptor_using_descriptor_handle(uint16_t conn_handle, uint16_t descriptor_handle, const uint8_t *data, uint16_t size);

#ifdef __cplusplus
}
#endif

#endif /* BLE_DEFINED_H_ */

