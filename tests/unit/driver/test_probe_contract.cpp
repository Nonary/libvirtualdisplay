#include <gtest/gtest.h>

#include "virtual_display/driver/probe_commands.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string_view>

namespace vdd = virtual_display::driver;

TEST(VirtualDisplayProbeContract, RefreshConversionSaturatesWithoutOverflow) {
  EXPECT_EQ(vdd::refresh_millihz_from_hz(0), 0u);
  EXPECT_EQ(vdd::refresh_millihz_from_hz(60), 60'000u);
  EXPECT_EQ(vdd::refresh_millihz_from_hz(240), 240'000u);
  EXPECT_EQ(vdd::refresh_millihz_from_hz((std::numeric_limits<std::uint32_t>::max)()), (std::numeric_limits<std::uint32_t>::max)());

  EXPECT_EQ(vdd::saturating_mul_u64(12, 34), 408u);
  EXPECT_EQ(vdd::saturating_mul_u64((std::numeric_limits<std::uint64_t>::max)(), 2), (std::numeric_limits<std::uint64_t>::max)());
  EXPECT_EQ(vdd::saturating_u32(static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) + 1u), (std::numeric_limits<std::uint32_t>::max)());
}

TEST(VirtualDisplayProbeContract, ParsesIntegerTokensExactly) {
  EXPECT_EQ(vdd::parse_probe_u32_token("0"), 0u);
  EXPECT_EQ(vdd::parse_probe_u32_token("4294967295"), (std::numeric_limits<std::uint32_t>::max)());
  EXPECT_FALSE(vdd::parse_probe_u32_token("").has_value());
  EXPECT_FALSE(vdd::parse_probe_u32_token("1x").has_value());
  EXPECT_FALSE(vdd::parse_probe_u32_token("-1").has_value());
  EXPECT_FALSE(vdd::parse_probe_u32_token("4294967296").has_value());

  EXPECT_EQ(vdd::parse_probe_i32_token("-2147483648"), (std::numeric_limits<std::int32_t>::min)());
  EXPECT_EQ(vdd::parse_probe_i32_token("2147483647"), (std::numeric_limits<std::int32_t>::max)());
  EXPECT_FALSE(vdd::parse_probe_i32_token("").has_value());
  EXPECT_FALSE(vdd::parse_probe_i32_token("42 ").has_value());
  EXPECT_FALSE(vdd::parse_probe_i32_token("2147483648").has_value());
}

TEST(VirtualDisplayProbeContract, DisplayConfigAllocationSizesAreBounded) {
  EXPECT_TRUE(vdd::display_config_counts_are_reasonable(0, 0));
  EXPECT_TRUE(vdd::display_config_counts_are_reasonable(vdd::kMaxDisplayConfigPaths, vdd::kMaxDisplayConfigModes));
  EXPECT_FALSE(vdd::display_config_counts_are_reasonable(vdd::kMaxDisplayConfigPaths + 1u, 1));
  EXPECT_FALSE(vdd::display_config_counts_are_reasonable(1, vdd::kMaxDisplayConfigModes + 1u));
}

TEST(VirtualDisplayProbeContract, PlansProbeCommandArgcBoundsAndExecutionStage) {
  // Only the fields production actually consumes are asserted: probe_command_arg_count_valid()
  // gates argv length (virtualdisplay_probe.cpp require_command_arg_count), and execution_stage
  // orders the active-console-session check relative to opening the control device. The probe
  // tool hardcodes its own dispatch, so the former probe_public_commands() registry check and
  // the never-read uses_display_config field were a test-only mirror and have been dropped.
  struct ExpectedPlan {
    std::string_view command;
    int minimum_argc;
    int maximum_argc;
    vdd::ProbeCommandExecutionStage execution_stage;
  };

  constexpr std::array kPlans {
    ExpectedPlan {"--diagnose", 2, 2, vdd::ProbeCommandExecutionStage::NoControlDevice},
    ExpectedPlan {"--apply-extended-topology", 2, 2, vdd::ProbeCommandExecutionStage::ActiveSessionBeforeControlDevice},
    ExpectedPlan {"--query-color-profiles", 2, 2, vdd::ProbeCommandExecutionStage::ActiveSessionBeforeControlDevice},
    ExpectedPlan {"--associate-color-profile", 5, (std::numeric_limits<int>::max)(), vdd::ProbeCommandExecutionStage::ActiveSessionBeforeControlDevice},
    ExpectedPlan {"--check", 2, 2, vdd::ProbeCommandExecutionStage::ControlDeviceWithoutActiveSession},
    ExpectedPlan {"--query-permanent", 2, 2, vdd::ProbeCommandExecutionStage::ControlDeviceWithoutActiveSession},
    ExpectedPlan {"--apply-manifest-topology", 2, 2, vdd::ProbeCommandExecutionStage::ControlDeviceBeforeActiveSession},
    ExpectedPlan {"--set-permanent", 3, 3, vdd::ProbeCommandExecutionStage::ControlDeviceWithoutActiveSession},
    ExpectedPlan {"--self-test-permanent", 2, 3, vdd::ProbeCommandExecutionStage::ControlDeviceWithoutActiveSession},
    ExpectedPlan {"--self-test-temp", 2, 5, vdd::ProbeCommandExecutionStage::ControlDeviceWithoutActiveSession},
    ExpectedPlan {"--self-test-4k240", 2, 3, vdd::ProbeCommandExecutionStage::ControlDeviceBeforeActiveSession},
    ExpectedPlan {"--self-test-hdr", 2, 5, vdd::ProbeCommandExecutionStage::ControlDeviceBeforeActiveSession},
    ExpectedPlan {"--self-test-lease-expiry", 2, 6, vdd::ProbeCommandExecutionStage::ControlDeviceWithoutActiveSession},
    ExpectedPlan {"--qa-multi-temp-lease", 2, 4, vdd::ProbeCommandExecutionStage::ControlDeviceBeforeActiveSession},
    ExpectedPlan {"--qa-temp-identity-retention", 2, 6, vdd::ProbeCommandExecutionStage::ControlDeviceBeforeActiveSession},
    ExpectedPlan {"--debug-temp-config", 2, 6, vdd::ProbeCommandExecutionStage::ControlDeviceBeforeActiveSession},
    ExpectedPlan {"--stress-capture-remove", 2, 6, vdd::ProbeCommandExecutionStage::ControlDeviceBeforeActiveSession},
    ExpectedPlan {"--qa-temp-lease", 2, 6, vdd::ProbeCommandExecutionStage::ControlDeviceBeforeActiveSession}
  };

  for (const auto &expected: kPlans) {
    const auto plan = vdd::probe_command_plan(expected.command);
    ASSERT_TRUE(plan.has_value()) << expected.command;
    EXPECT_EQ(plan->minimum_argc, expected.minimum_argc) << expected.command;
    EXPECT_EQ(plan->maximum_argc, expected.maximum_argc) << expected.command;
    EXPECT_EQ(plan->execution_stage, expected.execution_stage) << expected.command;

    EXPECT_TRUE(vdd::probe_command_arg_count_valid(expected.command, expected.minimum_argc)) << expected.command;
    EXPECT_TRUE(vdd::probe_command_arg_count_valid(expected.command, expected.maximum_argc)) << expected.command;
    EXPECT_FALSE(vdd::probe_command_arg_count_valid(expected.command, expected.minimum_argc - 1)) << expected.command;
    if (expected.maximum_argc < (std::numeric_limits<int>::max)()) {
      EXPECT_FALSE(vdd::probe_command_arg_count_valid(expected.command, expected.maximum_argc + 1)) << expected.command;
    }
  }

  EXPECT_FALSE(vdd::probe_command_plan("--unknown").has_value());
  EXPECT_FALSE(vdd::probe_command_arg_count_valid("--unknown", 2));
}
