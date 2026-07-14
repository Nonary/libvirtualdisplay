#include <gtest/gtest.h>
#include "virtual_display/driver/edid.h"

#include <cmath>

namespace vdd = virtual_display::driver;

namespace {

  vdd::EdidOptions default_options() {
    vdd::EdidOptions options {};
    options.manufacturer_id = {'S', 'D', 'D'};
    options.product_code = 0x4101;
    options.serial_number = 0x12345678;
    options.width = 2560;
    options.height = 1440;
    options.physical_width_mm = 590;
    options.physical_height_mm = 330;
    options.refresh_rate_millihz = 120'000;
    options.monitor_name = "Sunshine HDR";
    options.hdr_supported = true;
    return options;
  }

  std::uint16_t read_detailed_physical_width_mm(const std::array<std::byte, vdd::kEdidSize> &edid) {
    return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(edid[66]) |
      ((static_cast<std::uint16_t>(edid[68]) >> 4u) << 8u)
    );
  }

  std::uint16_t read_detailed_physical_height_mm(const std::array<std::byte, vdd::kEdidSize> &edid) {
    return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(edid[67]) |
      ((static_cast<std::uint16_t>(edid[68]) & 0x0fu) << 8u)
    );
  }

  std::uint32_t byte_value(const std::byte value) {
    return static_cast<std::uint8_t>(value);
  }

  std::uint64_t pixel_clock_hz(const vdd::PreferredTiming &timing) {
    return static_cast<std::uint64_t>(timing.pixel_clock_10khz) * 10'000ull;
  }

  std::uint32_t horizontal_scan_khz(const vdd::PreferredTiming &timing) {
    const auto horizontal_total =
      static_cast<std::uint64_t>(timing.horizontal_active) + timing.horizontal_blanking;
    return static_cast<std::uint32_t>(
      (pixel_clock_hz(timing) + horizontal_total * 1000ull - 1ull) /
      (horizontal_total * 1000ull)
    );
  }

  std::uint32_t vertical_rate_hz(const vdd::PreferredTiming &timing) {
    const auto total_pixels =
      (static_cast<std::uint64_t>(timing.horizontal_active) + timing.horizontal_blanking) *
      (static_cast<std::uint64_t>(timing.vertical_active) + timing.vertical_blanking);
    return static_cast<std::uint32_t>((pixel_clock_hz(timing) + total_pixels - 1ull) / total_pixels);
  }

  double decode_hdr_static_metadata_luminance_nits(const std::byte value) {
    return 50.0 * std::pow(2.0, static_cast<std::uint8_t>(value) / 32.0);
  }
}  // namespace

TEST(VirtualDisplayDriverEdid, BuildsParseableTwoBlockEdidWithStableIdentity) {
  const auto edid = vdd::create_edid(default_options());

  EXPECT_EQ(edid[0], std::byte {0x00});
  EXPECT_EQ(edid[1], std::byte {0xff});
  EXPECT_EQ(edid[126], std::byte {0x01});
  EXPECT_EQ(edid[128], std::byte {0x02});
  EXPECT_TRUE(vdd::has_valid_edid_checksums(edid));

  EXPECT_EQ(vdd::read_manufacturer_id(edid), (std::array<char, 3> {'S', 'D', 'D'}));
  EXPECT_EQ(vdd::read_product_code(edid), 0x4101u);
  EXPECT_EQ(vdd::read_serial_number(edid), 0x12345678u);
}

TEST(VirtualDisplayDriverEdid, RejectsInvalidManufacturerCodes) {
  auto edid = vdd::create_edid(default_options());

  edid[8] = std::byte {0x00};
  edid[9] = std::byte {0x00};
  EXPECT_EQ(vdd::read_manufacturer_id(edid), std::nullopt);

  edid = vdd::create_edid(default_options());
  edid[8] = std::byte {0x7f};
  edid[9] = std::byte {0xff};
  EXPECT_EQ(vdd::read_manufacturer_id(edid), std::nullopt);
}

