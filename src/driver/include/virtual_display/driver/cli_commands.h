#pragma once

#include "virtual_display/driver/control_protocol.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace virtual_display::driver {
  struct PermanentOptions {
    std::uint32_t count {1};
    std::uint32_t width {1920};
    std::uint32_t height {1080};
    std::uint32_t physical_width_mm {kDefaultPhysicalWidthMillimeters};
    std::uint32_t physical_height_mm {kDefaultPhysicalHeightMillimeters};
    std::uint32_t refresh_rate_millihz {60'000};
    std::string name {"Sunshine Display"};
  };

  struct ProfileOptions: PermanentOptions {
    std::uint32_t slot {};
    std::uint32_t hdr_supported {1};
    std::uint32_t layout_policy {kDisplayManifestLayoutPolicyNone};
    std::int32_t position_x {};
    std::int32_t position_y {};
  };

  enum class CliBrokerRouteAction {
    None,
    InvalidArguments,
    RequireBrokerCommand,
    QueryBrokerCommand
  };

  struct CliBrokerRoute {
    CliBrokerRouteAction action {CliBrokerRouteAction::None};
    std::string command;
    bool print_payload {};
  };

  bool parse_cli_u32(std::string_view value, std::uint32_t &parsed);
  bool parse_cli_i32(std::string_view value, std::int32_t &parsed);
  bool parse_refresh_millihz(std::string_view value, std::uint32_t &parsed);
  bool is_safe_display_name(std::string_view value);

  std::optional<PermanentOptions> parse_permanent_options(
    const std::vector<std::string> &args,
    std::size_t first,
    bool require_count
  );
  std::optional<ProfileOptions> parse_profile_options(
    const std::vector<std::string> &args,
    std::size_t first
  );

  PermanentDisplayCountRequest make_permanent_count_request(const PermanentOptions &options);
  std::string permanent_set_broker_command(const PermanentOptions &options);
  std::string manifest_profile_set_broker_command(const ProfileOptions &options);
  std::optional<std::string> color_profile_association_broker_command(const std::vector<std::string> &args);
  std::optional<std::string> stress_capture_remove_broker_command(const std::vector<std::string> &args);
  CliBrokerRoute cli_broker_route_for_args(const std::vector<std::string> &args);
  std::string cli_usage_text();
}  // namespace virtual_display::driver
