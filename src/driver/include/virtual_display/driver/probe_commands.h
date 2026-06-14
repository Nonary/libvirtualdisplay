#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace virtual_display::driver {
  inline constexpr std::uint32_t kMaxDisplayConfigPaths = 128;
  inline constexpr std::uint32_t kMaxDisplayConfigModes = 256;

  enum class ProbeCommandExecutionStage {
    NoControlDevice,
    ActiveSessionBeforeControlDevice,
    ControlDeviceBeforeActiveSession,
    ControlDeviceWithoutActiveSession
  };

  struct ProbeCommandPlan {
    int minimum_argc {};
    int maximum_argc {};
    ProbeCommandExecutionStage execution_stage {ProbeCommandExecutionStage::ControlDeviceWithoutActiveSession};
  };

  std::uint64_t saturating_mul_u64(std::uint64_t lhs, std::uint64_t rhs);
  std::uint32_t saturating_u32(std::uint64_t value);
  std::uint32_t refresh_millihz_from_hz(std::uint32_t refresh_hz);
  std::optional<std::uint32_t> parse_probe_u32_token(std::string_view text);
  std::optional<std::int32_t> parse_probe_i32_token(std::string_view text);
  bool probe_arg_count_valid(int argc, int minimum, int maximum);
  std::optional<ProbeCommandPlan> probe_command_plan(std::string_view command);
  bool probe_command_arg_count_valid(std::string_view command, int argc);
  bool display_config_counts_are_reasonable(std::uint32_t path_count, std::uint32_t mode_count);
  bool probe_command_uses_display_config(std::string_view command);
  ProbeCommandExecutionStage probe_command_execution_stage(std::string_view command);
}  // namespace virtual_display::driver
