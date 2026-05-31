#include <gtest/gtest.h>

#include "virtual_display/driver/windows_cli_utils.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace vdd = virtual_display::driver;

namespace {
  std::string read_cli_source() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} / "tools/virtualdisplay.cpp";
    std::ifstream file {path, std::ios::binary};
    if (!file) {
      ADD_FAILURE() << "Failed to open " << path.string();
      return {};
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  std::string read_driver_cli_utils_source() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} / "src/driver/windows_cli_utils.cpp";
    std::ifstream file {path, std::ios::binary};
    if (!file) {
      ADD_FAILURE() << "Failed to open " << path.string();
      return {};
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  void expect_contains(const std::string &content, const std::string &needle) {
    EXPECT_NE(content.find(needle), std::string::npos) << "missing: " << needle;
  }
}  // namespace

TEST(VirtualDisplayCliContract, ExposesFriendlyPermanentDisplayCommands) {
  const auto source = read_cli_source();

  expect_contains(source, "status");
  expect_contains(source, "broker install|start|stop|status|uninstall");
  expect_contains(source, "broker protocol|query-state|query-manifest");
  expect_contains(source, "helper-apply-extended-topology");
  expect_contains(source, "helper-apply-manifest-topology");
  expect_contains(source, "helper-query-color-profiles");
  expect_contains(source, "broker helper-associate-color-profile <source_luid high:low> <source_id> <profile> [--advanced-color] [--default]");
  expect_contains(source, "display query");
  expect_contains(source, "driver install [--inf PATH]");
  expect_contains(source, "spawn [--width N] [--height N] [--physical-width-mm N] [--physical-height-mm N] [--refresh HZ] [--name TEXT]");
  expect_contains(source, "permanent query");
  expect_contains(source, "permanent set --count N [--width N] [--height N] [--physical-width-mm N] [--physical-height-mm N] [--refresh HZ] [--name TEXT]");
  expect_contains(source, "permanent profile set --slot N");
  expect_contains(source, "permanent off");
  expect_contains(source, "require_broker_command(\"permanent-query\", true)");
  expect_contains(source, "require_broker_command(\"display-query\", true)");
}

TEST(VirtualDisplayCliContract, BrokerCommandsUseSecuredIpcPath) {
  const auto source = read_cli_source();

  expect_contains(source, "kBrokerPipeName[] = L\"\\\\\\\\.\\\\pipe\\\\SunshineVirtualDisplayBroker\"");
  expect_contains(source, "int query_broker(const std::string_view command)");
  expect_contains(source, "BrokerResponse request_broker(const std::string_view command)");
  expect_contains(source, "int require_broker_command(const std::string_view command, const bool print_payload)");
  expect_contains(source, "require_broker_command(\"permanent-query\", true)");
  expect_contains(source, "require_broker_command(\"display-query\", true)");
  expect_contains(source, "require_broker_command(permanent_set_broker_command(*options), true)");
  expect_contains(source, "start the broker service before using display management commands");
  expect_contains(source, "permanent-set ");
  expect_contains(source, "CreateFileW(");
  expect_contains(source, "FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION");
  expect_contains(source, "WriteFile(");
  expect_contains(source, "ReadFile(");
  expect_contains(source, "kBrokerPipeIoTimeoutMs {30'000}");
  expect_contains(source, "CancelIoEx(pipe, &overlapped)");
  expect_contains(source, "wait_for_broker_pipe_io");
  expect_contains(source, "broker_pipe_server_matches_service(*server_pid)");
  expect_contains(source, "status.dwCurrentState == SERVICE_RUNNING");
  expect_contains(source, "status.dwProcessId == server_pid");
  expect_contains(source, "broker_pipe_server_has_expected_image(*server_pid)");
  expect_contains(source, "error command_too_large");
  expect_contains(source, "bytes_written != requested");
  expect_contains(source, "ERROR_MORE_DATA");
  expect_contains(source, "kMaxBrokerResponseBytes");
  expect_contains(source, "error response_too_large");
  expect_contains(source, "if (args[0] == \"broker\")");
  expect_contains(source, "args[1] == \"query-state\"");
  expect_contains(source, "args[1] == \"query-manifest\"");
  expect_contains(source, "args[1] == \"helper-diagnose\"");
  expect_contains(source, "args[1] == \"helper-apply-extended-topology\"");
  expect_contains(source, "args[1] == \"helper-apply-manifest-topology\"");
  expect_contains(source, "args[1] == \"helper-query-color-profiles\"");
  expect_contains(source, "args[1] == \"helper-associate-color-profile\"");
  expect_contains(source, "color_profile_association_broker_command(args)");
  expect_contains(source, "<< (advanced_color ? \"advanced\" : \"standard\")");
  expect_contains(source, "<< (set_default ? \"default\" : \"nodefault\")");

  const auto broker_command = source.find("if (args[0] == \"broker\")");
  const auto direct_open = source.find("vdd::open_first_control_device()");
  ASSERT_NE(broker_command, std::string::npos);
  EXPECT_EQ(direct_open, std::string::npos);
}

