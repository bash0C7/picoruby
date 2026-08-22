#ifndef PICORUBY_I2S_H_
#define PICORUBY_I2S_H_

#include <stdint.h>

/* Returns 0 on success, -1 on failure. mono != 0 duplicates each sample into
 * both I2S slots (the ESP32-S3 PHILIPS slot macro forces slot_mask=BOTH). */
int I2S_init(uint32_t sample_rate, uint8_t bits, uint8_t mono);

/* Blocking write of a little-endian signed-16 PCM buffer. Returns bytes written, -1 on error. */
int I2S_write(const uint8_t *pcm, uint32_t nbytes);

int I2S_deinit(void);

#endif
