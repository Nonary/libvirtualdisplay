// Generates SunshineVirtualDisplayDriver.inf from its committed template by substituting
// the security-critical values from the device_identity single source of truth. This keeps
// the shipped, signed INF in lock-step with the C++ constants without a runtime
// file-scraping unit test: a build/ctest step runs this in --check mode and fails if the
// committed INF drifts from what the template + device_identity would produce.
#include "virtual_display/driver/device_identity.h"

#include <fstream>
#include <iostream>
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

  // Pure: template text -> rendered INF. The committed INF is, by construction, exactly
  // this function applied to the committed template.
  std::string render_inf(std::string text) {
    replace_all(text, "@VDD_CONTROL_INTERFACE_GUID@", vdd::format_inf_guid(vdd::kDeviceInterfaceGuid));
    replace_all(text, "@VDD_CONTROL_SECURITY_DESCRIPTOR@", vdd::control_interface_security_descriptor());
    return text;
  }
}  // namespace

int main(int argc, char **argv) {
  std::vector<std::string> args(argv + 1, argv + argc);

  bool check = false;
  if (!args.empty() && args.front() == "--check") {
    check = true;
    args.erase(args.begin());
  }

  if (args.size() != 2) {
    std::cerr << "usage: gen_driver_inf [--check] <template.inf.in> <output.inf>\n";
    return 2;
  }

  bool ok = false;
  const std::string template_text = read_file_binary(args[0], ok);
  if (!ok) {
    std::cerr << "gen_driver_inf: cannot read template: " << args[0] << '\n';
    return 2;
  }

  const std::string generated = render_inf(template_text);

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
