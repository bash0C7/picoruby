MRuby::Gem::Specification.new('picoruby-ble') do |spec|
  spec.license = 'MIT'
  spec.author  = 'HASUMI Hitoshi'
  spec.summary = 'BLE class'
  spec.add_dependency 'picoruby-cyw43' unless build.darwin?
  spec.add_dependency 'picoruby-mbedtls'

  # Apple/Darwin (CoreBluetooth) host port. Unlike rp2040/esp32 (whose port .c
  # are compiled by R2P2's CMake / ESP-IDF), the host build has no external
  # harness, so the gem compiles ports/darwin/*.c itself when targeting Darwin.
  if build.darwin?
    # The port's own CoreBluetooth Swift backend: build to a dynamic library and
    # emit a C header for the port C to call.
    # PICORB_PLATFORM_DARWIN (matches build.darwin? predicate + other gems' platform
    # gating) gates the shared src/mruby/ble.c pop_packet FIFO drain.
    spec.cc.defines << 'PICORB_PLATFORM_DARWIN'

    ext_dir = "#{dir}/ports/darwin/ext"
    header  = "#{ext_dir}/PicoBLEDarwin-Swift.h"
    # Build the dynamic library and emit a C header for the port C to include.
    system("swift", "build", "-c", "release", "--package-path", ext_dir,
           "--product", "PicoBLEDarwin",
           "-Xswiftc", "-emit-clang-header-path", "-Xswiftc", header) or
      raise "swift build (PicoBLEDarwin) failed"
    spec.cc.include_paths << ext_dir

    lib_dir = "#{ext_dir}/.build/release"
    build.linker.flags     << "-L#{lib_dir}"
    build.linker.libraries << "PicoBLEDarwin"
    build.linker.flags     << "-Wl,-rpath,#{lib_dir}"

    spec.objs += Dir.glob("#{dir}/ports/darwin/*.c").map do |f|
      f.relative_path_from(dir).pathmap("#{build_dir}/%X.o")
    end
  end
end
