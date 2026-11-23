# Gem graph for picoruby-aot: which PicoRuby gems exist, what they require,
# where their Ruby sources and RBS signatures live.
#
# This runs on the host under CRuby at build time. It never touches the
# target.

require "set"

module PicorubyAot
  class GemInfo
    attr_reader :name, :dir, :require_name, :rbfiles, :sigfiles, :deps

    def initialize(dir, name: nil, deps: nil)
      @dir = File.expand_path(dir)
      @name = name || File.basename(@dir)
      rake = File.exist?(rakefile) ? File.read(rakefile) : ""
      @require_name = rake[/require_name\s*=\s*['"]([^'"]+)/, 1] || @name.sub(/\Apicoruby-/, "")
      @rbfiles = Dir.glob("#{@dir}/mrblib/**/*.rb").sort
      @sigfiles = Dir.glob("#{@dir}/sig/**/*.rbs").sort
      @deps = deps || rake.scan(/add_dependency\s+['"]([^'"]+)['"]/).flatten.uniq
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

    # The gems of the build ({"name", "dir", "deps"} each) take their place
    # in the graph: a gem outside mrbgems/ joins it, and a gem already there
    # is replaced, by another directory or by itself with the dependencies
    # the build resolved. Returns the names of all of them.
    def add_build_gems(gems)
      gems.map do |e|
        name = e["name"]
        dir = File.expand_path(e["dir"])
        # Not the upstream mruby gems (under mrbgems/, absent from the
        # graph): they stay out of the wait search. And an in-tree gem does
        # not keep the dependencies read from its mrbgem.rake: those name
        # every branch, and a branch this build does not take would bring
        # into the closure the RBS of a gem the target does not carry.
        next name if dir.start_with?("#{@mrbgems_dir}/") && !@gems.key?(name)
        @gems[name] = GemInfo.new(dir, name: name, deps: e["deps"])
        name
      end
    end

    def [](name)
      @gems[name] or raise ArgumentError, "unknown gem: #{name}"
    end

    def each(&blk)
      @gems.values.each(&blk)
    end

    # The gem and every PicoRuby gem it depends on, transitively.
    # Upstream mruby gems (mruby-*) are not in the graph and are skipped.
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
