#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace virtual_display::driver {
  inline constexpr std::size_t kEdidBlockSize = 128;
  inline constexpr std::size_t kEdidSize = kEdidBlockSize * 2;

  struct EdidOptions {
    std::array<char, 3> manufacturer_id {'S', 'D', 'D'};
    std::uint16_t product_code {0x1000};
    std::uint32_t serial_number {1};
    std::uint32_t width {1920};
    std::uint32_t height {1080};
    std::uint32_t physical_width_mm {600};
    std::uint32_t physical_height_mm {340};
    std::uint32_t refresh_rate_millihz {60'000};
    std::uint32_t hdr_max_luminance_nits {1000};
    std::string_view monitor_name {"Sunshine Display"};
    bool hdr_supported {true};
  };

  struct PreferredTiming {
    std::uint32_t pixel_clock_10khz {};
    std::uint16_t horizontal_active {};
    std::uint16_t horizontal_blanking {};
    std::uint16_t vertical_active {};
    std::uint16_t vertical_blanking {};
  };

  // CTA-861 HDR Static Metadata Data Block EOTF support flags (byte 3 of the block).
  inline constexpr std::uint8_t kHdrEotfTraditionalSdr = 0x01;
  inline constexpr std::uint8_t kHdrEotfTraditionalHdr = 0x02;
  inline constexpr std::uint8_t kHdrEotfSmpte2084 = 0x04;  // PQ; required for Windows HDR
  inline constexpr std::uint8_t kHdrEotfHlg = 0x08;
  // Supported Static Metadata Descriptor types (byte 4 of the block); type 1 == SMPTE ST 2086.
  inline constexpr std::uint8_t kHdrStaticMetadataType1 = 0x01;

  struct HdrStaticMetadata {
    std::uint8_t supported_eotfs {};
    std::uint8_t supported_static_metadata_types {};
    std::uint8_t max_luminance_code {};
    std::uint8_t max_frame_average_luminance_code {};
    std::uint8_t min_luminance_code {};
  };

  struct SupportedColorimetry {
    bool bt2020_rgb {};
    bool bt2020_ycc {};
    bool bt2020_cycc {};
  };

  std::array<std::byte, kEdidSize> create_edid(const EdidOptions &options);
  bool has_valid_edid_checksums(std::span<const std::byte, kEdidSize> edid);
  bool has_hdr_static_metadata(std::span<const std::byte, kEdidSize> edid);
  bool has_bt2020_colorimetry(std::span<const std::byte, kEdidSize> edid);
  // Decodes the CTA HDR Static Metadata Data Block, or nullopt when absent. Lets callers
  // assert the advertised EOTFs / metadata types / luminance codes by meaning rather than by
  // pinning raw extension bytes.
  std::optional<HdrStaticMetadata> read_hdr_static_metadata(std::span<const std::byte, kEdidSize> edid);
  // Decodes the CTA Colorimetry Data Block's BT.2020 advertisement, or nullopt when absent.
  std::optional<SupportedColorimetry> read_supported_colorimetry(std::span<const std::byte, kEdidSize> edid);
  // CTA-861 maximum/minimum luminance code -> nits (50 * 2^(code/32)).
  double hdr_luminance_nits_from_code(std::uint8_t code);
  std::optional<std::array<char, 3>> read_manufacturer_id(std::span<const std::byte, kEdidSize> edid);
  std::uint16_t read_product_code(std::span<const std::byte, kEdidSize> edid);
  std::uint32_t read_serial_number(std::span<const std::byte, kEdidSize> edid);
  std::optional<std::string> read_monitor_name(std::span<const std::byte, kEdidSize> edid);
  PreferredTiming read_preferred_timing(std::span<const std::byte, kEdidSize> edid);
}  // namespace virtual_display::driver
