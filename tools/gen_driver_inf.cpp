// Generates SunshineVirtualDisplayDriver.inf from its committed template by substituting
// the security-critical values from the device_identity single source of truth plus
// package-specific DriverVer metadata. This keeps the shipped, signed INF in lock-step
// with the C++ constants without a runtime file-scraping unit test: a build/ctest step
// runs this in --check mode and fails if the committed INF drifts from what the template
// + device_identity would produce.
#include "virtual_display/driver/device_identity.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {
  namespace vdd = virtual_display::driver;

  std::string read_file_binary(const std::string &path, bool &ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      ok = false;
      return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    ok = true;
    return buffer.str();
  }

  void replace_all(std::string &text, const std::string &from, const std::string &to) {
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
      text.replace(pos, from.size(), to);
      pos += to.size();
    }
  }

  constexpr const char *kDefaultDriverVerDate = "06/06/2026";
  constexpr const char *kDefaultDriverVerVersion = "1.3.0.0";

  // Pure: template text -> rendered INF. The committed INF is, by construction, exactly
  // this function applied to the committed template with the default DriverVer values.
  std::string render_inf(std::string text, const std::string &driverver_date, const std::string &driverver_version) {
    replace_all(text, "@VDD_CONTROL_INTERFACE_GUID@", vdd::format_inf_guid(vdd::kDeviceInterfaceGuid));
    replace_all(text, "@VDD_CONTROL_SECURITY_DESCRIPTOR@", vdd::control_interface_security_descriptor());
    replace_all(text, "@VDD_DRIVER_VER_DATE@", driverver_date);
    replace_all(text, "@VDD_DRIVER_VER_VERSION@", driverver_version);
    return text;
  }
}  // namespace

int main(int argc, char **argv) {
  std::vector<std::string> args(argv + 1, argv + argc);

  bool check = false;
  std::string driverver_date = kDefaultDriverVerDate;
  std::string driverver_version = kDefaultDriverVerVersion;
  for (auto it = args.begin(); it != args.end();) {
    if (*it == "--check") {
      check = true;
      it = args.erase(it);
    } else if (*it == "--driverver-date") {
      if (std::next(it) == args.end()) {
        std::cerr << "gen_driver_inf: --driverver-date requires a value\n";
        return 2;
      }
      driverver_date = *std::next(it);
      it = args.erase(it, std::next(it, 2));
    } else if (*it == "--driverver-version") {
      if (std::next(it) == args.end()) {
        std::cerr << "gen_driver_inf: --driverver-version requires a value\n";
        return 2;
      }
      driverver_version = *std::next(it);
      it = args.erase(it, std::next(it, 2));
    } else {
      ++it;
    }
  }

  if (args.size() != 2) {
    std::cerr << "usage: gen_driver_inf [--check] [--driverver-date MM/DD/YYYY] [--driverver-version A.B.C.D] <template.inf.in> <output.inf>\n";
    return 2;
  }

  bool ok = false;
  const std::string template_text = read_file_binary(args[0], ok);
  if (!ok) {
    std::cerr << "gen_driver_inf: cannot read template: " << args[0] << '\n';
    return 2;
  }

  const std::string generated = render_inf(template_text, driverver_date, driverver_version);

  if (check) {
    const std::string committed = read_file_binary(args[1], ok);
    if (!ok) {
      std::cerr << "gen_driver_inf: cannot read committed INF: " << args[1] << '\n';
      return 2;
    }
    if (committed != generated) {
      std::cerr << "gen_driver_inf: " << args[1]
                << " is out of sync with the device_identity source of truth.\n"
                   "       Regenerate it: cmake --build <build-dir> --target regen_driver_inf\n";
      return 1;
    }
    return 0;
  }

  std::ofstream output(args[1], std::ios::binary);
  if (!output) {
    std::cerr << "gen_driver_inf: cannot write: " << args[1] << '\n';
    return 2;
  }
  output << generated;
  return 0;
}