TEST(VirtualDisplayCliContract, BrokerServiceCommandsManageWindowsService) {
  const auto source = read_cli_source();

  expect_contains(source, "kBrokerServiceName[] = L\"SunshineVirtualDisplayBroker\"");
  expect_contains(source, "broker_executable_path()");
  expect_contains(source, "virtualdisplay_broker.exe");
  expect_contains(source, "OpenSCManagerW");
  expect_contains(source, "CreateServiceW");
  expect_contains(source, "ChangeServiceConfigW");
  expect_contains(source, "kBrokerServiceSecurityDescriptor[] = L\"D:P(A;;GA;;;SY)(A;;GA;;;BA)\"");
  expect_contains(source, "ChangeServiceConfig2W");
  expect_contains(source, "SERVICE_CONFIG_SERVICE_SID_INFO");
  expect_contains(source, "SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO");
  expect_contains(source, "kBrokerServiceInstallAccess");
  expect_contains(source, "kBrokerServiceUpdateAccess");
  expect_contains(source, "SERVICE_QUERY_CONFIG");
  expect_contains(source, "WRITE_DAC");
  expect_contains(source, "broker_executable_path_is_protected(broker_path)");
  expect_contains(source, "SERVICE_AUTO_START");
  expect_contains(source, "SERVICE_SID_TYPE_UNRESTRICTED");
  expect_contains(source, "SeTcbPrivilege");
  expect_contains(source, "ConvertStringSecurityDescriptorToSecurityDescriptorW");
  expect_contains(source, "SetServiceObjectSecurity");
  expect_contains(source, "DACL_SECURITY_INFORMATION");
  expect_contains(source, "harden_broker_service(service.value)");
  expect_contains(source, "StartServiceW");
  expect_contains(source, "ControlService");
  expect_contains(source, "QueryServiceStatusEx");
  expect_contains(source, "DeleteService");
  expect_contains(source, "broker_service_state=");
  expect_contains(source, "args[1] == \"install\"");
  expect_contains(source, "args[1] == \"start\"");
  expect_contains(source, "args[1] == \"stop\"");
  expect_contains(source, "args[1] == \"status\"");
  expect_contains(source, "args[1] == \"uninstall\"");
  expect_contains(source, "wait_for_broker_service_state(service.value, SERVICE_RUNNING)");
  expect_contains(source, "wait_for_broker_service_state(service.value, SERVICE_STOPPED)");
  expect_contains(source, "stop broker service failed native_error=");
  expect_contains(source, "broker_service_started=1");
  expect_contains(source, "broker_service_stopped=1");

  const auto create_service = source.find("CreateServiceW");
  const auto harden_service = source.find("harden_broker_service(service.value)", create_service);
  const auto cleanup_service = source.find("DeleteService(service.value)", harden_service);
  ASSERT_NE(create_service, std::string::npos);
  ASSERT_NE(harden_service, std::string::npos);
  EXPECT_NE(cleanup_service, std::string::npos);
  EXPECT_NE(source.find("restore_service_binary_path(service.value, *previous_binary_path)"), std::string::npos);
}

