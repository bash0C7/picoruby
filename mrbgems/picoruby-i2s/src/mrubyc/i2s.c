/* mrubyc bindings for I2S (included by ../i2s.c when PICORB_VM_MRUBYC). */
#include <mrubyc.h>
#include "../../include/i2s.h"

static void c_i2s__init(mrbc_vm *vm, mrbc_value *v, int argc)
{
  (void)argc;
  int rate = GET_INT_ARG(1), bits = GET_INT_ARG(2), mono = GET_INT_ARG(3);
  SET_INT_RETURN(I2S_init((uint32_t)rate, (uint8_t)bits, (uint8_t)mono));
}

static void c_i2s__write(mrbc_vm *vm, mrbc_value *v, int argc)
{
  (void)argc;
  mrbc_value s = GET_ARG(1);
  SET_INT_RETURN(I2S_write(s.string->data, (uint32_t)s.string->size));
}

static void c_i2s__deinit(mrbc_vm *vm, mrbc_value *v, int argc)
{
  (void)argc; (void)v;
  SET_INT_RETURN(I2S_deinit());
}

void mrbc_i2s_init(mrbc_vm *vm)
{
  mrbc_class *c = mrbc_define_class(vm, "I2S", mrbc_class_object);
  mrbc_define_method(vm, c, "_init",   c_i2s__init);
  mrbc_define_method(vm, c, "_write",  c_i2s__write);
  mrbc_define_method(vm, c, "_deinit", c_i2s__deinit);
}
