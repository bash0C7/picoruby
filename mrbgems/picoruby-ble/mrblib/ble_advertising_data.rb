class BLE
  class AdvertisingData
    def initialize
      @data = ""
    end

    attr_reader :data

    def add(type, *values)
      if !type.is_a?(Integer) || type < 0 || type > 0xff
        raise ArgumentError, "AD type must fit 8 bits"
      end
      # Build each field locally so invalid data leaves the previous bytes intact.
      field = [type].pack("C")
      # @data is binary, so count bytes and never index it by character
      length = 1
      vi = 0
      while vi < values.size
        d = values[vi]
        case d
        when String
          data_size = d.bytesize
          if data_size > 0xff - length
            raise ArgumentError, "AD field exceeds 255 bytes"
          end
          field << d
          length += data_size
        when 0
          if length >= 0xff
            raise ArgumentError, "AD field exceeds 255 bytes"
          end
          field << "\x00"
          length += 1
        when Integer
          if d < 0
            raise ArgumentError, "AD integer data must be unsigned"
          end
          while 0 < d
            if length >= 0xff
              raise ArgumentError, "AD field exceeds 255 bytes"
            end
            field << [d & 0xff].pack("C")
            d >>= 8
            length += 1
          end
        when nil
          if length >= 0xff
            raise ArgumentError, "AD field exceeds 255 bytes"
          end
          field << "\x00"
          length += 1
        else
          raise ArgumentError, "invalid data type: `#{d}`"
        end
        vi += 1
      end
      if length > 0xff
        raise ArgumentError, "AD field exceeds 255 bytes"
      end
      @data << [length].pack("C") << field
      length
    end

    def self.build(&block)
      instance = self.new
      block.call(instance)
      adv_data = instance.data
      if 31 < adv_data.bytesize
        raise ArgumentError, "too long AdvData: (#{adv_data.bytesize} bytes). It must be less than 32 bytes."
      end
      adv_data
    end
  end
end