TEST(VirtualDisplayCliContract, WindowsUtf8WideningRejectsInvalidAndOversizedInput) {
  const auto source = read_cli_source();

  expect_contains(source, "value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())");
  expect_contains(source, "MB_ERR_INVALID_CHARS");
  expect_contains(source, "written != size");
}

TEST(VirtualDisplayCliContract, PermanentCommandsApplyModeAndNameSettings) {
  const auto source = read_cli_source();

  expect_contains(source, "request.width = options.width");
  expect_contains(source, "request.height = options.height");
  expect_contains(source, "request.physical_width_mm = options.physical_width_mm");
  expect_contains(source, "request.physical_height_mm = options.physical_height_mm");
  expect_contains(source, "request.refresh_rate_millihz = options.refresh_rate_millihz");
  expect_contains(source, "set_display_name(request.display_name, options.name)");
  expect_contains(source, "is_safe_display_name(*value)");
  expect_contains(source, "value.size() >= vdd::kDisplayNameChars");
  expect_contains(source, "ch < 0x20 || ch == 0x7f");
  expect_contains(source, "manifest-profile-set ");
  expect_contains(source, "parse_refresh_millihz");
  expect_contains(source, "std::strtod(text.c_str(), &end)");
  expect_contains(source, "end != text.c_str() + text.size()");
  expect_contains(source, "std::isfinite(refresh)");
  expect_contains(source, "(std::numeric_limits<std::uint32_t>::max)()");
  expect_contains(source, "args[0] == \"status\" && args.size() == 1");
  expect_contains(source, "args[0] == \"permanent\" && args.size() == 2 && args[1] == \"query\"");
  expect_contains(source, "args[0] == \"display\" && args.size() == 2 && args[1] == \"query\"");
  expect_contains(source, "args[0] == \"permanent\" && args.size() == 2 && args[1] == \"off\"");
}

TEST(VirtualDisplayCliContract, DisplayQueryPrintsIdentityFields) {
  const auto source = read_cli_source();

  expect_contains(source, "display_id=");
  expect_contains(source, "connector_index=");
  expect_contains(source, "container_id=");
  expect_contains(source, "product_code=");
  expect_contains(source, "serial_number=");
  expect_contains(source, "retain_identity=");
  expect_contains(source, "display_entries_reported=");
  expect_contains(source, "static_cast<unsigned int>(guid.data4[7])");
  expect_contains(source, "output_display_name(entry.display_name)");
  expect_contains(source, "escape_key_value(display_name(value))");
  expect_contains(source, "output += \"\\\\n\"");
  expect_contains(source, "output += \"\\\\x\"");
}

TEST(VirtualDisplayCliContract, DriverInstallSelfElevatesAndInstallsRootDevice) {
  const auto source = read_cli_source();

  expect_contains(source, "ShellExecuteExW(&execute)");
  expect_contains(source, "execute.lpVerb = L\"runas\"");
  expect_contains(source, "vdd::build_windows_command_parameters(wide_args)");
  expect_contains(source, "vdd::parse_driver_install_inf_path(args, default_driver_inf_path())");
  expect_contains(source, "Root\\\\SunshineVirtualDisplay");
  expect_contains(source, "UpdateDriverForPlugAndPlayDevicesW");
  expect_contains(source, "driver_installed=1");
  expect_contains(source, "multi_sz_contains(buffer.data(), required_size");
  expect_contains(source, "byte_count % sizeof(wchar_t)");
  expect_contains(source, "wide_values[char_count - 1] != L'\\0'");
  expect_contains(source, "if (!ShellExecuteExW(&execute))");
  expect_contains(source, "return static_cast<int>(exit_code)");
  expect_contains(source, "create root device failed native_error=");
  expect_contains(source, "driver install failed native_error=");
  expect_contains(source, "return reboot_required ? 3 : 0");
}

