#include "virtual_display/driver/windows_driver_modes.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {
  namespace vdd = virtual_display::driver;

  bool has_mode(
    const std::vector<vdd::WindowsDriverModeShape> &modes,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_rate_millihz
  ) {
    for (const auto &mode: modes) {
      if (mode.width == width && mode.height == height && mode.refresh_rate_millihz == refresh_rate_millihz) {
        return true;
      }
    }
    return false;
  }
}  // namespace

TEST(VirtualDisplayWindowsDriverModes, DefaultsPreferredModeToFullHdSixtyHertz) {
  const auto [modes, preferred_index] = vdd::build_windows_driver_mode_shapes(std::nullopt);

  ASSERT_LT(preferred_index, modes.size());
  EXPECT_EQ(modes[preferred_index].width, 1920u);
  EXPECT_EQ(modes[preferred_index].height, 1080u);
  EXPECT_EQ(modes[preferred_index].refresh_rate_millihz, 60'000u);
}

TEST(VirtualDisplayWindowsDriverModes, AddsRequestedModeAndScaledVariants) {
  const auto requested = vdd::active_windows_driver_mode_shape(5120, 1440, 240'000);

  const auto [modes, preferred_index] = vdd::build_windows_driver_mode_shapes(requested);

  ASSERT_LT(preferred_index, modes.size());
  EXPECT_EQ(modes[preferred_index].width, 5120u);
  EXPECT_EQ(modes[preferred_index].height, 1440u);
  EXPECT_EQ(modes[preferred_index].refresh_rate_millihz, 240'000u);
  EXPECT_TRUE(has_mode(modes, 5120, 1440, 480'000));
  EXPECT_TRUE(has_mode(modes, 2560, 720, 240'000));
  EXPECT_TRUE(has_mode(modes, 3840, 1080, 240'000));
}

TEST(VirtualDisplayWindowsDriverModes, DoesNotDuplicateExistingRequestedMode) {
  const auto requested = vdd::active_windows_driver_mode_shape(1920, 1080, 60'000);

  const auto [modes, preferred_index] = vdd::build_windows_driver_mode_shapes(requested);

  std::uint32_t matches = 0;
  for (const auto &mode: modes) {
    if (mode.width == 1920 && mode.height == 1080 && mode.refresh_rate_millihz == 60'000) {
      ++matches;
    }
  }
  EXPECT_EQ(matches, 1u);
  EXPECT_EQ(modes[preferred_index].width, 1920u);
  EXPECT_EQ(modes[preferred_index].height, 1080u);
  EXPECT_EQ(modes[preferred_index].refresh_rate_millihz, 60'000u);
}

TEST(VirtualDisplayWindowsDriverModes, ActiveModeShapeUsesActiveOnlyTimingForPixelRate) {
  const auto mode = vdd::active_windows_driver_mode_shape(3840, 2160, 144'000);

  EXPECT_EQ(mode.total_width, 3840u);
  EXPECT_EQ(mode.total_height, 2160u);
  EXPECT_EQ(mode.pixel_rate, 1'194'393'600ull);
}

TEST(VirtualDisplayWindowsDriverModes, FrequencyRationalReducesMillihertzValues) {
  const auto sixty_hertz = vdd::make_windows_driver_frequency_rational(60'000, 1000);
  EXPECT_EQ(sixty_hertz.numerator, 60u);
  EXPECT_EQ(sixty_hertz.denominator, 1u);

  const auto fractional = vdd::make_windows_driver_frequency_rational(59'940, 1000);
  EXPECT_EQ(fractional.numerator, 2997u);
  EXPECT_EQ(fractional.denominator, 50u);
}

TEST(VirtualDisplayWindowsDriverModes, FrequencyRationalHandlesZeroAndSaturation) {
  const auto zero = vdd::make_windows_driver_frequency_rational(0, 0);
  EXPECT_EQ(zero.numerator, 0u);
  EXPECT_EQ(zero.denominator, 1u);

  const auto saturated = vdd::make_windows_driver_frequency_rational(
    static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) + 10ull,
    1
  );
  EXPECT_EQ(saturated.numerator, (std::numeric_limits<std::uint32_t>::max)());
  EXPECT_EQ(saturated.denominator, 1u);
}

TEST(VirtualDisplayWindowsDriverModes, TargetModesPreferRequestedTimingOverMonitorDescriptionTiming) {
  const auto monitor_description = vdd::active_windows_driver_mode_shape(1920, 1080, 60'000);
  const auto requested = vdd::active_windows_driver_mode_shape(5120, 1440, 240'000);

  const auto [modes, preferred_index] =
    vdd::build_windows_driver_target_mode_shapes(monitor_description, &requested);

  ASSERT_LT(preferred_index, modes.size());
  EXPECT_EQ(modes[preferred_index].width, requested.width);
  EXPECT_EQ(modes[preferred_index].height, requested.height);
  EXPECT_EQ(modes[preferred_index].refresh_rate_millihz, requested.refresh_rate_millihz);
  EXPECT_TRUE(has_mode(modes, 5120, 1440, 480'000));
}

TEST(VirtualDisplayWindowsDriverModes, TargetModesFallBackToMonitorDescriptionWhenNoRequestExists) {
  const auto monitor_description = vdd::active_windows_driver_mode_shape(3440, 1440, 100'000);

  const auto [modes, preferred_index] =
    vdd::build_windows_driver_target_mode_shapes(monitor_description, nullptr);

  ASSERT_LT(preferred_index, modes.size());
  EXPECT_EQ(modes[preferred_index].width, monitor_description.width);
  EXPECT_EQ(modes[preferred_index].height, monitor_description.height);
  EXPECT_EQ(modes[preferred_index].refresh_rate_millihz, monitor_description.refresh_rate_millihz);
}

TEST(VirtualDisplayWindowsDriverModes, TargetModesFallBackToDefaultWhenNoTimingExists) {
  const auto [modes, preferred_index] =
    vdd::build_windows_driver_target_mode_shapes(std::nullopt, nullptr);

  ASSERT_LT(preferred_index, modes.size());
  EXPECT_EQ(modes[preferred_index].width, 1920u);
  EXPECT_EQ(modes[preferred_index].height, 1080u);
  EXPECT_EQ(modes[preferred_index].refresh_rate_millihz, 60'000u);
}

TEST(VirtualDisplayWindowsDriverModes, RegisteredMonitorDescriptionTimingOverridesEdidTiming) {
  vdd::WindowsDriverRegisteredModeStore store;
  std::array<std::byte, vdd::kEdidSize> edid {};
  edid[0] = std::byte {0x11};
  const auto requested_mode = vdd::active_windows_driver_mode_shape(5120, 1440, 240'000);

  ASSERT_TRUE(store.register_mode(edid, requested_mode));
  const auto registered = store.registered_mode(edid);

  ASSERT_TRUE(registered.has_value());
  EXPECT_EQ(registered->width, requested_mode.width);
  EXPECT_EQ(registered->height, requested_mode.height);
  EXPECT_EQ(registered->refresh_rate_millihz, requested_mode.refresh_rate_millihz);
}

TEST(VirtualDisplayWindowsDriverModes, RegisteredMonitorDescriptionUnregistersByReferenceCount) {
  vdd::WindowsDriverRegisteredModeStore store;
  std::array<std::byte, vdd::kEdidSize> edid {};
  edid[0] = std::byte {0x22};
  const auto requested_mode = vdd::active_windows_driver_mode_shape(3440, 1440, 100'000);

  ASSERT_TRUE(store.register_mode(edid, requested_mode));
  ASSERT_TRUE(store.register_mode(edid, requested_mode));

  store.unregister_mode(edid);
  ASSERT_TRUE(store.registered_mode(edid).has_value());

  store.unregister_mode(edid);
  EXPECT_FALSE(store.registered_mode(edid).has_value());
}

TEST(VirtualDisplayWindowsDriverModes, RegisteredMonitorDescriptionIgnoresInvalidTiming) {
  vdd::WindowsDriverRegisteredModeStore store;
  std::array<std::byte, vdd::kEdidSize> edid {};
  const auto parsed_edid_mode = vdd::active_windows_driver_mode_shape(1920, 1080, 60'000);
  auto invalid_mode = parsed_edid_mode;
  invalid_mode.width = 0;

  EXPECT_FALSE(store.register_mode(edid, invalid_mode));
  EXPECT_FALSE(store.registered_mode(edid).has_value());
}

TEST(VirtualDisplayWindowsDriverModes, SignalFrequenciesUseReducedVerticalAndHorizontalRates) {
  auto mode = vdd::active_windows_driver_mode_shape(1920, 1080, 59'940);
  mode.total_height = 1125;

  const auto frequencies = vdd::windows_driver_signal_frequencies(mode);

  EXPECT_EQ(frequencies.vertical.numerator, 2997u);
  EXPECT_EQ(frequencies.vertical.denominator, 50u);
  EXPECT_EQ(frequencies.horizontal.numerator, 134865u);
  EXPECT_EQ(frequencies.horizontal.denominator, 2u);
}
