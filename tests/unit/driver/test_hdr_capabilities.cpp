#include <gtest/gtest.h>
#include "virtual_display/driver/hdr_capabilities.h"

namespace vdd = virtual_display::driver;

TEST(VirtualDisplayDriverHdrCapabilities, AdvertisesWindowsHdrPrerequisites) {
  const auto capabilities = vdd::hdr_output_capabilities();

  // Assert the observable contract -- the default advertisement satisfies Windows' HDR-toggle
  // prerequisites -- instead of mirroring each struct field. The per-field necessity of that
  // predicate is exercised by RequiresFp16HighColorAndTenBitPathForHdrToggle below.
  EXPECT_TRUE(vdd::supports_windows_hdr_toggle(capabilities));

  // The one prerequisite the toggle predicate does not cover: the driver must advertise no
  // endpoint gamma transform so Windows owns the HDR transfer function.
  EXPECT_FALSE(capabilities.endpoint_gamma_transform);
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