TEST(VirtualDisplayCliContract, QuotesWindowsArgumentsForElevation) {
  EXPECT_EQ(vdd::quote_windows_command_argument(L""), L"\"\"");
  EXPECT_EQ(vdd::quote_windows_command_argument(L"simple"), L"\"simple\"");
  EXPECT_EQ(vdd::quote_windows_command_argument(L"C:\\Path With Spaces\\tool.exe"), L"\"C:\\Path With Spaces\\tool.exe\"");
  EXPECT_EQ(vdd::quote_windows_command_argument(L"ends\\"), L"\"ends\\\\\"");
  EXPECT_EQ(vdd::quote_windows_command_argument(L"say \"hello\""), L"\"say \\\"hello\\\"\"");
  EXPECT_EQ(vdd::quote_windows_command_argument(L"slash\\\"quote"), L"\"slash\\\\\\\"quote\"");
}

TEST(VirtualDisplayCliContract, BuildsQuotedElevationParameterList) {
  const std::vector<std::wstring> args {
    L"driver",
    L"install",
    L"--inf",
    L"C:\\Path With Spaces\\driver.inf",
    L"say \"hello\""
  };

  EXPECT_EQ(
    vdd::build_windows_command_parameters(args),
    L"\"driver\" \"install\" \"--inf\" \"C:\\Path With Spaces\\driver.inf\" \"say \\\"hello\\\"\""
  );
}

TEST(VirtualDisplayCliContract, ParsesDriverInstallInfPathOptions) {
  const std::filesystem::path default_inf {"default-driver.inf"};
  const std::vector<std::string> default_args {"driver", "install"};
  const auto default_result = vdd::parse_driver_install_inf_path(default_args, default_inf);
  ASSERT_EQ(default_result.status, vdd::DriverInstallInfPathStatus::Ok);
  EXPECT_EQ(default_result.inf_path, default_inf);

  const std::vector<std::string> explicit_args {"driver", "install", "--inf", "custom-driver.inf"};
  const auto explicit_result = vdd::parse_driver_install_inf_path(explicit_args, default_inf);
  ASSERT_EQ(explicit_result.status, vdd::DriverInstallInfPathStatus::Ok);
  EXPECT_TRUE(explicit_result.inf_path.is_absolute());
  EXPECT_EQ(explicit_result.inf_path.filename(), "custom-driver.inf");

  const std::vector<std::string> unknown_args {"driver", "install", "--force"};
  const auto unknown_result = vdd::parse_driver_install_inf_path(unknown_args, default_inf);
  EXPECT_EQ(unknown_result.status, vdd::DriverInstallInfPathStatus::UnknownOption);
  EXPECT_EQ(unknown_result.option, "--force");

  const std::vector<std::string> missing_value_args {"driver", "install", "--inf"};
  const auto missing_value_result = vdd::parse_driver_install_inf_path(missing_value_args, default_inf);
  EXPECT_EQ(missing_value_result.status, vdd::DriverInstallInfPathStatus::MissingInfValue);

  const auto empty_default_result = vdd::parse_driver_install_inf_path(default_args, {});
  EXPECT_EQ(empty_default_result.status, vdd::DriverInstallInfPathStatus::EmptyDefaultPath);
}

TEST(VirtualDisplayCliContract, DriverInstallInfPathParsingUsesNonThrowingAbsolutePath) {
  const auto cli_source = read_cli_source();
  const auto utility_source = read_driver_cli_utils_source();

  expect_contains(utility_source, "std::error_code path_error;");
  expect_contains(utility_source, "std::filesystem::absolute(inf_value, path_error)");
  expect_contains(utility_source, "DriverInstallInfPathStatus::InvalidInfPath");
  expect_contains(cli_source, "case vdd::DriverInstallInfPathStatus::InvalidInfPath:");
  expect_contains(cli_source, "invalid --inf path:");
}
