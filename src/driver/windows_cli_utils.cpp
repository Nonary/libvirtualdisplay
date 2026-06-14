#include "virtual_display/driver/windows_cli_utils.h"

#include <system_error>

namespace virtual_display::driver {
  std::wstring quote_windows_command_argument(const std::wstring_view argument) {
    std::wstring quoted {L"\""};
    std::size_t backslashes = 0;
    for (const wchar_t ch : argument) {
      if (ch == L'"') {
        quoted.append(backslashes * 2 + 1, L'\\');
        quoted += ch;
        backslashes = 0;
        continue;
      }
      if (ch == L'\\') {
        ++backslashes;
        continue;
      }
      if (backslashes != 0) {
        quoted.append(backslashes, L'\\');
        backslashes = 0;
      }
      quoted += ch;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
  }

  std::wstring build_windows_command_parameters(const std::span<const std::wstring> arguments) {
    std::wstring parameters;
    for (const auto &argument : arguments) {
      if (!parameters.empty()) {
        parameters += L' ';
      }
      parameters += quote_windows_command_argument(argument);
    }
    return parameters;
  }

  DriverInstallInfPathResult parse_driver_install_inf_path(
    const std::span<const std::string> args,
    const std::filesystem::path &default_inf_path
  ) {
    auto inf_path = default_inf_path;

    for (std::size_t index = 2; index < args.size(); ++index) {
      const auto &arg = args[index];
      if (arg != "--inf") {
        return {DriverInstallInfPathStatus::UnknownOption, {}, arg};
      }

      if (index + 1 >= args.size()) {
        return {DriverInstallInfPathStatus::MissingInfValue, {}, arg};
      }
      const auto &inf_value = args[++index];
      if (inf_value.empty()) {
        return {DriverInstallInfPathStatus::InvalidInfPath, {}, inf_value};
      }
      std::error_code path_error;
      inf_path = std::filesystem::absolute(inf_value, path_error);
      if (path_error || inf_path.empty()) {
        return {DriverInstallInfPathStatus::InvalidInfPath, {}, inf_value};
      }
    }

    if (inf_path.empty()) {
      return {DriverInstallInfPathStatus::EmptyDefaultPath, {}, {}};
    }
    return {DriverInstallInfPathStatus::Ok, inf_path, {}};
  }
}  // namespace virtual_display::driver
