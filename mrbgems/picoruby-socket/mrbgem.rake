MRuby::Gem::Specification.new('picoruby-socket') do |spec|
  spec.license = 'MIT'
  spec.author  = 'HASUMI Hitoshi'
  spec.summary = 'CRuby-compatible Socket implementation for PicoRuby'
  spec.description = 'Provides TCPSocket, UDPSocket, and TCPServer classes compatible with CRuby'

  spec.require_name = 'socket'

  # Apple cross-builds (iOS / watchOS) select ports/darwin through
  # `conf.ports :darwin, :posix`: the same BSD sockets as ports/posix, but TLS
  # on mbedTLS because those SDKs ship no OpenSSL. A macOS host build sets no
  # conf.ports, keeps ports/posix and links OpenSSL as before.
  # Same rule as mruby's port loader: the first port in the chain that has a
  # directory in this gem is the one compiled.
  darwin_port = build.effective_ports.find { |p| File.directory?("#{dir}/ports/#{p}") }.to_s == "darwin"

  # Dependencies
  if !build.posix? || darwin_port
    spec.add_dependency 'picoruby-mbedtls'  # SSL/TLS support off the OpenSSL path
  end
  spec.add_dependency 'picoruby-machine'
  spec.add_dependency 'picoruby-metaprog' if build.femtoruby?
  if build.picoruby? && !build.posix? && !build.platform?(:esp32)
    spec.add_dependency 'mruby-task'
  end

  # Add include directory
  spec.cc.include_paths << "#{dir}/include"

  # Add mbedtls include path for SSL support (non-POSIX, and the darwin port)
  if (!build.posix? && !build.platform?(:esp32)) || darwin_port
    mbedtls_dir = "#{MRUBY_ROOT}/mrbgems/picoruby-mbedtls/lib/mbedtls"
    if File.directory?(mbedtls_dir)
      spec.cc.include_paths << "#{mbedtls_dir}/include"
      # Use the same config as picoruby-mbedtls. Without this, TUs in this
      # gem see the default mbedtls_config.h and disagree with libmbedtls
      # about struct layouts (e.g. mbedtls_ssl_config embedded by value in
      # altcp_tls_mbedtls.c).
      spec.cc.defines << "MBEDTLS_CONFIG_FILE='\"#{MRUBY_ROOT}/mrbgems/picoruby-mbedtls/include/mbedtls_config.h\"'"
      # Same -Wundef suppression as picoruby-mbedtls (ssl.h upstream bug)
      spec.cc.flags << '-Wno-undef'
    end
  end

  # include/socket.h types picorb_ssl_context_t / picorb_ssl_socket_t for
  # OpenSSL on POSIX; the darwin port defines them itself (mbedTLS) exactly
  # like the non-POSIX ports, so tell the header to keep them opaque.
  spec.cc.defines << "PICORB_SOCKET_TLS_MBEDTLS" if darwin_port

  unless build.posix? || build.platform?(:esp32)
    # LwIP configuration
    LWIP_VERSION = "STABLE-2_2_1_RELEASE"
    LWIP_REPO = "https://github.com/lwip-tcpip/lwip"
    lwip_dir = "#{dir}/lib/lwip"

    # Clone or update LwIP repository
    if File.symlink?(lwip_dir)
      # Symlink to pico-sdk's lwip (used in R2P2 builds)
      # Note: This modifies a submodule's submodule. To ignore the changes in git status,
      # add 'ignore = dirty' to picoruby-r2p2/lib/pico-sdk in .gitmodules
      unless File.directory?(lwip_dir)
        raise "Symlink #{lwip_dir} exists but target is missing. Run: rake r2p2:setup"
      end
    elsif File.directory?(lwip_dir)
      if File.directory?("#{lwip_dir}/.git")
        current_branch = `cd #{lwip_dir} && git branch --show-current 2>/dev/null`.strip
        current_tag = `cd #{lwip_dir} && git describe --tags --exact-match HEAD 2>/dev/null`.strip

        unless current_branch == LWIP_VERSION || current_tag == LWIP_VERSION
          puts "lwip version mismatch. Fetching and checking out #{LWIP_VERSION}..."
          sh "cd #{lwip_dir} && git fetch origin #{LWIP_VERSION}:#{LWIP_VERSION} 2>/dev/null || git fetch origin"
          sh "cd #{lwip_dir} && git checkout #{LWIP_VERSION}"
        end
      else
        puts "lwip directory exists but is not a git repository. Removing and cloning..."
        FileUtils.rm_rf(lwip_dir)
        sh "git clone -b #{LWIP_VERSION} #{LWIP_REPO} #{lwip_dir}"
      end
    else
      sh "git clone -b #{LWIP_VERSION} #{LWIP_REPO} #{lwip_dir}"
    end

    # Apply patches to LwIP
    patch_file = "#{dir}/patches/lwip-altcp-proxyconnect.patch"
    if File.exist?(patch_file)
      proxyconnect_file = "#{lwip_dir}/src/apps/http/altcp_proxyconnect.c"
      if File.exist?(proxyconnect_file)
        patch_applied = `cd #{lwip_dir} && git apply --check #{patch_file} 2>&1`.strip
        if patch_applied.empty?
          sh "cd #{lwip_dir} && git apply #{patch_file}"
          puts "Applied patch: lwip-altcp-proxyconnect.patch"
        end
      end
    end

    spec.cc.defines << 'PICO_CYW43_ARCH_POLL=1'

    # Add LwIP include paths
    spec.cc.include_paths << "#{lwip_dir}/src/include"
    spec.cc.include_paths << "#{lwip_dir}/contrib/ports/unix/port/include"
    spec.cc.include_paths << "#{lwip_dir}/src/apps/altcp_tls"

    # Compile LwIP source files
    Dir.glob("#{lwip_dir}/src/**/*.c").each do |src|
      next if src.end_with?('makefsdata.c')
      next if src.end_with?('altcp_tls_mbedtls.c')  # Use custom version from ports/rp2040
      obj = src.relative_path_from(dir).pathmap("#{build_dir}/%X.o")
      spec.objs << obj
    end
  end
end
