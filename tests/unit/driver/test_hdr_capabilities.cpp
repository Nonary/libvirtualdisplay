#include <gtest/gtest.h>
#include "virtual_display/driver/hdr_capabilities.h"

namespace vdd = virtual_display::driver;

TEST(VirtualDisplayDriverHdrCapabilities, AdvertisesWindowsHdrPrerequisites) {
  const auto capabilities = vdd::hdr_output_capabilities();

  EXPECT_TRUE(capabilities.fp16_swapchain);
  EXPECT_TRUE(capabilities.wide_color_space);
  EXPECT_TRUE(capabilities.high_color_space);
  EXPECT_FALSE(capabilities.endpoint_gamma_transform);
  EXPECT_TRUE(capabilities.output_bits.rgb_8bpc);
  EXPECT_TRUE(capabilities.output_bits.rgb_10bpc);
  EXPECT_FALSE(capabilities.output_bits.ycbcr444);
  EXPECT_FALSE(capabilities.output_bits.ycbcr422);
  EXPECT_FALSE(capabilities.output_bits.ycbcr420);
  EXPECT_FALSE(capabilities.dithering_bits.rgb_8bpc);
  EXPECT_TRUE(capabilities.dithering_bits.rgb_10bpc);
  EXPECT_TRUE(vdd::supports_windows_hdr_toggle(capabilities));
}

TEST(VirtualDisplayDriverHdrCapabilities, RequiresFp16HighColorAndTenBitPathForHdrToggle) {
  auto capabilities = vdd::hdr_output_capabilities();

  capabilities.fp16_swapchain = false;
  EXPECT_FALSE(vdd::supports_windows_hdr_toggle(capabilities));

  capabilities = vdd::hdr_output_capabilities();
  capabilities.high_color_space = false;
  EXPECT_FALSE(vdd::supports_windows_hdr_toggle(capabilities));

  capabilities = vdd::hdr_output_capabilities();
  capabilities.wide_color_space = false;
  EXPECT_FALSE(vdd::supports_windows_hdr_toggle(capabilities));

  capabilities = vdd::hdr_output_capabilities();
  capabilities.output_bits.rgb_10bpc = false;
  EXPECT_FALSE(vdd::supports_windows_hdr_toggle(capabilities));

  capabilities = vdd::hdr_output_capabilities();
  capabilities.dithering_bits.rgb_10bpc = false;
  EXPECT_FALSE(vdd::supports_windows_hdr_toggle(capabilities));
}
