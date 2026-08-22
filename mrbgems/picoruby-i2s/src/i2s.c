/* picoruby-i2s — generic I2S TX peripheral binding.
 *
 * VM bindings + dual-VM dispatch ONLY. This file is compiled into libmruby by
 * the mruby gem build, which does NOT carry the ESP-IDF include path, so it must
 * NOT include any driver/ header. The actual esp_driver_i2s implementation
 * (I2S_init / I2S_write / I2S_deinit, declared in include/i2s.h) lives in
 * ports/esp32/i2s.c, compiled by the picoruby-esp32 IDF component directly
 * (its SRCS lists that file, where the IDF include path is available).
 * Mirrors picoruby-rmt's src/ (libmruby) + ports/esp32/ (component) split
 * exactly.
 */
#include "../include/i2s.h"

#if defined(PICORB_VM_MRUBY)
#include "mruby/i2s.c"
#elif defined(PICORB_VM_MRUBYC)
#include "mrubyc/i2s.c"
#endif
