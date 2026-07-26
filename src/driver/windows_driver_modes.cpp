#include "virtual_display/driver/windows_driver_modes.h"

#include "virtual_display/driver/control_protocol.h"

#include <algorithm>
#include <array>
#include <limits>
#include <numeric>

namespace virtual_display::driver {
  namespace {
    struct ModeSpec {
      std::uint32_t width;
      std::uint32_t height;
      std::uint32_t refresh_rate_millihz;
    };

    constexpr std::array<ModeSpec, 48> kDefaultWindowsDriverModes {{
      {800, 600, 30'000},
      {800, 600, 60'000},
      {800, 600, 72'000},
      {800, 600, 90'000},
      {800, 600, 120'000},
      {800, 600, 144'000},
      {800, 600, 240'000},
      {1280, 720, 30'000},
      {1280, 720, 60'000},
      {1280, 720, 72'000},
      {1280, 720, 90'000},
      {1280, 720, 120'000},
      {1280, 720, 144'000},
      {1366, 768, 30'000},
      {1366, 768, 60'000},
      {1366, 768, 72'000},
      {1366, 768, 90'000},
      {1366, 768, 120'000},
      {1366, 768, 144'000},
      {1366, 768, 240'000},
      {1920, 1080, 30'000},
      {1920, 1080, 60'000},
      {1920, 1080, 72'000},
      {1920, 1080, 90'000},
      {1920, 1080, 120'000},
      {1920, 1080, 144'000},
      {1920, 1080, 240'000},
      {2560, 1440, 30'000},
      {2560, 1440, 60'000},
      {2560, 1440, 72'000},
      {2560, 1440, 90'000},
      {2560, 1440, 120'000},
      {2560, 1440, 144'000},
      {2560, 1440, 240'000},
      {3840, 2160, 30'000},
      {3840, 2160, 60'000},
      {3840, 2160, 72'000},
      {3840, 2160, 90'000},
      {3840, 2160, 120'000},
      {3840, 2160, 144'000},
      {3840, 2160, 240'000},
      {5120, 1440, 30'000},
      {5120, 1440, 60'000},
      {5120, 1440, 120'000},
      {5120, 1440, 144'000},
      {5120, 1440, 175'000},
      {5120, 1440, 240'000},
      {5120, 1440, 480'000}
    }};

    constexpr std::array<std::uint32_t, 5> kPreferredModeScalePercent {{
      100,
      50,
      75,
      125,
      150
    }};

    WindowsDriverModeShape mode_shape_from_spec(const ModeSpec &spec) {
      return active_windows_driver_mode_shape(spec.width, spec.height, spec.refresh_rate_millihz);
    }

    std::optional<WindowsDriverModeShape> scaled_mode_shape(
      const WindowsDriverModeShape &base,
      const std::uint32_t scale_percent,
      const std::uint32_t refresh_rate_millihz
    ) {
      const auto width = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(base.width) * scale_percent / 100ull
      );
      const auto height = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(base.height) * scale_percent / 100ull
      );

      if (width < kMinWidth || width > kMaxWidth ||
          height < kMinHeight || height > kMaxHeight ||
          refresh_rate_millihz < kMinRefreshRateMilliHz) {
        return std::nullopt;
      }

      return active_windows_driver_mode_shape(width, height, refresh_rate_millihz);
    }

    std::uint32_t append_preferred_mode_variants(
      std::vector<WindowsDriverModeShape> &modes,
      const WindowsDriverModeShape &preferred
    ) {
      std::uint32_t preferred_index = static_cast<std::uint32_t>(modes.size());
      bool preferred_index_set = false;

      // Requested timings are strictly additive: they append after the integral
      // defaults and never displace them. Whole-Hz mode matching takes the first
      // candidate in enumeration order, so keeping the nominal rate ahead of a
      // near-identical fractional one is what stops a "60 Hz" request from
      // landing on 59.94. Callers that genuinely want the fractional timing
      // select it by preferred index or by its exact vSyncFreq rational.
      for (const auto scale_percent: kPreferredModeScalePercent) {
        if (const auto scaled = scaled_mode_shape(preferred, scale_percent, preferred.refresh_rate_millihz)) {
          const auto index = append_unique_windows_driver_mode(modes, *scaled);
          if (scale_percent == 100) {
            preferred_index = index;
            preferred_index_set = true;
          }
        }

        for (const auto multiplier: {2ull, 4ull}) {
          const auto multiplied_refresh_rate_millihz = clamp_windows_driver_u32(
            static_cast<std::uint64_t>(preferred.refresh_rate_millihz) * multiplier
          );
          if (const auto multiplied = scaled_mode_shape(preferred, scale_percent, multiplied_refresh_rate_millihz)) {
            (void) append_unique_windows_driver_mode(modes, *multiplied);
          }
        }
      }

      if (!preferred_index_set) {
        preferred_index = append_unique_windows_driver_mode(modes, preferred);
      }
      return preferred_index;
    }
  }  // namespace

  bool WindowsDriverRegisteredModeStore::register_mode(
    const std::span<const std::byte, kEdidSize> edid,
    const WindowsDriverModeShape &mode
  ) {
    if (mode.width == 0 || mode.height == 0 || mode.refresh_rate_millihz == 0) {
      return false;
    }

    std::array<std::byte, kEdidSize> key {};
    std::copy(edid.begin(), edid.end(), key.begin());
    auto &entry = entries_[key];
    entry.mode = mode;
    ++entry.references;
    return true;
  }

  void WindowsDriverRegisteredModeStore::unregister_mode(
    const std::span<const std::byte, kEdidSize> edid
  ) {
    std::array<std::byte, kEdidSize> key {};
    std::copy(edid.begin(), edid.end(), key.begin());

    const auto entry = entries_.find(key);
    if (entry == entries_.end()) {
      return;
    }

    if (entry->second.references <= 1) {
      entries_.erase(entry);
      return;
    }

    --entry->second.references;
  }

  std::optional<WindowsDriverModeShape> WindowsDriverRegisteredModeStore::registered_mode(
    const std::span<const std::byte, kEdidSize> edid
  ) const {
    std::array<std::byte, kEdidSize> key {};
    std::copy(edid.begin(), edid.end(), key.begin());

    const auto entry = entries_.find(key);
    if (entry == entries_.end()) {
      return std::nullopt;
    }

    return entry->second.mode;
  }

  std::uint32_t clamp_windows_driver_u32(const std::uint64_t value) {
    return static_cast<std::uint32_t>(
      (std::min<std::uint64_t>)(value, (std::numeric_limits<std::uint32_t>::max)())
    );
  }

  WindowsDriverModeShape active_windows_driver_mode_shape(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_rate_millihz
  ) {
    return {
      width,
      height,
      width,
      height,
      static_cast<std::uint64_t>(width) *
        static_cast<std::uint64_t>(height) *
        static_cast<std::uint64_t>((std::max)(refresh_rate_millihz, 1u)) /
        1000ull,
      refresh_rate_millihz
    };
  }

  std::uint32_t append_unique_windows_driver_mode(
    std::vector<WindowsDriverModeShape> &modes,
    const WindowsDriverModeShape &candidate
  ) {
    for (std::size_t index = 0; index < modes.size(); ++index) {
      const auto &mode = modes[index];
      if (mode.width == candidate.width &&
          mode.height == candidate.height &&
          mode.refresh_rate_millihz == candidate.refresh_rate_millihz) {
        return static_cast<std::uint32_t>(index);
      }
    }

    const auto index = static_cast<std::uint32_t>(modes.size());
    modes.push_back(candidate);
    return index;
  }

  std::uint32_t default_windows_driver_preferred_mode_index(const std::vector<WindowsDriverModeShape> &modes) {
    for (std::size_t index = 0; index < modes.size(); ++index) {
      const auto &mode = modes[index];
      if (mode.width == 1920 && mode.height == 1080 && mode.refresh_rate_millihz == 60'000) {
        return static_cast<std::uint32_t>(index);
      }
    }

    return 0;
  }

  std::pair<std::vector<WindowsDriverModeShape>, std::uint32_t> build_windows_driver_mode_shapes(
    const std::optional<WindowsDriverModeShape> &preferred
  ) {
    std::vector<WindowsDriverModeShape> modes;
    modes.reserve(kDefaultWindowsDriverModes.size() + (preferred ? kPreferredModeScalePercent.size() * 3u : 0u));

    for (const auto &spec: kDefaultWindowsDriverModes) {
      modes.push_back(mode_shape_from_spec(spec));
    }

    std::uint32_t preferred_index = default_windows_driver_preferred_mode_index(modes);
    if (preferred) {
      preferred_index = append_preferred_mode_variants(modes, *preferred);
    }

    return {std::move(modes), preferred_index};
  }

  std::pair<std::vector<WindowsDriverModeShape>, std::uint32_t> build_windows_driver_target_mode_shapes(
    const std::optional<WindowsDriverModeShape> &monitor_description,
    const WindowsDriverModeShape *requested_shape
  ) {
    return build_windows_driver_mode_shapes(
      requested_shape ? std::optional<WindowsDriverModeShape> {*requested_shape} : monitor_description
    );
  }

  WindowsDriverFrequencyRational make_windows_driver_frequency_rational(
    std::uint64_t numerator,
    std::uint32_t denominator
  ) {
    denominator = (std::max)(denominator, 1u);
    if (numerator == 0) {
      return {0, 1};
    }

    const auto divisor = std::gcd(numerator, static_cast<std::uint64_t>(denominator));
    numerator /= divisor;
    denominator = static_cast<std::uint32_t>(denominator / divisor);

    return {clamp_windows_driver_u32(numerator), denominator};
  }

  WindowsDriverSignalFrequencies windows_driver_signal_frequencies(const WindowsDriverModeShape &shape) {
    const auto total_height = (std::max)(shape.total_height, shape.height);
    return {
      make_windows_driver_frequency_rational((std::max)(shape.refresh_rate_millihz, 1u), 1000),
      make_windows_driver_frequency_rational(
        static_cast<std::uint64_t>((std::max)(shape.refresh_rate_millihz, 1u)) *
          static_cast<std::uint64_t>((std::max)(total_height, 1u)),
        1000
      )
    };
  }
}  // namespace virtual_display::driver
