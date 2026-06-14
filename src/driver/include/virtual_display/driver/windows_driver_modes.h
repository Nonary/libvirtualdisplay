#pragma once

#include "virtual_display/driver/edid.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace virtual_display::driver {
  struct WindowsDriverModeShape {
    std::uint32_t width {1920};
    std::uint32_t height {1080};
    std::uint32_t total_width {2200};
    std::uint32_t total_height {1125};
    std::uint64_t pixel_rate {148'500'000ull};
    std::uint32_t refresh_rate_millihz {60'000};
  };

  struct WindowsDriverFrequencyRational {
    std::uint32_t numerator {};
    std::uint32_t denominator {1};
  };

  struct WindowsDriverSignalFrequencies {
    WindowsDriverFrequencyRational vertical;
    WindowsDriverFrequencyRational horizontal;
  };

  class WindowsDriverRegisteredModeStore {
  public:
    bool register_mode(
      std::span<const std::byte, kEdidSize> edid,
      const WindowsDriverModeShape &mode
    );
    void unregister_mode(std::span<const std::byte, kEdidSize> edid);
    std::optional<WindowsDriverModeShape> registered_mode(
      std::span<const std::byte, kEdidSize> edid
    ) const;

  private:
    struct Entry {
      WindowsDriverModeShape mode;
      std::uint32_t references {};
    };

    std::map<std::array<std::byte, kEdidSize>, Entry> entries_;
  };

  std::uint32_t clamp_windows_driver_u32(std::uint64_t value);
  WindowsDriverModeShape active_windows_driver_mode_shape(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t refresh_rate_millihz
  );
  std::uint32_t append_unique_windows_driver_mode(
    std::vector<WindowsDriverModeShape> &modes,
    const WindowsDriverModeShape &candidate
  );
  std::uint32_t default_windows_driver_preferred_mode_index(const std::vector<WindowsDriverModeShape> &modes);
  std::pair<std::vector<WindowsDriverModeShape>, std::uint32_t> build_windows_driver_mode_shapes(
    const std::optional<WindowsDriverModeShape> &preferred
  );
  std::pair<std::vector<WindowsDriverModeShape>, std::uint32_t> build_windows_driver_target_mode_shapes(
    const std::optional<WindowsDriverModeShape> &monitor_description,
    const WindowsDriverModeShape *requested_shape
  );
  WindowsDriverFrequencyRational make_windows_driver_frequency_rational(
    std::uint64_t numerator,
    std::uint32_t denominator
  );
  WindowsDriverSignalFrequencies windows_driver_signal_frequencies(const WindowsDriverModeShape &shape);
}  // namespace virtual_display::driver
