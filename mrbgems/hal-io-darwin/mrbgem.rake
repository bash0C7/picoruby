MRuby::Gem::Specification.new('hal-io-darwin') do |spec|
  spec.license = 'MIT'
  spec.authors = 'bash0C7'
  spec.summary = 'Darwin HAL for mruby-io: the POSIX HAL, minus process spawning where the SDK forbids it (watchOS / tvOS)'

  # External HAL provider for mruby-io (mruby's `hal-<short>-<conf>` naming):
  # when this gem is in the build, mruby-io's ports/posix/io_hal.c is dropped
  # and src/io_hal.c supplies the HAL. Depend on mruby-io for io_hal.h.
  mruby_io = "#{MRUBY_ROOT}/mrbgems/picoruby-mruby/lib/mruby/mrbgems/mruby-io"
  spec.add_dependency 'mruby-io', gemdir: mruby_io
  spec.cc.include_paths << "#{mruby_io}/include"
end
