/* mruby bindings for I2S (included by ../i2s.c when PICORB_VM_MRUBY). */
#include <mruby.h>
#include <mruby/string.h>
#include "../../include/i2s.h"

static mrb_value mrb_i2s__init(mrb_state *mrb, mrb_value self)
{
  mrb_int rate, bits, mono;
  mrb_get_args(mrb, "iii", &rate, &bits, &mono);
  return mrb_fixnum_value(I2S_init((uint32_t)rate, (uint8_t)bits, (uint8_t)mono));
}

static mrb_value mrb_i2s__write(mrb_state *mrb, mrb_value self)
{
  char *buf; mrb_int len;
  mrb_get_args(mrb, "s", &buf, &len);
  return mrb_fixnum_value(I2S_write((const uint8_t *)buf, (uint32_t)len));
}

static mrb_value mrb_i2s__deinit(mrb_state *mrb, mrb_value self)
{
  return mrb_fixnum_value(I2S_deinit());
}

void mrb_picoruby_i2s_gem_init(mrb_state *mrb)
{
  struct RClass *c = mrb_define_class(mrb, "I2S", mrb->object_class);
  mrb_define_method(mrb, c, "_init",   mrb_i2s__init,   MRB_ARGS_REQ(3));
  mrb_define_method(mrb, c, "_write",  mrb_i2s__write,  MRB_ARGS_REQ(1));
  mrb_define_method(mrb, c, "_deinit", mrb_i2s__deinit, MRB_ARGS_NONE());
}

void mrb_picoruby_i2s_gem_final(mrb_state *mrb) { (void)mrb; }
