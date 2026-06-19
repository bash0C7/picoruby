MRuby::Build.new do |conf|
  conf.toolchain :gcc

  conf.cc.defines << "MRB_TICK_UNIT=4"
  conf.cc.defines << "MRB_TIMESLICE_TICK_COUNT=3"

  conf.cc.defines << "PICORB_PLATFORM_POSIX"
  # macOS host builds are also Darwin: activates build.darwin? so the picoruby-ble
  # CoreBluetooth port (mrbgems/picoruby-ble/ports/darwin) compiles when the gem is
  # included. Inert for builds that omit picoruby-ble (no gembox pulls it).
  conf.cc.defines << "PICORB_PLATFORM_DARWIN" if RUBY_PLATFORM.include?("darwin")

  conf.cc.defines << "MRB_INT64"
  conf.cc.defines << "MRB_NO_BOXING"
  conf.cc.defines << "MRB_UTF8_STRING"

  conf.picoruby

  # Link OpenSSL libraries for socket SSL support
  conf.linker.libraries << 'ssl'
  conf.linker.libraries << 'crypto'

  conf.gembox "mruby-posix"
  conf.gembox "minimum"
  conf.gembox "core"
  conf.gembox "stdlib"
  conf.gembox "shell"
  conf.gembox "networking"
  conf.gem core: "picoruby-shinonome"
  conf.gem core: "picoruby-bin-r2p2"
end
