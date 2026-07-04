/* picoruby-i2s ESP-IDF port: I2S TX over esp_driver_i2s.
 *
 * Compiled by the picoruby-esp32 IDF component (this file's path is added to
 * that component's idf_component_register SRCS directly, same as
 * picoruby-rmt/ports/esp32/rmt.c), so it has the esp_driver_i2s include path
 * that a libmruby gem compile lacks. Implements the portable
 * I2S_init / I2S_write / I2S_deinit declared in include/i2s.h; the VM bindings
 * in src/{mruby,mrubyc}/i2s.c call these as extern symbols, resolved at the
 * final link. Mirrors picoruby-rmt/ports/esp32/rmt.c.
 */
#include "driver/i2s_std.h"
#include "../../include/i2s.h"

/* CoreS3 audio pins (M5Unified M5Unified.cpp:1820-1822, board_M5StackCoreS3).
 * MCLK is intentionally UNUSED: M5Unified sets no pin_mck for the CoreS3
 * speaker (AW88298 derives its clock from BCLK), and GPIO0 is an ESP32-S3 boot
 * strapping pin, so driving MCLK there is both unnecessary and risky. */
#define I2S_SPK_BCLK GPIO_NUM_34
#define I2S_SPK_WS   GPIO_NUM_33
#define I2S_SPK_DOUT GPIO_NUM_13

static i2s_chan_handle_t s_tx = 0;

int I2S_init(uint32_t sample_rate, uint8_t bits, uint8_t mono)
{
  (void)bits; (void)mono; /* mono handled by L/R duplication in I2S_write */
  /* I2S_NUM_1: M5Unified routes the CoreS3 speaker to the last I2S port. */
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  if (i2s_new_channel(&chan, &s_tx, 0) != ESP_OK) return -1;
  i2s_std_config_t std = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
    /* S3 (HW v2) PHILIPS macro forces slot_mask=BOTH; mono via duplication in write. */
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED, .bclk = I2S_SPK_BCLK, .ws = I2S_SPK_WS,
      .dout = I2S_SPK_DOUT, .din = I2S_GPIO_UNUSED,
    },
  };
  if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK) return -1;
  if (i2s_channel_enable(s_tx) != ESP_OK) return -1;
  return 0;
}

int I2S_write(const uint8_t *pcm, uint32_t nbytes)
{
  const int16_t *src = (const int16_t *)pcm;
  uint32_t n = nbytes / 2;            /* mono sample count */
  uint32_t total = 0;
  int16_t st[256 * 2];                /* stereo chunk buffer (1 KB on stack) */
  uint32_t i = 0;
  /* Write in 256-sample chunks instead of one i2s_channel_write per sample:
   * a 2.3s clip is ~18.5k samples, so per-sample writes meant ~18.5k FreeRTOS
   * driver calls (each with a 1s timeout), stalling the caller for seconds. */
  while (i < n) {
    uint32_t m = (n - i) < 256 ? (n - i) : 256;
    for (uint32_t k = 0; k < m; k++) {
      st[2 * k]     = src[i + k];     /* duplicate mono into L + R */
      st[2 * k + 1] = src[i + k];
    }
    size_t w = 0;
    if (i2s_channel_write(s_tx, st, m * 2 * sizeof(int16_t), &w, 1000) != ESP_OK) return -1;
    total += (uint32_t)w;
    i += m;
  }
  return (int)total;
}

int I2S_deinit(void)
{
  if (!s_tx) return 0;
  i2s_channel_disable(s_tx);
  i2s_del_channel(s_tx);
  s_tx = 0;
  return 0;
}
