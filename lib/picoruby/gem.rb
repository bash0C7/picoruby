module MRuby
  module Gem
    class Specification
      attr_accessor :require_name

      alias_method :original_setup_compilers, :setup_compilers
      def setup_compilers
        original_setup_compilers
        return unless cc.build.posix?
        # setup for POSIX (and POSIX-family ports selected via conf.ports).
        # Pick the first port dir present in effective_ports (e.g. darwin then
        # posix). Fall back to "posix" so host posix builds (effective_ports
        # => ["posix"]) and builds that don't set conf.ports are unchanged.
        platform_port =
          build.effective_ports.find { |p| Dir.exist?("#{dir}/ports/#{p}") } || "posix"
        # Port objects an external `hal-<short>-<conf>` gem replaced: List#resolve_external_hal!
        # already dropped them from objs; they must not come back through this hook.
        replaced = (port_objs || []) - objs
        [platform_port, "common"].each do |subdir|
          # ports/<port>/ext/ holds a Swift package (picoruby-ble builds its own
          # with `swift build`, app-linked ones are built by Xcode); its C shims
          # include package headers and are not libmruby sources.
          ext_prefix = "#{dir}/ports/#{subdir}/ext/"
          Dir.glob("#{dir}/ports/#{subdir}/**/*.c").reject { |src| src.start_with?(ext_prefix) }.each do |src|
            obj = objfile(src.pathmap("#{build_dir}/ports/#{subdir}/%n"))
            next if replaced.include?(obj)
            build.libmruby_objs << obj
            file obj => src do |f|
              cc.run f.name, f.prerequisites.first
            end
          end
        end
      end

      def define_gem_init_builder
        file "#{build_dir}/gem_init.c" => [build.mrbcfile, __FILE__] + [rbfiles].flatten do |t|
          mkdir_p build_dir
          if build.cc.defines.include?("PICORB_VM_MRUBYC") && name.start_with?("picoruby-")
            rbfiles.clear
          end
          generate_gem_init("#{build_dir}/gem_init.c")
        end
      end

    end
  end
end
