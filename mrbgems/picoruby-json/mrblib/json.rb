#
# JSON library for PicoRuby
# This is a simple JSON parser and generator for PicoRuby.
# It is designed to be small and simple, not to be fast or complete.
#
# Author: Hitoshi HASUMI
# License: MIT
#


module JSON

  class JSONError < StandardError; end
  class ParserError < JSONError; end
  class GeneratorError < JSONError; end
  class DiggerError < JSONError; end

  # Detect WASM build for Regexp-based optimization (lazy, cached)
  def self.wasm_build?
    return @wasm_build unless @wasm_build.nil?
    @wasm_build = RUBY_DESCRIPTION.include?("wasm32")
  end

  # Toggle for Regexp optimization (can be set to false for benchmarking)
  # Defaults to wasm_build? but can be overridden: JSON.use_regexp = false
  def self.use_regexp?
    return @use_regexp unless @use_regexp.nil?
    @use_regexp = wasm_build?
  end

  def self.use_regexp=(val)
    @use_regexp = val
  end

  # Regexp patterns (lazy initialization)
  def self.ws_pattern
    @ws_pattern ||= /^[ \t\n\r]+/
  end

  def self.string_content_pattern
    @string_content_pattern ||= /^[^"\\]+/
  end

  def self.number_pattern
    @number_pattern ||= /^-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/
  end

  module Common
    def expect(char)
      if @json[@index] != char
        raise JSON::JSONError.new("Expected '#{char}' at index #{@index}, but got '#{@json[@index]}'")
      end
      @index += 1
    end

    def skip_whitespace
      if JSON.use_regexp?
        if md = JSON.ws_pattern.match(@json, @index)
          @index = md.end(0)
        end
      else
        while @index < @json.length && [' ', "\t", "\n", "\r"].include?(@json[@index])
          @index += 1
        end
      end
    end

    def parse_true
      expect_sequence('true')
      true
    end

    def parse_false
      expect_sequence('false')
      false
    end

    def parse_null
      expect_sequence('null')
      nil
    end

    def expect_sequence(sequence)
      sequence.each_char do |char|
        expect(char)
      end
    end
  end

  def self.parse(json)
    JSON::Parser.new(json).parse
  end

  def self.generate(obj)
    JSON::Generator.new(obj).generate
  end

  # Digger class is to dig into the JSON object especially dedicated
  # to the small memory environment.
  # It does not parse the whole JSON object but it scans the JSON string
  # and extract a part of the JSON object.
  #
  # Usage:
  #   json = <<~JSON
  #     [{"id":"02e548eb-900d-4de4-93d8-xxxxxxxxxxxx","device":{"name":"MyName","id":"9b098976-98fb-439f-ac09-xxxxxxxxxxxx","created_at":"2024-09-19T01:08:25Z","updated_at":"2024-09-19T08:53:00Z","mac_address":"f0:08:d1:ea:da:00","bt_mac_address":"f0:08:d1:ea:da:02","serial_number":"4W121010002448","firmware_version":"Remo-E-lite/1.10.0","temperature_offset":0,"humidity_offset":0},"model":{"id":"7f3de26b-0afa-44fe-8680-xxxxxxxxxxxx","manufacturer":"","name":"Smart Meter","image":"ico_smartmeter"},"type":"EL_SMART_METER","nickname":"NyNickname","image":"ico_smartmeter","settings":null,"aircon":null,"signals":[],"smart_meter":{"echonetlite_properties":[{"name":"coefficient","epc":211,"val":"1","updated_at":"2024-09-20T01:44:15Z"},{"name":"cumulative_electric_energy_effective_digits","epc":215,"val":"6","updated_at":"2024-09-20T01:44:15Z"},{"name":"normal_direction_cumulative_electric_energy","epc":224,"val":"80481","updated_at":"2024-09-20T01:44:15Z"},{"name":"cumulative_electric_energy_unit","epc":225,"val":"1","updated_at":"2024-09-20T01:44:15Z"},{"name":"reverse_direction_cumulative_electric_energy","epc":227,"val":"9","updated_at":"2024-09-20T01:44:15Z"},{"name":"measured_instantaneous","epc":231,"val":"599","updated_at":"2024-09-20T01:46:14Z"}]}}]
  #   JSON
  #
  #   json = '[{"device":{"name":"Remo"}}, {"device":{"name":null}}]'
  #   JSON::Digger.new(json).dig(0, 'device', 'name')
  #   => Digger object
  #   JSON::Digger.new(json).dig(0, 'device', 'name').parse
  #   => "Remo"
  #   JSON::Digger.new(json).dig(0, 'device')
  #   => Digger object
  #   JSON::Digger.new(json).dig(0, 'device').parse
  #   => {"name"=>"Remo"}
  #   JSON::Digger.new(json).dig(1, 'device', 'name').parse
  #   => nil
  #   JSON::Digger.new(json).dig(0, 'device').dig('name').parse
  #   => "Remo"
  #   JSON::Digger.new(json).dig(3, 'device', 'name')
  #   => JSON::DiggerError: Array index out of range
  #   JSON::Digger.new(json).dig(1, '___device', 'name')
  #   => JSON::DiggerError: Key not found: ___device
  #
  class Digger
    include JSON::Common

    def initialize(json)
      @json = json
      reset
    end

    # attr_reader :json

    def dig(*keys)
      ki = 0
      while ki < keys.size
        key = keys[ki]
        case key
        when Integer
          if key < 0
            raise ArgumentError.new("Negative index is not supported")
          end
          # @type var key: Integer
          dig_array(key)
        when String
          # @type var key: String
          dig_object(key)
        else
          raise ArgumentError.new("Unsupported type: #{key.class}")
        end
        @json = @json[@start_index, @index - @start_index]
        @json.strip!
        reset
        # p @json
        ki += 1
      end
      return self
    end

    def parse
      JSON.parse(@json)
    end

    # private

    def reset
      @start_index = 0
      @index = 0
      @stack = []
    end

    # Override to never use Regexp for Digger (performance)
    def skip_whitespace
      while @index < @json.length && [' ', "\t", "\n", "\r"].include?(@json[@index] || "")
        @index += 1
      end
    end

    def push_stack(type)
      @stack.push(type)
      #puts "push_stack: #{@stack}, index: #{@index}"
    end

    def pop_stack
      @stack.pop
      #puts "pop_stack: #{@stack}, index: #{@index}"
    end

    def dig_string(need_return)
      skip_whitespace
      expect('"')
      string_start = @index
      while char = @json[@index]
        if char == '\\'
          @index += 2
        elsif char == '"'
          if need_return
            str = @json[string_start, @index - string_start]
          end
          @index += 1
          return str
        else
          @index += 1
        end
      end
      raise JSON::DiggerError.new("Unterminated string")
    end

    def dig_number
      while char = @json[@index]
        if char == '-' || char == '.' || char == 'e' || char == 'E' || ('0' <= char && char <= '9')
          @index += 1
        else
          break
        end
      end
    end

    def dig_object(key)
      push_stack(:object)
      skip_whitespace
      expect('{')
      while char = @json[@index]
        if char == '}'
          @index += 1
          if @stack[-1] == :object
            pop_stack
            break
          end
        end
        found_key = dig_string(true)
        if key && found_key == key
          skip_whitespace
          expect(':')
          @start_index = @index
          dig_value
          return
        else
          skip_whitespace
          expect(':')
          dig_value
          skip_whitespace
          if @json[@index] == '}'# && @stack[-1] == :object
            @index += 1
            pop_stack
            break
          end
          expect(',')
          skip_whitespace
        end
      end
      if key
        raise JSON::DiggerError.new("Key not found: #{key}")
      end
    end

    def dig_value
      skip_whitespace
      case @json[@index]
      when '{'
        dig_object(nil)
      when '['
        dig_array(nil)
      when '"'
        dig_string(false)
      when 't'
        skip_whitespace
        parse_true
      when 'f'
        skip_whitespace
        parse_false
      when 'n'
        skip_whitespace
        parse_null
      when '-', '0'
        dig_number
      else
        if @json[@index].to_i != 0 # from '1' to '9'
          dig_number
        else
          @index += 1
        end
      end
    end

    def dig_array(array_pos)
      push_stack(:array)
      skip_whitespace
      expect('[')
      skip_whitespace
      current_array_pos = 0
      @start_index = @index if array_pos
      while char = @json[@index]
        case char
        when ']'
          @index += 1
          break
        when ','
          @index += 1
          if @stack[-1] == :array
            if array_pos
              if current_array_pos == array_pos
                pop_stack
                break
              end
              @start_index = @index
              current_array_pos += 1
            end
          end
        else
          dig_value
        end
      end
      if array_pos && current_array_pos < array_pos
        JSON::DiggerError.new("Array index out of range")
      end
    end
  end

  class Generator
    include JSON::Common

    def initialize(obj)
      @obj = obj
    end

    def generate(obj = @obj)
      case obj
      when Hash
        generate_object(obj)
      when Array
        generate_array(obj)
      when String, Symbol
        generate_string(obj)
      when Integer, Float
        generate_number(obj)
      when TrueClass
        "true"
      when FalseClass
        "false"
      when NilClass
        'null'
      else
        generate_string(obj.to_s)
      end
    end

    # private

    def generate_object(obj)
      result = '{'
      keys = obj.keys
      i = 0
      while i < keys.size
        result += ',' if 0 < i
        key = keys[i]
        result += "#{generate_string(key)}:#{generate(obj[key])}"
        i += 1
      end
      result += '}'
    end

    def generate_array(obj)
      result = '['
      i = 0
      while i < obj.size
        result += ',' if 0 < i
        result += generate(obj[i])
        i += 1
      end
      result += ']'
    end

    HEX_CHARS = "0123456789abcdef"

    def generate_string(obj)
      # Manually escape special characters since PicoRuby does not support gsub nor Regexp
      str = obj.to_s
      result = '"'
      i = 0
      while i < str.length
        char = str[i]
        case char
        when '"'
          result += '\\"'
        when '\\'
          result += '\\\\'
        when "\b"
          result += '\\b'
        when "\f"
          result += '\\f'
        when "\n"
          result += '\\n'
        when "\r"
          result += '\\r'
        when "\t"
          result += '\\t'
        else
          if char
            code = char.ord
            if code < 0x20
              # Escape control characters as \u00XX
              result += '\\u00'
              result += (HEX_CHARS[code >> 4] || '0')
              result += (HEX_CHARS[code & 0x0f] || '0')
            else
              result += char
            end
          end
        end
        i += 1
      end
      result += '"'
      result
    end

    def generate_number(obj)
      obj.to_s
    end
  end

  class Parser
    include JSON::Common

    def initialize(json)
      @json = json
      @index = 0
      @char_index = 0
    end

    def parse
      skip_whitespace
      case @json.getbyte(@index)
      when 0x7b # '{'
        parse_object
      when 0x5b # '['
        parse_array
      when 0x22 # '"'
        parse_string
      when 0x2d, 0x30..0x39 # '-', '0'..'9'
        parse_number
      when 0x74 # 't'
        parse_true
      when 0x66 # 'f'
        parse_false
      when 0x6e # 'n'
        parse_null
      else
        raise JSON::ParserError.new("Unexpected character at index #{@index}")
      end
    end

    # private

    def expect(char)
      raise JSON::JSONError.new("Expected a character, got nil") if char.nil?
      if @json.getbyte(@index) != char.ord
        got = @json.getbyte(@index)
        raise JSON::JSONError.new("Expected '#{char}' at index #{@index}, but got byte #{got.inspect}")
      end
      @index += 1
      @char_index += 1
    end

    def skip_whitespace
      if JSON.use_regexp?
        if md = JSON.ws_pattern.match(@json, @char_index)
          matched = md[0] || ""
          @index += matched.bytesize
          @char_index += matched.length
        end
        return
      end
      len = @json.bytesize
      while @index < len
        b = @json.getbyte(@index)
        break unless b == 0x20 || b == 0x09 || b == 0x0a || b == 0x0d # ' ', "\t", "\n", "\r"
        @index += 1
        @char_index += 1
      end
    end

    def parse_object
      result = {} #: Hash[String, untyped]
      @index += 1  # Skip '{'
      @char_index += 1
      skip_whitespace

      unless @json.getbyte(@index) == 0x7d # '}'
        while true
          key = parse_string
          skip_whitespace
          expect(':')
          skip_whitespace
          value = parse
          result[key] = value
          skip_whitespace
          break if @json.getbyte(@index) == 0x7d
          expect(',')
          skip_whitespace
        end
      end

      @index += 1  # Skip '}'
      @char_index += 1
      result
    end

    def parse_array
      result = [] #: Array[untyped]
      @index += 1  # Skip '['
      @char_index += 1
      skip_whitespace

      unless @json.getbyte(@index) == 0x5d # ']'
        while true
          value = parse
          result << value
          skip_whitespace
          break if @json.getbyte(@index) == 0x5d
          expect(',')
          skip_whitespace
        end
      end

      @index += 1  # Skip ']'
      @char_index += 1
      result
    end

    def parse_string
      @index += 1  # Skip opening quote
      @char_index += 1
      len = @json.bytesize
      start = @index
      parts = [] #: Array[String]
      while true
        if @index >= len
          raise JSON::ParserError.new("Unterminated string")
        end
        if JSON.use_regexp? && @index == start && (md = JSON.string_content_pattern.match(@json, @char_index))
          matched = md[0] || ""
          parts << matched
          @index += matched.bytesize
          @char_index += matched.length
          start = @index
          next
        end
        b = @json.getbyte(@index)
        if b == 0x22 # '"'
          if @index > start
            seg = @json.byteslice(start, @index - start) or raise JSON::ParserError.new("Invalid string range")
            parts << seg
            @char_index += seg.length
          end
          @index += 1
          @char_index += 1
          break
        elsif b == 0x5c # '\\'
          if @index > start
            seg = @json.byteslice(start, @index - start) or raise JSON::ParserError.new("Invalid string range")
            parts << seg
            @char_index += seg.length
          end
          snip = @json.byteslice(@index, 2) or raise JSON::ParserError.new("Invalid string range")
          parts << snip
          @index += 2
          @char_index += 2
          start = @index
        else
          @index += 1
        end
      end
      replace_escape_sequence(parts.join)
    end

    def replace_escape_sequence(str)
      len = str.bytesize
      parts = [] #: Array[String]
      start = 0
      i = 0
      while i < len
        b = str.getbyte(i)
        if b == 0x5c # '\\'
          if i > start
            parts << (str.byteslice(start, i - start) or raise JSON::ParserError.new("Invalid string range"))
          end
          i += 1
          esc = str.getbyte(i)
          case esc
          when 0x22 # '"'
            parts << '"'
          when 0x5c # '\\'
            parts << '\\'
          when 0x2f # '/'
            parts << '/'
          when 0x62 # 'b'
            parts << "\b"
          when 0x66 # 'f'
            parts << "\f"
          when 0x6e # 'n'
            parts << "\n"
          when 0x72 # 'r'
            parts << "\r"
          when 0x74 # 't'
            parts << "\t"
          when 0x75 # 'u'
            hex = str.byteslice(i + 1, 4)
            if hex && hex.bytesize == 4
              code = 0
              j = 0
              while j < 4
                h = hex.getbyte(j) or raise JSON::ParserError.new("Invalid hex in unicode escape")
                code <<= 4
                if h >= 0x30 && h <= 0x39
                  code += h - 0x30
                elsif h >= 0x61 && h <= 0x66
                  code += h - 0x61 + 10
                elsif h >= 0x41 && h <= 0x46
                  code += h - 0x41 + 10
                else
                  raise JSON::ParserError.new("Invalid hex in unicode escape")
                end
                j += 1
              end
              parts << [code].pack('C*')
              i += 4
            else
              raise JSON::ParserError.new("Incomplete unicode escape sequence")
            end
          when nil
            raise JSON::ParserError.new("Unterminated escape sequence")
          else
            raise JSON::ParserError.new("Unknown escape sequence byte: #{esc}")
          end
          i += 1
          start = i
        else
          i += 1
        end
      end
      if len > start
        parts << (str.byteslice(start, len - start) or raise JSON::ParserError.new("Invalid string range"))
      end
      parts.join
    end

    def parse_number
      start = @index

      if JSON.use_regexp?
        md = JSON.number_pattern.match(@json, @char_index)
        unless md
          raise JSON::ParserError.new("Invalid number at index #{@index}")
        end
        matched = md[0] || ""
        is_float = matched.include?('.') || matched.include?('e') || matched.include?('E')
        @index += matched.bytesize
        @char_index += matched.length
        if is_float
          parse_float(start, @index)
        else
          parse_integer(start, @index)
        end
        return
      end

      len = @json.bytesize
      is_float = false
      if @json.getbyte(@index) == 0x2d # '-'
        @index += 1
        @char_index += 1
      end

      while @index < len && digit_byte?(@json.getbyte(@index))
        @index += 1
        @char_index += 1
      end

      if @index < len && @json.getbyte(@index) == 0x2e # '.'
        is_float = true
        @index += 1
        @char_index += 1
        while @index < len && digit_byte?(@json.getbyte(@index))
          @index += 1
          @char_index += 1
        end
      end

      if @index < len && (@json.getbyte(@index) == 0x65 || @json.getbyte(@index) == 0x45) # 'e', 'E'
        is_float = true
        @index += 1
        @char_index += 1
        if @index < len && (@json.getbyte(@index) == 0x2b || @json.getbyte(@index) == 0x2d) # '+', '-'
          @index += 1
          @char_index += 1
        end
        while @index < len && digit_byte?(@json.getbyte(@index))
          @index += 1
          @char_index += 1
        end
      end

      if is_float
        parse_float(start, @index)
      else
        parse_integer(start, @index)
      end
    end

    def digit_byte?(b)
      !!(b && b >= 0x30 && b <= 0x39) # '0'..'9'
    end

    def parse_integer(start, end_index)
      result = 0
      is_negative = @json.getbyte(start) == 0x2d # '-'
      start += 1 if is_negative

      i = start
      while i < end_index
        byte = @json.getbyte(i) or raise JSON::ParserError.new("Invalid number format")
        result = result * 10 + (byte - 0x30) # '0'
        i += 1
      end

      is_negative ? -result : result
    end

    def parse_float(start, end_index)
      result = 0.0
      decimal_divider = 1.0
      exponent = 0
      is_negative = @json.getbyte(start) == 0x2d # '-'
      exponent_negative = false
      parsing_exponent = false
      start += 1 if is_negative

      i = start
      while i < end_index
        b = @json.getbyte(i)
        if b && b >= 0x30 && b <= 0x39 # '0'..'9'
          d = b - 0x30 # '0'
          if parsing_exponent
            exponent = exponent * 10 + d
          elsif decimal_divider == 1.0
            result = result * 10 + d
          else
            result += d / decimal_divider
            decimal_divider *= 10
          end
        elsif b == 0x2e # '.'
          decimal_divider = 10.0
        elsif b == 0x65 || b == 0x45 # 'e', 'E'
          parsing_exponent = true
        elsif b == 0x2d # '-'
          exponent_negative = true if parsing_exponent
        end
        i += 1
      end

      result *= (10.0 ** (exponent_negative ? -exponent : exponent))
      is_negative ? -result : result
    end

    def is_digit?(char)
      return false unless char
      '0' <= char && char <= '9'
    end

  end
end

