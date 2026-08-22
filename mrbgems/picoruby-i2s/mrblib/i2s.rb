# Generic I2S TX peripheral. StackChan-specific amp (AW88298) control and audio
# decoding live in the application, not here — this gem only streams PCM.
class I2S
  def initialize(sample_rate:, bits: 16, mono: true)
    _init(sample_rate, bits, mono ? 1 : 0)
  end

  # pcm: a little-endian signed-16 byte String. Blocking. Returns bytes written.
  def write(pcm)
    _write(pcm)
  end

  def close
    _deinit
  end
end