TEST(VirtualDisplayDriverEdid, StopsMonitorNameAtDescriptorTerminator) {
  auto edid = vdd::create_edid(default_options());
  edid[90 + 5 + 4] = std::byte {'\n'};
  edid[90 + 5 + 5] = std::byte {'X'};

  EXPECT_EQ(vdd::read_monitor_name(edid), "Suns");
}

TEST(VirtualDisplayDriverEdid, RejectsUnsafeMonitorNameCharacters) {
  auto edid = vdd::create_edid(default_options());
  edid[90 + 5 + 4] = std::byte {'\t'};

  EXPECT_EQ(vdd::read_monitor_name(edid), std::nullopt);
}

TEST(VirtualDisplayDriverEdid, EncodesPhysicalSizeForDpiScaling) {
  auto options = default_options();
  options.physical_width_mm = 700;
  options.physical_height_mm = 390;

  const auto edid = vdd::create_edid(options);

  EXPECT_EQ(static_cast<std::uint8_t>(edid[21]), 70u);
  EXPECT_EQ(static_cast<std::uint8_t>(edid[22]), 39u);
  EXPECT_EQ(read_detailed_physical_width_mm(edid), 700u);
  EXPECT_EQ(read_detailed_physical_height_mm(edid), 390u);
}

TEST(VirtualDisplayDriverEdid, HdrEdidUsesRequestedPreferredTiming) {
  const auto edid = vdd::create_edid(default_options());
  const auto timing = vdd::read_preferred_timing(edid);

  EXPECT_EQ(timing.horizontal_active, 2560u);
  EXPECT_EQ(timing.vertical_active, 1440u);
  EXPECT_GT(timing.horizontal_blanking, 0u);
  EXPECT_GT(timing.vertical_blanking, 0u);
  EXPECT_GT(timing.pixel_clock_10khz, 0u);
}

