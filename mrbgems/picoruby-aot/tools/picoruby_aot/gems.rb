# Gem graph for picoruby-aot: which PicoRuby gems exist, what they require,
# where their Ruby sources and RBS signatures live.
#
# This runs on the host under CRuby at build time. It never touches the
# target.

require "set"

module PicorubyAot
  class GemInfo
    attr_reader :name, :dir, :require_name, :rbfiles, :sigfiles, :deps

    def initialize(dir, name = nil)
      @dir = File.expand_path(dir)
      rake = File.exist?(rakefile) ? File.read(rakefile) : ""
      @name = name || rake[/Specification\.new[\s(]*['"]([^'"]+)/, 1] || File.basename(@dir)
      @require_name = rake[/require_name\s*=\s*['"]([^'"]+)/, 1] || @name.sub(/\Apicoruby-/, "")
      @rbfiles = Dir.glob("#{@dir}/mrblib/**/*.rb").sort
      @sigfiles = Dir.glob("#{@dir}/sig/**/*.rbs").sort
      @deps = rake.scan(/add_dependency\s+['"]([^'"]+)['"]/).flatten.uniq
    end

    def rakefile
      "#{@dir}/mrbgem.rake"
    end

    def pure_ruby?
      !Dir.exist?("#{@dir}/src")
    end

    def to_s
      name
    end
  end

  class GemGraph
    attr_reader :gems

    def initialize(mrbgems_dir)
      @mrbgems_dir = File.expand_path(mrbgems_dir)
      @gems = {}
      Dir.glob("#{@mrbgems_dir}/picoruby-*/").sort.each do |d|
        g = GemInfo.new(d)
        @gems[g.name] = g
      end
    end

    # The gem in +dir+, named +name+ or after its Specification.new. It
    # replaces a gem of the same name, so the gem the build uses wins over
    # the in-tree one the mrbgems glob found.
    def add(dir, name = nil)
      g = GemInfo.new(dir, name)
      @gems[g.name] = g
    end

    def [](name)
      @gems[name] or raise ArgumentError, "unknown gem: #{name}"
    end

    def each(&blk)
      @gems.values.each(&blk)
    end

    # The gem and every gem in the graph it depends on, transitively.
    # A dependency not in the graph is skipped: mruby-* gems are in it only
    # when the build's gems were added.
    def closure(name)
      seen = Set.new
      queue = [name]
      until queue.empty?
        n = queue.shift
        next if seen.include?(n)
        next unless @gems.key?(n)
        seen << n
        queue.concat(@gems[n].deps)
      end
      seen.map { |n| @gems[n] }
    end

    def by_require_name(req)
      @gems.values.find { |g| g.require_name == req }
    end
  end
end
