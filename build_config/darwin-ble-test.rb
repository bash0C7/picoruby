MRuby::Build.new("darwin-ble-test") do |conf|
  conf.toolchain :clang

  conf.cc.defines << "MRB_TICK_UNIT=4"
  conf.cc.defines << "MRB_TIMESLICE_TICK_COUNT=3"
  conf.cc.defines << "PICORB_ALLOC_ALIGN=8"
  conf.cc.defines << "PICORB_ALLOC_ESTALLOC"
  # Required for build.posix? (lib/picoruby/gem.rb) to be true, which is what
  # makes gems compile their ports/common/*.c alongside ports/<port>/*.c —
  # without it, picoruby-mbedtls links with cipher/cmac/digest/hmac/pkey
  # wrapper symbols undefined (ports/common/*.c never compiled).
  conf.cc.defines << "PICORB_PLATFORM_POSIX"
  conf.cc.defines << "PICORB_PLATFORM_DARWIN"
  conf.cc.defines << "MRB_INT64"
  conf.cc.defines << "MRB_NO_BOXING"
  conf.cc.defines << "MRB_UTF8_STRING"

  conf.picoruby

  conf.gem core: "mruby-compiler"
  conf.gem core: "mruby-bin-mrbc"
  # picoruby-picotest depends on 'mruby-metaprog' by bare name (no core:/gemdir:),
  # which otherwise resolves via a network mgem-list fetch. Load it locally first.
  conf.gem gemdir: "#{MRUBY_ROOT}/mrbgems/picoruby-mruby/lib/mruby/mrbgems/mruby-metaprog"
  # picoruby-picotest's list_tests uses String#start_with?, which isn't in the
  # base VM without this gem.
  conf.gem gemdir: "#{MRUBY_ROOT}/mrbgems/picoruby-mruby/lib/mruby/mrbgems/mruby-string-ext"
  # Produces build/darwin-ble-test/bin/picoruby, the executable the test files run under.
  conf.gem core: "picoruby-bin-picoruby"
  conf.gem core: "picoruby-ble"
  conf.gem core: "picoruby-picotest"
  # Kernel#exit, used by the test files' own driver boilerplate at the bottom.
  conf.gem core: "picoruby-machine"
end
