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

  # Bare CLI Mach-O (no .app bundle), but CoreBluetooth's TCC check still
  # requires NSBluetoothAlwaysUsageDescription somewhere it can read — embed
  # an Info.plist directly in __TEXT via the standard sectcreate trick used
  # by other Bluetooth-touching command-line tools on macOS.
  conf.linker.flags << "-Wl,-sectcreate,__TEXT,__info_plist,#{File.expand_path("darwin-ble-test-Info.plist", __dir__)}"

  conf.gem core: "mruby-compiler"
  conf.gem core: "mruby-bin-mrbc"
  # picoruby-picotest depends on 'mruby-metaprog' by bare name (no core:/gemdir:),
  # which otherwise resolves via a network mgem-list fetch. Load it locally first.
  conf.gem gemdir: "#{MRUBY_ROOT}/mrbgems/picoruby-mruby/lib/mruby/mrbgems/mruby-metaprog"
  # picoruby-picotest's list_tests uses String#start_with?, which isn't in the
  # base VM without this gem.
  conf.gem gemdir: "#{MRUBY_ROOT}/mrbgems/picoruby-mruby/lib/mruby/mrbgems/mruby-string-ext"
  # mrblib/ble_advertising_report.rb's AdvertisingReport#initialize falls back
  # to Kernel#sprintf for an unrecognized AD type byte — not in the base VM
  # without this gem. Confirmed on real hardware: the darwin :central scan
  # DID receive and start parsing a live ESP32 advertisement, then crashed
  # here with NoMethodError before this gem was added.
  conf.gem gemdir: "#{MRUBY_ROOT}/mrbgems/picoruby-mruby/lib/mruby/mrbgems/mruby-sprintf"
  # Same file's initialize also uses Array#pack('C') to build @address —
  # confirmed on real hardware (NoMethodError: undefined method 'pack').
  conf.gem gemdir: "#{MRUBY_ROOT}/mrbgems/picoruby-mruby/lib/mruby/mrbgems/mruby-pack"
  # picoruby-picotest's failure/exception reporting calls Kernel#caller —
  # confirmed on real hardware (NoMethodError: undefined method 'caller')
  # when an assertion fails. Adding proactively: each rebuild of this binary
  # invalidates the ad-hoc-signed bundle's TCC Bluetooth grant, requiring a
  # manual re-grant in System Settings, so batching gem gaps found via
  # hardware runs avoids extra round-trips.
  conf.gem gemdir: "#{MRUBY_ROOT}/mrbgems/picoruby-mruby/lib/mruby/mrbgems/mruby-kernel-ext"
  # e2e_broadcaster_scan_test.rb's assertion uses Array#uniq — confirmed on
  # real hardware (NoMethodError: undefined method 'uniq'). Added alongside
  # the other gaps above for the same re-sign-invalidates-TCC reason.
  conf.gem gemdir: "#{MRUBY_ROOT}/mrbgems/picoruby-mruby/lib/mruby/mrbgems/mruby-array-ext"
  # Produces build/darwin-ble-test/bin/picoruby, the executable the test files run under.
  conf.gem core: "picoruby-bin-picoruby"
  conf.gem core: "picoruby-ble"
  conf.gem core: "picoruby-picotest"
  # Kernel#exit, used by the test files' own driver boilerplate at the bottom.
  conf.gem core: "picoruby-machine"
end
