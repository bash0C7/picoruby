# picoruby-i2s

Generic **I2S TX** peripheral binding for PicoRuby on ESP32 (wraps ESP-IDF
`esp_driver_i2s`, std mode). A building block like `picoruby-rmt` / `picoruby-i2c`:
it only streams PCM out I2S. Device-specific amp control (e.g. AW88298) and audio
decoding belong in the application, not here.

> **Status: DRAFT (Phase 4 Spike A, stackchan-picoruby).** The interface
> (`mrblib` / `include` / `sig`) is settled; the ESP-IDF C in `src/` is authored to
> convention but **not yet compiled/linked** — its build through the unified mrbgem
> process (IDF header availability, `esp_driver_i2s` link, active VM) is verified at
> the first device build.

## Install

Add to your ESP32 build config (gem selection only — no firmware code change):

```ruby
conf.gem github: 'bash0C7/picoruby-i2s'
# or, during local bring-up:
conf.gem gemdir: '/Users/bash/dev/src/github.com/bash0C7/picoruby-i2s'
```

## Usage

```ruby
require 'i2s'

spk = I2S.new(sample_rate: 8000)   # mono, 16-bit LE by default
spk.write(pcm_le_int16_string)     # blocking
spk.close
```

Pins (CoreS3): MCLK=GPIO0, BCLK=GPIO34, WS=GPIO33, DOUT=GPIO13. Mono is emitted by
duplicating each sample into both I2S slots (the ESP32-S3 PHILIPS slot macro forces
`slot_mask=BOTH`).
