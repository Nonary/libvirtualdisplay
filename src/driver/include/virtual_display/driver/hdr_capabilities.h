#pragma once

namespace virtual_display::driver {
  struct WireColorSupport {
    bool rgb_8bpc {};
    bool rgb_10bpc {};
    bool ycbcr444 {};
    bool ycbcr422 {};
    bool ycbcr420 {};
  };

  struct HdrOutputCapabilities {
    bool fp16_swapchain {};
    bool wide_color_space {};
    bool high_color_space {};
    bool endpoint_gamma_transform {};
    WireColorSupport output_bits {};
    WireColorSupport dithering_bits {};
  };

  HdrOutputCapabilities hdr_output_capabilities();
  bool supports_windows_hdr_toggle(const HdrOutputCapabilities &capabilities);
}  // namespace virtual_display::driver
