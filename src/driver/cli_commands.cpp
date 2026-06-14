#include "virtual_display/driver/cli_commands.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <utility>

namespace virtual_display::driver {
  bool parse_cli_u32(const std::string_view value, std::uint32_t &parsed) {
    std::uint32_t output {};
    const auto *begin = value.data();
    const auto *end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, output);
    if (result.ec != std::errc {} || result.ptr != end) {
      return false;
    }
    parsed = output;
    return true;
  }

  bool parse_cli_i32(const std::string_view value, std::int32_t &parsed) {
    std::int32_t output {};
    const auto *begin = value.data();
    const auto *end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, output);
    if (result.ec != std::errc {} || result.ptr != end) {
      return false;
    }
    parsed = output;
    return true;
  }

  bool parse_refresh_millihz(const std::string_view value, std::uint32_t &parsed) {
    std::string text {value};
    if (text.empty() || std::any_of(text.begin(), text.end(), [](const unsigned char ch) {
          return std::isspace(ch) != 0;
        })) {
      return false;
    }

    errno = 0;
    char *end = nullptr;
    const double refresh = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || end != text.c_str() + text.size() || !std::isfinite(refresh) || refresh <= 0.0) {
      return false;
    }

    const double millihz = refresh <= 1000.0 ? refresh * 1000.0 : refresh;
    if (!std::isfinite(millihz) ||
        millihz < 1.0 ||
        millihz > static_cast<double>((std::numeric_limits<std::uint32_t>::max)())) {
      return false;
    }

    parsed = static_cast<std::uint32_t>(millihz);
    return true;
  }

  bool is_safe_display_name(const std::string_view value) {
    if (value.empty() || value.size() >= kDisplayNameChars) {
      return false;
    }

    return std::none_of(value.begin(), value.end(), [](const unsigned char ch) {
      return ch < 0x20 || ch == 0x7f;
    });
  }

  std::optional<PermanentOptions> parse_permanent_options(
    const std::vector<std::string> &args,
    const std::size_t first,
    const bool require_count
  ) {
    PermanentOptions options {};
    bool saw_count = false;

    for (std::size_t index = first; index < args.size(); ++index) {
      const auto &arg = args[index];
      const auto need_value = [&]() -> std::optional<std::string> {
        if (index + 1 >= args.size()) {
          std::cerr << arg << " requires a value\n";
          return std::nullopt;
        }
        return args[++index];
      };

      if (arg == "--count") {
        const auto value = need_value();
        if (!value || !parse_cli_u32(*value, options.count)) {
          std::cerr << "invalid --count value\n";
          return std::nullopt;
        }
        saw_count = true;
      } else if (arg == "--width") {
        const auto value = need_value();
        if (!value || !parse_cli_u32(*value, options.width)) {
          std::cerr << "invalid --width value\n";
          return std::nullopt;
        }
      } else if (arg == "--height") {
        const auto value = need_value();
        if (!value || !parse_cli_u32(*value, options.height)) {
          std::cerr << "invalid --height value\n";
          return std::nullopt;
        }
      } else if (arg == "--physical-width-mm") {
        const auto value = need_value();
        if (!value || !parse_cli_u32(*value, options.physical_width_mm)) {
          std::cerr << "invalid --physical-width-mm value\n";
          return std::nullopt;
        }
      } else if (arg == "--physical-height-mm") {
        const auto value = need_value();
        if (!value || !parse_cli_u32(*value, options.physical_height_mm)) {
          std::cerr << "invalid --physical-height-mm value\n";
          return std::nullopt;
        }
      } else if (arg == "--refresh") {
        const auto value = need_value();
        if (!value || !parse_refresh_millihz(*value, options.refresh_rate_millihz)) {
          std::cerr << "invalid --refresh value\n";
          return std::nullopt;
        }
      } else if (arg == "--name") {
        const auto value = need_value();
        if (!value || !is_safe_display_name(*value)) {
          std::cerr << "invalid --name value\n";
          return std::nullopt;
        }
        options.name = *value;
      } else {
        std::cerr << "unknown option: " << arg << '\n';
        return std::nullopt;
      }
    }

    if (require_count && !saw_count) {
      std::cerr << "permanent set requires --count\n";
      return std::nullopt;
    }
    return options;
  }

  std::optional<ProfileOptions> parse_profile_options(
    const std::vector<std::string> &args,
    const std::size_t first
  ) {
    ProfileOptions options {};
    bool saw_slot = false;

    for (std::size_t index = first; index < args.size(); ++index) {
      const auto &arg = args[index];
      const auto need_value = [&]() -> std::optional<std::string> {
        if (index + 1 >= args.size()) {
          std::cerr << arg << " requires a value\n";
          return std::nullopt;
        }
        return args[++index];
      };

      if (arg == "--slot") {
        const auto value = need_value();
        if (!value || !parse_cli_u32(*value, options.slot)) {
          std::cerr << "invalid --slot value\n";
          return std::nullopt;
        }
        saw_slot = true;
      } else if (arg == "--width") {
        const auto value = need_value();
        if (!value || !parse_cli_u32(*value, options.width)) {
          std::cerr << "invalid --width value\n";
          return std::nullopt;
        }
      } else if (arg == "--height") {
        const auto value = need_value();
        if (!value || !parse_cli_u32(*value, options.height)) {
          std::cerr << "invalid --height value\n";
          return std::nullopt;
        }
      } else if (arg == "--physical-width-mm") {
        const auto value = need_value();
        if (!value || !parse_cli_u32(*value, options.physical_width_mm)) {
          std::cerr << "invalid --physical-width-mm value\n";
          return std::nullopt;
        }
      } else if (arg == "--physical-height-mm") {
        const auto value = need_value();
        if (!value || !parse_cli_u32(*value, options.physical_height_mm)) {
          std::cerr << "invalid --physical-height-mm value\n";
          return std::nullopt;
        }
      } else if (arg == "--refresh") {
        const auto value = need_value();
        if (!value || !parse_refresh_millihz(*value, options.refresh_rate_millihz)) {
          std::cerr << "invalid --refresh value\n";
          return std::nullopt;
        }
      } else if (arg == "--name") {
        const auto value = need_value();
        if (!value || !is_safe_display_name(*value)) {
          std::cerr << "invalid --name value\n";
          return std::nullopt;
        }
        options.name = *value;
      } else if (arg == "--layout") {
        const auto value = need_value();
        if (!value) {
          return std::nullopt;
        }
        if (*value == "none") {
          options.layout_policy = kDisplayManifestLayoutPolicyNone;
        } else if (*value == "apply") {
          options.layout_policy = kDisplayManifestLayoutPolicyApply;
        } else if (*value == "persist") {
          options.layout_policy = kDisplayManifestLayoutPolicyApplyAndPersist;
        } else {
          std::cerr << "invalid --layout value\n";
          return std::nullopt;
        }
      } else if (arg == "--x") {
        const auto value = need_value();
        if (!value || !parse_cli_i32(*value, options.position_x)) {
          std::cerr << "invalid --x value\n";
          return std::nullopt;
        }
      } else if (arg == "--y") {
        const auto value = need_value();
        if (!value || !parse_cli_i32(*value, options.position_y)) {
          std::cerr << "invalid --y value\n";
          return std::nullopt;
        }
      } else if (arg == "--hdr") {
        const auto value = need_value();
        if (!value || !parse_cli_u32(*value, options.hdr_supported) || options.hdr_supported > 1) {
          std::cerr << "invalid --hdr value\n";
          return std::nullopt;
        }
      } else {
        std::cerr << "unknown option: " << arg << '\n';
        return std::nullopt;
      }
    }

    if (!saw_slot) {
      std::cerr << "permanent profile set requires --slot\n";
      return std::nullopt;
    }
    return options;
  }

  PermanentDisplayCountRequest make_permanent_count_request(const PermanentOptions &options) {
    PermanentDisplayCountRequest request {};
    request.display_count = options.count;
    request.width = options.width;
    request.height = options.height;
    request.physical_width_mm = options.physical_width_mm;
    request.physical_height_mm = options.physical_height_mm;
    request.refresh_rate_millihz = options.refresh_rate_millihz;
    std::fill(std::begin(request.display_name), std::end(request.display_name), '\0');
    std::memcpy(
      request.display_name,
      options.name.data(),
      (std::min)(options.name.size(), static_cast<std::size_t>(kDisplayNameChars - 1))
    );
    return request;
  }

  std::string permanent_set_broker_command(const PermanentOptions &options) {
    std::ostringstream command;
    command
      << "permanent-set " << options.count
      << ' ' << options.width
      << ' ' << options.height
      << ' ' << options.physical_width_mm
      << ' ' << options.physical_height_mm
      << ' ' << options.refresh_rate_millihz
      << ' ' << options.name;
    return command.str();
  }

  std::string manifest_profile_set_broker_command(const ProfileOptions &options) {
    std::ostringstream command;
    command
      << "manifest-profile-set " << options.slot
      << ' ' << options.width
      << ' ' << options.height
      << ' ' << options.physical_width_mm
      << ' ' << options.physical_height_mm
      << ' ' << options.refresh_rate_millihz
      << ' ' << options.hdr_supported
      << ' ' << options.layout_policy
      << ' ' << options.position_x
      << ' ' << options.position_y
      << ' ' << options.name;
    return command.str();
  }

  std::optional<std::string> color_profile_association_broker_command(const std::vector<std::string> &args) {
    if (args.size() < 5) {
      return std::nullopt;
    }

    bool advanced_color = false;
    bool set_default = false;
    for (std::size_t index = 5; index < args.size(); ++index) {
      if (args[index] == "--advanced-color") {
        advanced_color = true;
      } else if (args[index] == "--default") {
        set_default = true;
      } else {
        return std::nullopt;
      }
    }

    std::ostringstream command;
    command
      << "helper-associate-color-profile "
      << args[2] << ' '
      << args[3] << ' '
      << (advanced_color ? "advanced" : "standard") << ' '
      << (set_default ? "default" : "nodefault") << ' '
      << args[4];
    return command.str();
  }

  std::optional<std::string> stress_capture_remove_broker_command(const std::vector<std::string> &args) {
    if (args.size() == 2) {
      return std::string {"helper-stress-capture-remove"};
    }
    if (args.size() != 6) {
      return std::nullopt;
    }

    for (std::size_t index = 2; index < args.size(); ++index) {
      std::uint32_t parsed {};
      if (!parse_cli_u32(args[index], parsed)) {
        return std::nullopt;
      }
      if (index == 2 && parsed == 0) {
        return std::nullopt;
      }
    }

    std::ostringstream command;
    command
      << "helper-stress-capture-remove "
      << args[2] << ' '
      << args[3] << ' '
      << args[4] << ' '
      << args[5];
    return command.str();
  }

  CliBrokerRoute cli_broker_route_for_args(const std::vector<std::string> &args) {
    if (args.empty()) {
      return {};
    }

    const auto require_route = [](std::string command) {
      CliBrokerRoute route {};
      route.action = CliBrokerRouteAction::RequireBrokerCommand;
      route.command = std::move(command);
      route.print_payload = true;
      return route;
    };
    const auto query_route = [](std::string command) {
      CliBrokerRoute route {};
      route.action = CliBrokerRouteAction::QueryBrokerCommand;
      route.command = std::move(command);
      return route;
    };
    const auto invalid_route = [] {
      CliBrokerRoute route {};
      route.action = CliBrokerRouteAction::InvalidArguments;
      return route;
    };

    if (args[0] == "broker") {
      if (args.size() == 2 &&
          (args[1] == "protocol" ||
           args[1] == "query-state" ||
           args[1] == "query-manifest" ||
           args[1] == "helper-diagnose" ||
           args[1] == "helper-apply-extended-topology" ||
           args[1] == "helper-apply-manifest-topology" ||
           args[1] == "helper-query-color-profiles")) {
        return query_route(args[1]);
      }
      if (args.size() >= 5 && args[1] == "helper-associate-color-profile") {
        const auto command = color_profile_association_broker_command(args);
        return command ? query_route(*command) : invalid_route();
      }
      if ((args.size() == 2 || args.size() == 6) && args[1] == "helper-stress-capture-remove") {
        const auto command = stress_capture_remove_broker_command(args);
        return command ? query_route(*command) : invalid_route();
      }
      return {};
    }

    if (args[0] == "status" && args.size() == 1) {
      return require_route("permanent-query");
    }
    if (args[0] == "permanent" && args.size() == 2 && args[1] == "query") {
      return require_route("permanent-query");
    }
    if (args[0] == "display" && args.size() == 2 && args[1] == "query") {
      return require_route("display-query");
    }
    if (args[0] == "spawn") {
      const auto options = parse_permanent_options(args, 1, false);
      return options ? require_route(permanent_set_broker_command(*options)) : invalid_route();
    }
    if (args[0] == "permanent" && args.size() == 2 && args[1] == "off") {
      PermanentOptions options {};
      options.count = 0;
      return require_route(permanent_set_broker_command(options));
    }
    if (args[0] == "permanent" && args.size() >= 2 && args[1] == "set") {
      const auto options = parse_permanent_options(args, 2, true);
      return options ? require_route(permanent_set_broker_command(*options)) : invalid_route();
    }
    if (args[0] == "permanent" && args.size() >= 3 && args[1] == "profile" && args[2] == "set") {
      const auto options = parse_profile_options(args, 3);
      return options ? require_route(manifest_profile_set_broker_command(*options)) : invalid_route();
    }

    return {};
  }

  std::string cli_usage_text() {
    return
      "virtualdisplay commands:\n"
      "  driver install [--inf PATH]\n"
      "  vulkan-layer install [--json PATH] | uninstall | status\n"
      "  broker install|start|stop|status|uninstall\n"
      "  broker protocol|query-state|query-manifest|helper-diagnose|helper-apply-extended-topology|helper-apply-manifest-topology|helper-query-color-profiles\n"
      "  broker helper-associate-color-profile <source_luid high:low> <source_id> <profile> [--advanced-color] [--default]\n"
      "  broker helper-stress-capture-remove [iterations width height refresh_hz]\n"
      "  status\n"
      "  display query\n"
      "  spawn [--width N] [--height N] [--physical-width-mm N] [--physical-height-mm N] [--refresh HZ] [--name TEXT]\n"
      "  permanent query\n"
      "  permanent set --count N [--width N] [--height N] [--physical-width-mm N] [--physical-height-mm N] [--refresh HZ] [--name TEXT]\n"
      "  permanent profile set --slot N [--width N] [--height N] [--physical-width-mm N] [--physical-height-mm N] [--refresh HZ] [--name TEXT] [--layout none|apply|persist] [--x N] [--y N] [--hdr 0|1]\n"
      "  permanent off\n";
  }
}  // namespace virtual_display::driver
