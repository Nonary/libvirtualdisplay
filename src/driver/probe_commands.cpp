#include "virtual_display/driver/probe_commands.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>

namespace virtual_display::driver {
  std::uint64_t saturating_mul_u64(const std::uint64_t lhs, const std::uint64_t rhs) {
    if (lhs != 0 && rhs > (std::numeric_limits<std::uint64_t>::max)() / lhs) {
      return (std::numeric_limits<std::uint64_t>::max)();
    }
    return lhs * rhs;
  }

  std::uint32_t saturating_u32(const std::uint64_t value) {
    return static_cast<std::uint32_t>((std::min<std::uint64_t>)(
      value,
      (std::numeric_limits<std::uint32_t>::max)()
    ));
  }

  std::uint32_t refresh_millihz_from_hz(const std::uint32_t refresh_hz) {
    return saturating_u32(saturating_mul_u64(refresh_hz, 1000ull));
  }

  std::optional<std::uint32_t> parse_probe_u32_token(const std::string_view text) {
    std::uint32_t value {};
    const auto *begin = text.data();
    const auto *end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (text.empty() || result.ec != std::errc {} || result.ptr != end) {
      return std::nullopt;
    }
    return value;
  }

  std::optional<std::uint64_t> parse_probe_u64_token(const std::string_view text) {
    std::uint64_t value {};
    const auto *begin = text.data();
    const auto *end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (text.empty() || result.ec != std::errc {} || result.ptr != end) {
      return std::nullopt;
    }
    return value;
  }

  std::optional<std::int32_t> parse_probe_i32_token(const std::string_view text) {
    std::int32_t value {};
    const auto *begin = text.data();
    const auto *end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (text.empty() || result.ec != std::errc {} || result.ptr != end) {
      return std::nullopt;
    }
    return value;
  }

  bool probe_arg_count_valid(const int argc, const int minimum, const int maximum) {
    return argc >= minimum && argc <= maximum;
  }

  std::optional<ProbeCommandPlan> probe_command_plan(const std::string_view command) {
    const auto plan = [&](const int minimum_argc, const int maximum_argc) {
      return ProbeCommandPlan {
        minimum_argc,
        maximum_argc,
        probe_command_execution_stage(command)
      };
    };

    if (command == "--diagnose" ||
        command == "--apply-extended-topology-current-session" ||
        command == "--dump-display-config-current-session" ||
        command == "--apply-extended-topology" ||
        command == "--query-color-profiles" ||
        command == "--check" ||
        command == "--query-permanent" ||
        command == "--apply-manifest-topology") {
      return plan(2, 2);
    }
    if (command == "--set-hdr-current-session") {
      return plan(3, 4);
    }
    if (command == "--set-permanent") {
      return plan(3, 3);
    }
    if (command == "--remote-query-permanent") {
      return plan(3, 3);
    }
    if (command == "--remote-query-state") {
      return plan(3, 3);
    }
    if (command == "--remote-set-permanent") {
      return plan(4, 4);
    }
    if (command == "--remote-set-hdr") {
      return plan(5, 6);
    }
    if (command == "--associate-color-profile") {
      return plan(5, (std::numeric_limits<int>::max)());
    }
    if (command == "--self-test-permanent" ||
        command == "--self-test-4k240") {
      return plan(2, 3);
    }
    if (command == "--qa-multi-temp-lease") {
      return plan(2, 4);
    }
    if (command == "--self-test-temp" ||
        command == "--self-test-hdr") {
      return plan(2, 5);
    }
    if (command == "--self-test-lease-expiry" ||
        command == "--qa-temp-identity-retention" ||
        command == "--debug-temp-config" ||
        command == "--stress-capture-remove" ||
        command == "--qa-temp-lease") {
      return plan(2, 6);
    }
    return std::nullopt;
  }

  bool probe_command_arg_count_valid(const std::string_view command, const int argc) {
    const auto plan = probe_command_plan(command);
    return plan && probe_arg_count_valid(argc, plan->minimum_argc, plan->maximum_argc);
  }

  bool display_config_counts_are_reasonable(const std::uint32_t path_count, const std::uint32_t mode_count) {
    return path_count <= kMaxDisplayConfigPaths && mode_count <= kMaxDisplayConfigModes;
  }

  bool probe_command_uses_display_config(const std::string_view command) {
    return command == "--apply-extended-topology" ||
           command == "--apply-manifest-topology" ||
           command == "--query-color-profiles" ||
           command == "--associate-color-profile" ||
           command == "--self-test-4k240" ||
           command == "--self-test-hdr" ||
           command == "--qa-temp-identity-retention" ||
           command == "--qa-temp-lease" ||
           command == "--qa-multi-temp-lease" ||
           command == "--stress-capture-remove" ||
           command == "--debug-temp-config";
  }

  ProbeCommandExecutionStage probe_command_execution_stage(const std::string_view command) {
    if (command == "--diagnose" ||
        command == "--apply-extended-topology-current-session" ||
        command == "--dump-display-config-current-session" ||
        command == "--set-hdr-current-session") {
      return ProbeCommandExecutionStage::NoControlDevice;
    }
    if (command == "--apply-extended-topology" ||
        command == "--query-color-profiles" ||
        command == "--associate-color-profile") {
      return ProbeCommandExecutionStage::ActiveSessionBeforeControlDevice;
    }
    if (probe_command_uses_display_config(command)) {
      return ProbeCommandExecutionStage::ControlDeviceBeforeActiveSession;
    }
    return ProbeCommandExecutionStage::ControlDeviceWithoutActiveSession;
  }
}  // namespace virtual_display::driver
