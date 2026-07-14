#include "virtual_display/driver/edid.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace virtual_display::driver {
  namespace {
    constexpr std::uint32_t kMinEdidHdrMaxLuminanceNits = 400;
    constexpr std::uint32_t kMaxEdidHdrMaxLuminanceNits = 2000;

    std::byte byte(const std::uint32_t value) {
      return static_cast<std::byte>(value & 0xffu);
    }

    std::uint8_t to_u8(const std::byte value) {
      return static_cast<std::uint8_t>(value);
    }

    std::byte hdr_max_luminance_cta_value(const std::uint32_t nits) {
      const auto safe_nits = std::max(nits, 1u);
      const auto code = std::ceil(32.0 * std::log2(static_cast<double>(safe_nits) / 50.0));
      return byte(static_cast<std::uint32_t>(std::clamp(code, 0.0, 255.0)));
    }

    void put_le16(std::span<std::byte> data, const std::size_t offset, const std::uint16_t value) {
      data[offset] = byte(value);
      data[offset + 1] = byte(value >> 8);
    }

    void put_le32(std::span<std::byte> data, const std::size_t offset, const std::uint32_t value) {
      data[offset] = byte(value);
      data[offset + 1] = byte(value >> 8);
      data[offset + 2] = byte(value >> 16);
      data[offset + 3] = byte(value >> 24);
    }

    std::uint16_t encode_manufacturer_id(const std::array<char, 3> &manufacturer_id) {
      auto encode_char = [](const char ch) -> std::uint16_t {
        if (ch < 'A' || ch > 'Z') {
          return 1;
        }

        return static_cast<std::uint16_t>(ch - 'A' + 1);
      };

      return static_cast<std::uint16_t>(
        (encode_char(manufacturer_id[0]) << 10) |
        (encode_char(manufacturer_id[1]) << 5) |
        encode_char(manufacturer_id[2])
      );
    }

    std::uint16_t align8(const std::uint32_t value) {
      return static_cast<std::uint16_t>((value + 7u) & ~7u);
    }

    std::uint8_t physical_size_cm(const std::uint32_t millimeters) {
      return static_cast<std::uint8_t>(std::clamp((millimeters + 5u) / 10u, 1u, 255u));
    }

    std::uint16_t physical_size_mm(const std::uint32_t millimeters) {
      return static_cast<std::uint16_t>(std::clamp(millimeters, 1u, 4095u));
    }

    std::uint64_t preferred_total_pixels(const EdidOptions &options) {
      const auto horizontal_blanking = align8(std::clamp(options.width / 5u, 160u, 2047u));
      const auto vertical_blanking = static_cast<std::uint16_t>(std::clamp(options.height / 20u, 45u, 1023u));
      return
        static_cast<std::uint64_t>(options.width + horizontal_blanking) *
        static_cast<std::uint64_t>(options.height + vertical_blanking);
    }

    std::uint64_t preferred_pixel_clock_hz(const EdidOptions &options) {
      const auto total_pixels = preferred_total_pixels(options);
      return total_pixels * static_cast<std::uint64_t>(std::max(options.refresh_rate_millihz, 1u)) / 1000u;
    }

    constexpr auto kMaxBaseDetailedTimingPixelClockHz = 0xffffull * 10'000ull;
    constexpr std::uint32_t kMaxRangeDescriptorVerticalHz = 255;

    PreferredTiming make_preferred_timing(const EdidOptions &options) {
      const auto horizontal_blanking = align8(std::clamp(options.width / 5u, 160u, 2047u));
      const auto vertical_blanking = static_cast<std::uint16_t>(std::clamp(options.height / 20u, 45u, 1023u));
      const auto pixel_clock_hz = preferred_pixel_clock_hz(options);

      return PreferredTiming {
        static_cast<std::uint32_t>(std::clamp<std::uint64_t>(pixel_clock_hz / 10'000u, 1u, 0xffffu)),
        static_cast<std::uint16_t>(options.width),
        horizontal_blanking,
        static_cast<std::uint16_t>(options.height),
        vertical_blanking
      };
    }

    EdidOptions detailed_timing_options(const EdidOptions &options) {
      if (preferred_pixel_clock_hz(options) <= kMaxBaseDetailedTimingPixelClockHz &&
          options.refresh_rate_millihz <= kMaxRangeDescriptorVerticalHz * 1000u) {
        return options;
      }

      auto safe_options = options;
      safe_options.refresh_rate_millihz = std::min<std::uint32_t>(
        safe_options.refresh_rate_millihz,
        60'000
      );
      if (preferred_pixel_clock_hz(safe_options) <= kMaxBaseDetailedTimingPixelClockHz) {
        return safe_options;
      }

      const auto total_pixels = preferred_total_pixels(safe_options);
      safe_options.refresh_rate_millihz = static_cast<std::uint32_t>(
        std::max<std::uint64_t>(1, kMaxBaseDetailedTimingPixelClockHz * 1000ull / total_pixels)
      );
      return safe_options;
    }

    EdidOptions sanitized_options(const EdidOptions &options) {
      auto safe = options;
      safe.width = std::clamp(safe.width, 1u, 4095u);
      safe.height = std::clamp(safe.height, 1u, 4095u);
      safe.refresh_rate_millihz = std::clamp(
        safe.refresh_rate_millihz,
        1u,
        (std::numeric_limits<std::uint32_t>::max)()
      );
      safe.physical_width_mm = std::clamp(safe.physical_width_mm, 1u, 4095u);
      safe.physical_height_mm = std::clamp(safe.physical_height_mm, 1u, 4095u);
      safe.hdr_max_luminance_nits = std::clamp(
        safe.hdr_max_luminance_nits,
        kMinEdidHdrMaxLuminanceNits,
        kMaxEdidHdrMaxLuminanceNits
      );
      return safe;
    }

    void write_detailed_timing(std::span<std::byte> data, const std::size_t offset, const EdidOptions &options) {
      const auto timing = make_preferred_timing(detailed_timing_options(options));
      const auto h_sync_offset = std::clamp<std::uint16_t>(timing.horizontal_blanking / 3u, 8u, 255u);
      const auto h_sync_width = std::clamp<std::uint16_t>(timing.horizontal_blanking / 5u, 8u, 255u);
      const auto v_sync_offset = std::uint16_t {3};
      const auto v_sync_width = std::uint16_t {5};

      put_le16(data, offset, static_cast<std::uint16_t>(timing.pixel_clock_10khz));
      data[offset + 2] = byte(timing.horizontal_active);
      data[offset + 3] = byte(timing.horizontal_blanking);
      data[offset + 4] = byte(((timing.horizontal_active >> 8) << 4) | (timing.horizontal_blanking >> 8));
      data[offset + 5] = byte(timing.vertical_active);
      data[offset + 6] = byte(timing.vertical_blanking);
      data[offset + 7] = byte(((timing.vertical_active >> 8) << 4) | (timing.vertical_blanking >> 8));
      data[offset + 8] = byte(h_sync_offset);
      data[offset + 9] = byte(h_sync_width);
      data[offset + 10] = byte((v_sync_offset << 4) | v_sync_width);
      data[offset + 11] = byte(((h_sync_offset >> 8) << 6) | ((h_sync_width >> 8) << 4));
      const auto physical_width = physical_size_mm(options.physical_width_mm);
      const auto physical_height = physical_size_mm(options.physical_height_mm);
      data[offset + 12] = byte(physical_width);
      data[offset + 13] = byte(physical_height);
      data[offset + 14] = byte(((physical_width >> 8) << 4) | (physical_height >> 8));
      data[offset + 15] = byte(0);
      data[offset + 16] = byte(0);
      data[offset + 17] = byte(0x1a);
    }

    std::string sanitized_edid_text(const std::string_view text, const std::size_t max_size) {
      std::string sanitized;
      sanitized.reserve(std::min(text.size(), max_size));
      for (const unsigned char ch : text) {
        if (sanitized.size() == max_size) {
          break;
        }
        sanitized.push_back(ch >= 0x20 && ch <= 0x7e ? static_cast<char>(ch) : ' ');
      }
      while (!sanitized.empty() && sanitized.back() == ' ') {
        sanitized.pop_back();
      }
      return sanitized;
    }

    void write_text_descriptor(
      std::span<std::byte> data,
      const std::size_t offset,
      const std::byte descriptor_type,
      const std::string_view text
    ) {
      data[offset + 0] = std::byte {0x00};
      data[offset + 1] = std::byte {0x00};
      data[offset + 2] = std::byte {0x00};
      data[offset + 3] = descriptor_type;
      data[offset + 4] = std::byte {0x00};

      std::fill_n(data.begin() + static_cast<std::ptrdiff_t>(offset + 5), 13, std::byte {' '});
      const auto safe_text = sanitized_edid_text(text, 12);
      const auto copy_size = safe_text.size();
      if (copy_size != 0) {
        std::memcpy(data.data() + offset + 5, safe_text.data(), copy_size);
      }
      data[offset + 5 + copy_size] = std::byte {'\n'};
    }

    void write_edid_string_field(
      std::span<std::byte> data,
      const std::size_t offset,
      const std::string_view text
    ) {
      constexpr std::size_t kFieldSize = 13;
      std::fill_n(data.begin() + static_cast<std::ptrdiff_t>(offset), kFieldSize, std::byte {' '});
      const auto safe_text = sanitized_edid_text(text, kFieldSize - 1);
      const auto copy_size = safe_text.size();
      if (copy_size != 0) {
        std::memcpy(data.data() + offset, safe_text.data(), copy_size);
      }
      data[offset + copy_size] = std::byte {'\n'};
    }

    void write_range_descriptor(std::span<std::byte> data, const std::size_t offset, const EdidOptions &options) {
      const auto timing = make_preferred_timing(detailed_timing_options(options));
      const auto horizontal_total =
        static_cast<std::uint32_t>(timing.horizontal_active) +
        static_cast<std::uint32_t>(timing.horizontal_blanking);
      const auto vertical_total =
        static_cast<std::uint32_t>(timing.vertical_active) +
        static_cast<std::uint32_t>(timing.vertical_blanking);
      const auto pixel_hz = static_cast<std::uint64_t>(timing.pixel_clock_10khz) * 10'000ull;
      const auto horizontal_khz = static_cast<std::uint32_t>(
        (pixel_hz + static_cast<std::uint64_t>(horizontal_total) * 1000ull - 1ull) /
        (static_cast<std::uint64_t>(horizontal_total) * 1000ull)
      );
      const auto vertical_hz = static_cast<std::uint32_t>(
        (pixel_hz + static_cast<std::uint64_t>(horizontal_total) * vertical_total - 1ull) /
        (static_cast<std::uint64_t>(horizontal_total) * vertical_total)
      );

      data[offset + 0] = std::byte {0x00};
      data[offset + 1] = std::byte {0x00};
      data[offset + 2] = std::byte {0x00};
      data[offset + 3] = std::byte {0xfd};
      data[offset + 4] = std::byte {0x00};
      data[offset + 5] = byte(std::clamp<std::uint32_t>(std::min(30u, vertical_hz), 1u, 255u));
      data[offset + 6] = byte(std::clamp<std::uint32_t>(std::max(60u, vertical_hz), 1u, 255u));
      data[offset + 7] = std::byte {30};
      data[offset + 8] = byte(std::clamp<std::uint32_t>(std::max(30u, horizontal_khz), 1u, 255u));
      const auto max_pixel_clock_10mhz = static_cast<std::uint32_t>(
        std::clamp<std::uint64_t>((pixel_hz + 9'999'999ull) / 10'000'000ull, 30ull, 255ull)
      );
      data[offset + 9] = byte(max_pixel_clock_10mhz);
      data[offset + 10] = std::byte {0x20};
    }

    void write_checksum(std::span<std::byte> block) {
      unsigned int sum = 0;
      for (std::size_t index = 0; index < kEdidBlockSize - 1; ++index) {
        sum += to_u8(block[index]);
      }

      block[kEdidBlockSize - 1] = byte((256u - (sum & 0xffu)) & 0xffu);
    }
  }  // namespace

  std::array<std::byte, kEdidSize> create_edid(const EdidOptions &options) {
    const auto safe_options = sanitized_options(options);

    std::array<std::byte, kEdidSize> edid {};
    auto base = std::span<std::byte> {edid.data(), kEdidBlockSize};
    auto extension = std::span<std::byte> {edid.data() + kEdidBlockSize, kEdidBlockSize};

    const std::array<std::byte, 8> header {
      std::byte {0x00}, std::byte {0xff}, std::byte {0xff}, std::byte {0xff},
      std::byte {0xff}, std::byte {0xff}, std::byte {0xff}, std::byte {0x00}
    };
    std::copy(header.begin(), header.end(), base.begin());

    const auto manufacturer = encode_manufacturer_id(safe_options.manufacturer_id);
    base[8] = byte(manufacturer >> 8);
    base[9] = byte(manufacturer);
    put_le16(base, 10, safe_options.product_code);
    put_le32(base, 12, safe_options.serial_number);
    base[16] = std::byte {1};
    base[17] = std::byte {36};
    base[18] = std::byte {1};
    base[19] = std::byte {4};
    base[20] = std::byte {0xa5};
    base[21] = byte(physical_size_cm(safe_options.physical_width_mm));
    base[22] = byte(physical_size_cm(safe_options.physical_height_mm));
    base[23] = std::byte {0x78};
    base[24] = std::byte {0x0a};
    base[25] = std::byte {0xee};
    base[26] = std::byte {0x91};
    base[27] = std::byte {0xa3};
    base[28] = std::byte {0x54};
    base[29] = std::byte {0x4c};
    base[30] = std::byte {0x99};
    base[31] = std::byte {0x26};
    base[32] = std::byte {0x0f};
    base[33] = std::byte {0x50};
    base[34] = std::byte {0x54};

    std::fill(base.begin() + 38, base.begin() + 54, std::byte {0x01});
    write_detailed_timing(base, 54, safe_options);
    write_text_descriptor(base, 72, std::byte {0xff}, std::to_string(safe_options.serial_number));
    write_text_descriptor(base, 90, std::byte {0xfc}, safe_options.monitor_name);
    write_range_descriptor(base, 108, safe_options);
    base[126] = std::byte {1};
    write_checksum(base);

    extension[0] = std::byte {0x02};
    extension[1] = std::byte {0x03};

    std::size_t data_offset = 4;
    if (safe_options.hdr_supported) {
      std::array<std::byte, 64> hdr_cta_blocks {
        std::byte {0x51}, std::byte {0x5d}, std::byte {0x5e}, std::byte {0x5f},
        std::byte {0x60}, std::byte {0x61}, std::byte {0x10}, std::byte {0x1f},
        std::byte {0x22}, std::byte {0x21}, std::byte {0x20}, std::byte {0x05},
        std::byte {0x14}, std::byte {0x04}, std::byte {0x13}, std::byte {0x12},
        std::byte {0x03}, std::byte {0x01}, std::byte {0x23}, std::byte {0x0f},
        std::byte {0x56}, std::byte {0x05}, std::byte {0x83}, std::byte {0x0f},
        std::byte {0x08}, std::byte {0x00}, std::byte {0x6d}, std::byte {0x03},
        std::byte {0x0c}, std::byte {0x00}, std::byte {0x10}, std::byte {0x00},
        std::byte {0x38}, std::byte {0x78}, std::byte {0x20}, std::byte {0x00},
        std::byte {0x60}, std::byte {0x01}, std::byte {0x02}, std::byte {0x03},
        std::byte {0x67}, std::byte {0xd8}, std::byte {0x5d}, std::byte {0xc4},
        std::byte {0x01}, std::byte {0x78}, std::byte {0x80}, std::byte {0x03},
        std::byte {0xe3}, std::byte {0x05}, std::byte {0xe0}, std::byte {0x01},
        std::byte {0xe4}, std::byte {0x0f}, std::byte {0x18}, std::byte {0x00},
        std::byte {0x00}, std::byte {0xe6}, std::byte {0x06}, std::byte {0x0f},
        std::byte {0x01}, std::byte {0x00}, std::byte {0x00}, std::byte {0x00}
      };
      const auto hdr_max_luminance = hdr_max_luminance_cta_value(safe_options.hdr_max_luminance_nits);
      hdr_cta_blocks[61] = hdr_max_luminance;
      hdr_cta_blocks[62] = hdr_max_luminance;
      // Windows HDR classification expects the complete CTA metadata block set.
      // Keep that block stable while the base EDID remains identity-specific.
      std::copy(hdr_cta_blocks.begin(), hdr_cta_blocks.end(), extension.begin() + 4);
      data_offset += hdr_cta_blocks.size();
      extension[3] = std::byte {0xf0};
    }

    extension[2] = byte(data_offset);
    write_checksum(extension);

    return edid;
  }

  bool has_valid_edid_checksums(const std::span<const std::byte, kEdidSize> edid) {
    for (std::size_t block_index = 0; block_index < 2; ++block_index) {
      unsigned int sum = 0;
      for (std::size_t offset = 0; offset < kEdidBlockSize; ++offset) {
        sum += to_u8(edid[block_index * kEdidBlockSize + offset]);
      }
      if ((sum & 0xffu) != 0) {
        return false;
      }
    }

    return true;
  }

  namespace {
    constexpr std::size_t kCtaBlockNotFound = static_cast<std::size_t>(-1);

    // Offset of the first CTA-861 extended-tag data block (tag 7) matching extended_tag with
    // at least min_length bytes after its header, or kCtaBlockNotFound. Shared by every CTA
    // decoder so the bounds-checked walk lives in one place.
    std::size_t find_cta_data_block(
      const std::span<const std::byte> extension,
      const unsigned extended_tag,
      const std::size_t min_length
    ) {
      if (extension.empty() || extension[0] != std::byte {0x02}) {
        return kCtaBlockNotFound;
      }

      const auto data_end = std::clamp<std::size_t>(to_u8(extension[2]), 4, kEdidBlockSize - 1);
      for (std::size_t offset = 4; offset < data_end;) {
        const auto header = to_u8(extension[offset]);
        const auto tag = header >> 5;
        const std::size_t length = header & 0x1fu;
        if (length == 0 || offset >= data_end || length > data_end - offset - 1) {
          break;
        }

        if (tag == 0x07 && length >= min_length && to_u8(extension[offset + 1]) == extended_tag) {
          return offset;
        }

        offset += length + 1;
      }

      return kCtaBlockNotFound;
    }
  }  // namespace

  std::optional<HdrStaticMetadata> read_hdr_static_metadata(const std::span<const std::byte, kEdidSize> edid) {
    const auto extension = edid.subspan(kEdidBlockSize, kEdidBlockSize);
    const auto offset = find_cta_data_block(extension, 0x06, 3);
    if (offset == kCtaBlockNotFound) {
      return std::nullopt;
    }

    const std::size_t length = to_u8(extension[offset]) & 0x1fu;
    HdrStaticMetadata metadata {};
    metadata.supported_eotfs = to_u8(extension[offset + 2]);
    metadata.supported_static_metadata_types = to_u8(extension[offset + 3]);
    if (length >= 4) {
      metadata.max_luminance_code = to_u8(extension[offset + 4]);
    }
    if (length >= 5) {
      metadata.max_frame_average_luminance_code = to_u8(extension[offset + 5]);
    }
    if (length >= 6) {
      metadata.min_luminance_code = to_u8(extension[offset + 6]);
    }
    return metadata;
  }

  std::optional<SupportedColorimetry> read_supported_colorimetry(const std::span<const std::byte, kEdidSize> edid) {
    const auto extension = edid.subspan(kEdidBlockSize, kEdidBlockSize);
    const auto offset = find_cta_data_block(extension, 0x05, 2);
    if (offset == kCtaBlockNotFound) {
      return std::nullopt;
    }

    const auto colorimetry = to_u8(extension[offset + 2]);
    SupportedColorimetry result {};
    result.bt2020_rgb = (colorimetry & 0x80u) != 0;
    result.bt2020_ycc = (colorimetry & 0x40u) != 0;
    result.bt2020_cycc = (colorimetry & 0x20u) != 0;
    return result;
  }

  double hdr_luminance_nits_from_code(const std::uint8_t code) {
    return 50.0 * std::pow(2.0, static_cast<double>(code) / 32.0);
  }

  bool has_hdr_static_metadata(const std::span<const std::byte, kEdidSize> edid) {
    const auto metadata = read_hdr_static_metadata(edid);
    return metadata.has_value() &&
           (metadata->supported_eotfs & kHdrEotfSmpte2084) != 0 &&
           (metadata->supported_static_metadata_types & kHdrStaticMetadataType1) != 0;
  }

  bool has_bt2020_colorimetry(const std::span<const std::byte, kEdidSize> edid) {
    const auto colorimetry = read_supported_colorimetry(edid);
    return colorimetry.has_value() && colorimetry->bt2020_rgb && colorimetry->bt2020_ycc;
  }

  std::optional<std::array<char, 3>> read_manufacturer_id(const std::span<const std::byte, kEdidSize> edid) {
    const auto encoded = static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(to_u8(edid[8])) << 8) |
      static_cast<std::uint16_t>(to_u8(edid[9]))
    );

    const std::array<unsigned, 3> parts {
      static_cast<unsigned>((encoded >> 10) & 0x1fu),
      static_cast<unsigned>((encoded >> 5) & 0x1fu),
      static_cast<unsigned>(encoded & 0x1fu)
    };

    std::array<char, 3> manufacturer_id {};
    for (std::size_t index = 0; index < parts.size(); ++index) {
      if (parts[index] == 0 || parts[index] > 26) {
        return std::nullopt;
      }
      manufacturer_id[index] = static_cast<char>('A' + parts[index] - 1);
    }

    return manufacturer_id;
  }

  std::uint16_t read_product_code(const std::span<const std::byte, kEdidSize> edid) {
    return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(to_u8(edid[10])) |
      (static_cast<std::uint16_t>(to_u8(edid[11])) << 8)
    );
  }

  std::uint32_t read_serial_number(const std::span<const std::byte, kEdidSize> edid) {
    return static_cast<std::uint32_t>(to_u8(edid[12])) |
           (static_cast<std::uint32_t>(to_u8(edid[13])) << 8) |
           (static_cast<std::uint32_t>(to_u8(edid[14])) << 16) |
           (static_cast<std::uint32_t>(to_u8(edid[15])) << 24);
  }

  std::optional<std::string> read_monitor_name(const std::span<const std::byte, kEdidSize> edid) {
    constexpr std::array<std::size_t, 4> kDescriptorOffsets {54, 72, 90, 108};
    for (const auto offset: kDescriptorOffsets) {
      if (edid[offset] != std::byte {0x00} ||
          edid[offset + 1] != std::byte {0x00} ||
          edid[offset + 2] != std::byte {0x00} ||
          edid[offset + 3] != std::byte {0xfc} ||
          edid[offset + 4] != std::byte {0x00}) {
        continue;
      }

      std::string name;
      name.reserve(13);
      for (std::size_t index = offset + 5; index < offset + 18; ++index) {
        const auto ch = to_u8(edid[index]);
        if (ch == '\0' || ch == '\n' || ch == '\r') {
          break;
        }
        if (ch < 0x20 || ch > 0x7e) {
          return std::nullopt;
        }
        name.push_back(static_cast<char>(ch));
      }
      while (!name.empty() && name.back() == ' ') {
        name.pop_back();
      }
      if (!name.empty()) {
        return name;
      }
    }

    return std::nullopt;
  }

  PreferredTiming read_preferred_timing(const std::span<const std::byte, kEdidSize> edid) {
    constexpr std::size_t offset = 54;

    PreferredTiming timing {};
    timing.pixel_clock_10khz = static_cast<std::uint32_t>(to_u8(edid[offset])) |
                               (static_cast<std::uint32_t>(to_u8(edid[offset + 1])) << 8);
    timing.horizontal_active = static_cast<std::uint16_t>(
      to_u8(edid[offset + 2]) |
      ((to_u8(edid[offset + 4]) & 0xf0u) << 4)
    );
    timing.horizontal_blanking = static_cast<std::uint16_t>(
      to_u8(edid[offset + 3]) |
      ((to_u8(edid[offset + 4]) & 0x0fu) << 8)
    );
    timing.vertical_active = static_cast<std::uint16_t>(
      to_u8(edid[offset + 5]) |
      ((to_u8(edid[offset + 7]) & 0xf0u) << 4)
    );
    timing.vertical_blanking = static_cast<std::uint16_t>(
      to_u8(edid[offset + 6]) |
      ((to_u8(edid[offset + 7]) & 0x0fu) << 8)
    );

    return timing;
  }
}  // namespace virtual_display::driver