TEST(VirtualDisplayDriverEdid, HdrEdidDoesNotEncodeRequestedHighRefreshInBaseTiming) {
  auto options = default_options();
  options.width = 3840;
  options.height = 2160;
  options.refresh_rate_millihz = 240'000;

  const auto edid = vdd::create_edid(options);
  const auto timing = vdd::read_preferred_timing(edid);

  EXPECT_EQ(timing.horizontal_active, 3840u);
  EXPECT_EQ(timing.vertical_active, 2160u);
  EXPECT_NE(timing.pixel_clock_10khz, 0xffffu);

  const auto horizontal_total =
    static_cast<std::uint64_t>(timing.horizontal_active) + static_cast<std::uint64_t>(timing.horizontal_blanking);
  const auto vertical_total =
    static_cast<std::uint64_t>(timing.vertical_active) + static_cast<std::uint64_t>(timing.vertical_blanking);
  ASSERT_GT(horizontal_total, 0u);
  ASSERT_GT(vertical_total, 0u);

  const auto total_pixels = horizontal_total * vertical_total;
  const auto effective_millihz = static_cast<std::uint64_t>(timing.pixel_clock_10khz) * 10'000'000ull / total_pixels;

  EXPECT_NEAR(static_cast<double>(effective_millihz), 60'000.0, 10.0);
}

TEST(VirtualDisplayDriverEdid, UltrawideModesUseSafeBaseDetailedTiming) {
  auto options = default_options();
  options.width = 5120;
  options.height = 1440;
  options.refresh_rate_millihz = 240'000;

  const auto edid = vdd::create_edid(options);
  const auto timing = vdd::read_preferred_timing(edid);

  EXPECT_TRUE(vdd::has_valid_edid_checksums(edid));
  EXPECT_EQ(timing.horizontal_active, 4095u);
  EXPECT_EQ(timing.vertical_active, 1440u);
  EXPECT_NE(timing.pixel_clock_10khz, 0xffffu);
}

TEST(VirtualDisplayDriverEdid, RangeDescriptorIncludesEmittedPreferredTiming) {
  const auto edid = vdd::create_edid(default_options());
  const auto timing = vdd::read_preferred_timing(edid);
  const auto preferred_vertical_hz = vertical_rate_hz(timing);
  const auto preferred_horizontal_khz = horizontal_scan_khz(timing);

  EXPECT_EQ(edid[108 + 3], std::byte {0xfd});
  EXPECT_LE(byte_value(edid[108 + 5]), preferred_vertical_hz);
  EXPECT_GE(byte_value(edid[108 + 6]), preferred_vertical_hz);
  EXPECT_GE(byte_value(edid[108 + 8]), preferred_horizontal_khz);
  EXPECT_GT(byte_value(edid[108 + 8]), 160u);
  EXPECT_GE(byte_value(edid[108 + 9]) * 10'000'000ull, pixel_clock_hz(timing));
}

TEST(VirtualDisplayDriverEdid, RangeDescriptorTracksFallbackPreferredTiming) {
  auto options = default_options();
  options.width = 3840;
  options.height = 2160;
  options.refresh_rate_millihz = 240'000;

  const auto edid = vdd::create_edid(options);
  const auto timing = vdd::read_preferred_timing(edid);

  EXPECT_EQ(edid[108 + 3], std::byte {0xfd});
  EXPECT_EQ(byte_value(edid[108 + 6]), vertical_rate_hz(timing));
  EXPECT_EQ(byte_value(edid[108 + 8]), horizontal_scan_khz(timing));
  EXPECT_EQ(byte_value(edid[108 + 9]) * 10'000'000ull, 630'000'000ull);
}

TEST(VirtualDisplayDriverEdid, RangeDescriptorAllowsLowRefreshPreferredTiming) {
  auto options = default_options();
  options.refresh_rate_millihz = 24'000;

  const auto edid = vdd::create_edid(options);
  const auto timing = vdd::read_preferred_timing(edid);

  EXPECT_EQ(vertical_rate_hz(timing), 24u);
  EXPECT_EQ(byte_value(edid[108 + 5]), 24u);
  EXPECT_GE(byte_value(edid[108 + 6]), 60u);
}

TEST(VirtualDisplayDriverEdid, ClampsTimingInputsBeforeEncoding) {
  auto options = default_options();
  options.width = 100'000;
  options.height = 100'000;
  options.physical_width_mm = 100'000;
  options.physical_height_mm = 100'000;
  options.refresh_rate_millihz = 10'000'000;

  const auto edid = vdd::create_edid(options);
  const auto timing = vdd::read_preferred_timing(edid);

  EXPECT_EQ(timing.horizontal_active, 4095u);
  EXPECT_EQ(timing.vertical_active, 4095u);
  EXPECT_NE(timing.pixel_clock_10khz, 0xffffu);
  EXPECT_EQ(read_detailed_physical_width_mm(edid), 4095u);
  EXPECT_EQ(read_detailed_physical_height_mm(edid), 4095u);
  EXPECT_TRUE(vdd::has_valid_edid_checksums(edid));
}

TEST(VirtualDisplayDriverEdid, HandlesEmptyMonitorName) {
  auto options = default_options();
  options.monitor_name = std::string_view {};

  const auto edid = vdd::create_edid(options);

  EXPECT_TRUE(vdd::has_valid_edid_checksums(edid));
}

TEST(VirtualDisplayDriverEdid, EmbedsHdrStaticMetadataWhenRequested) {
  const auto edid = vdd::create_edid(default_options());

  EXPECT_TRUE(vdd::has_hdr_static_metadata(edid));
}

TEST(VirtualDisplayDriverEdid, RejectsHdrStaticMetadataWithoutDescriptorType) {
  auto edid = vdd::create_edid(default_options());
  constexpr auto cta = vdd::kEdidBlockSize;
  edid[cta + 64] = std::byte {0x00};

  EXPECT_FALSE(vdd::has_hdr_static_metadata(edid));
}

TEST(VirtualDisplayDriverEdid, RejectsHdrStaticMetadataWithoutPqEotf) {
  auto edid = vdd::create_edid(default_options());
  constexpr auto cta = vdd::kEdidBlockSize;
  edid[cta + 63] = std::byte {0x00};

  EXPECT_FALSE(vdd::has_hdr_static_metadata(edid));
}

TEST(VirtualDisplayDriverEdid, EmbedsBt2020ColorimetryWhenHdrRequested) {
  const auto edid = vdd::create_edid(default_options());

  EXPECT_TRUE(vdd::has_bt2020_colorimetry(edid));
}

TEST(VirtualDisplayDriverEdid, EmitsWindowsHdrClassifiedCtaMetadata) {
  const auto edid = vdd::create_edid(default_options());
  const auto cta = vdd::kEdidBlockSize;

  // One labeled structural pin: the CTA-861 extension header Windows reads to find and
  // classify the data block collection -- DTD offset (0x44) and the descriptor flags (0xf0).
  EXPECT_EQ(edid[cta + 2], std::byte {0x44});
  EXPECT_EQ(edid[cta + 3], std::byte {0xf0});

  // Decode the HDR Static Metadata Data Block by meaning instead of pinning its raw bytes:
  // PQ EOTF + ST2086 metadata are the prerequisites Windows checks to classify the EDID as
  // HDR-capable, and the maximum luminance must clear the 1000-nit floor it expects.
  const auto hdr = vdd::read_hdr_static_metadata(edid);
  ASSERT_TRUE(hdr.has_value());
  EXPECT_TRUE((hdr->supported_eotfs & vdd::kHdrEotfSmpte2084) != 0);
  EXPECT_TRUE((hdr->supported_static_metadata_types & vdd::kHdrStaticMetadataType1) != 0);
  EXPECT_GE(vdd::hdr_luminance_nits_from_code(hdr->max_luminance_code), 1000.0);
  EXPECT_EQ(hdr->max_frame_average_luminance_code, hdr->max_luminance_code);
  EXPECT_EQ(hdr->min_luminance_code, 0u);

  // Decode the Colorimetry Data Block: BT.2020 RGB + YCC must be advertised for HDR.
  const auto colorimetry = vdd::read_supported_colorimetry(edid);
  ASSERT_TRUE(colorimetry.has_value());
  EXPECT_TRUE(colorimetry->bt2020_rgb);
  EXPECT_TRUE(colorimetry->bt2020_ycc);
}

TEST(VirtualDisplayDriverEdid, DefaultsHdrStaticMetadataToAtLeastThousandNitMaximum) {
  const auto edid = vdd::create_edid(default_options());
  const auto cta = vdd::kEdidBlockSize;

  EXPECT_GE(decode_hdr_static_metadata_luminance_nits(edid[cta + 65]), 1000.0);
  EXPECT_NEAR(decode_hdr_static_metadata_luminance_nits(edid[cta + 65]), 1015.24, 0.01);
  EXPECT_EQ(edid[cta + 66], edid[cta + 65]);
}

TEST(VirtualDisplayDriverEdid, EncodesRequestedHdrMaximumLuminance) {
  auto options = default_options();
  options.hdr_max_luminance_nits = 2000;

  const auto edid = vdd::create_edid(options);
  const auto hdr = vdd::read_hdr_static_metadata(edid);

  ASSERT_TRUE(hdr.has_value());
  EXPECT_TRUE(vdd::has_valid_edid_checksums(edid));
  EXPECT_GE(vdd::hdr_luminance_nits_from_code(hdr->max_luminance_code), 2000.0);
  EXPECT_LT(vdd::hdr_luminance_nits_from_code(hdr->max_luminance_code), 2050.0);
  EXPECT_EQ(hdr->max_frame_average_luminance_code, hdr->max_luminance_code);
}

TEST(VirtualDisplayDriverEdid, OmitsHdrStaticMetadataWhenDisabled) {
  auto options = default_options();
  options.hdr_supported = false;

  const auto edid = vdd::create_edid(options);

  EXPECT_TRUE(vdd::has_valid_edid_checksums(edid));
  EXPECT_FALSE(vdd::has_hdr_static_metadata(edid));
}

TEST(VirtualDisplayDriverEdid, OmitsBt2020ColorimetryWhenHdrDisabled) {
  auto options = default_options();
  options.hdr_supported = false;

  const auto edid = vdd::create_edid(options);

  EXPECT_TRUE(vdd::has_valid_edid_checksums(edid));
  EXPECT_FALSE(vdd::has_bt2020_colorimetry(edid));
}
