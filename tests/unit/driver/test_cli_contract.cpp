#include <gtest/gtest.h>

#include "virtual_display/driver/broker_commands.h"
#include "virtual_display/driver/cli_commands.h"
#include "virtual_display/driver/windows_cli_utils.h"

#include <filesystem>
#include <string>
#include <vector>

namespace vdd = virtual_display::driver;

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

  const std::vector<std::string> repeated_args {"driver", "install", "--inf", "first.inf", "--inf", "second.inf"};
  const auto repeated_result = vdd::parse_driver_install_inf_path(repeated_args, default_inf);
  ASSERT_EQ(repeated_result.status, vdd::DriverInstallInfPathStatus::Ok);
  EXPECT_EQ(repeated_result.inf_path.filename(), "second.inf");

  const std::vector<std::string> unknown_args {"driver", "install", "--force"};
  const auto unknown_result = vdd::parse_driver_install_inf_path(unknown_args, default_inf);
  EXPECT_EQ(unknown_result.status, vdd::DriverInstallInfPathStatus::UnknownOption);
  EXPECT_EQ(unknown_result.option, "--force");

  const std::vector<std::string> missing_value_args {"driver", "install", "--inf"};
  const auto missing_value_result = vdd::parse_driver_install_inf_path(missing_value_args, default_inf);
  EXPECT_EQ(missing_value_result.status, vdd::DriverInstallInfPathStatus::MissingInfValue);
  EXPECT_EQ(missing_value_result.option, "--inf");

  const std::vector<std::string> empty_inf_args {"driver", "install", "--inf", ""};
  const auto empty_inf_result = vdd::parse_driver_install_inf_path(empty_inf_args, default_inf);
  EXPECT_EQ(empty_inf_result.status, vdd::DriverInstallInfPathStatus::InvalidInfPath);
  EXPECT_EQ(empty_inf_result.option, "");

  const auto empty_default_result = vdd::parse_driver_install_inf_path(default_args, {});
  EXPECT_EQ(empty_default_result.status, vdd::DriverInstallInfPathStatus::EmptyDefaultPath);
}

TEST(VirtualDisplayCliContract, ParsesRefreshTextAsMillihertz) {
  std::uint32_t parsed {};

  EXPECT_TRUE(vdd::parse_refresh_millihz("60", parsed));
  EXPECT_EQ(parsed, 60'000u);
  EXPECT_TRUE(vdd::parse_refresh_millihz("144.5", parsed));
  EXPECT_EQ(parsed, 144'500u);
  EXPECT_TRUE(vdd::parse_refresh_millihz("120000", parsed));
  EXPECT_EQ(parsed, 120'000u);

  EXPECT_FALSE(vdd::parse_refresh_millihz("", parsed));
  EXPECT_FALSE(vdd::parse_refresh_millihz("60 hz", parsed));
  EXPECT_FALSE(vdd::parse_refresh_millihz("60 ", parsed));
  EXPECT_FALSE(vdd::parse_refresh_millihz("0", parsed));
  EXPECT_FALSE(vdd::parse_refresh_millihz("nan", parsed));
  EXPECT_FALSE(vdd::parse_refresh_millihz("inf", parsed));
  EXPECT_FALSE(vdd::parse_refresh_millihz("4294967296", parsed));
}

TEST(VirtualDisplayCliContract, ValidatesDisplayNamesBeforeBrokerEncoding) {
  EXPECT_TRUE(vdd::is_safe_display_name("Desk Display"));
  EXPECT_TRUE(vdd::is_safe_display_name(std::string(vdd::kDisplayNameChars - 1u, 'x')));

  EXPECT_FALSE(vdd::is_safe_display_name(""));
  EXPECT_FALSE(vdd::is_safe_display_name(std::string(vdd::kDisplayNameChars, 'x')));
  EXPECT_FALSE(vdd::is_safe_display_name("bad\nname"));
  EXPECT_FALSE(vdd::is_safe_display_name(std::string {"bad\x7fname", 8}));
}

TEST(VirtualDisplayCliContract, ParsesPermanentDisplayOptionsIntoBrokerCommand) {
  const std::vector<std::string> args {
    "permanent",
    "set",
    "--count",
    "2",
    "--width",
    "2560",
    "--height",
    "1440",
    "--physical-width-mm",
    "700",
    "--physical-height-mm",
    "390",
    "--refresh",
    "144.5",
    "--name",
    "Desk Display"
  };

  const auto options = vdd::parse_permanent_options(args, 2, true);
  ASSERT_TRUE(options.has_value());
  EXPECT_EQ(options->count, 2u);
  EXPECT_EQ(options->width, 2560u);
  EXPECT_EQ(options->height, 1440u);
  EXPECT_EQ(options->physical_width_mm, 700u);
  EXPECT_EQ(options->physical_height_mm, 390u);
  EXPECT_EQ(options->refresh_rate_millihz, 144500u);
  EXPECT_EQ(options->name, "Desk Display");

  // The CLI emits a wire command that the broker must accept. Round-trip it through the
  // broker's parser instead of pinning a brittle literal (the exact string is pinned once
  // in test_broker_contract.cpp::ParsesPermanentSetCommand). This catches one-sided drift
  // between the CLI formatter and the broker parser.
  const auto command = vdd::permanent_set_broker_command(*options);
  const auto parsed = vdd::parse_permanent_set_command(command);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->display_count, options->count);
  EXPECT_EQ(parsed->width, options->width);
  EXPECT_EQ(parsed->height, options->height);
  EXPECT_EQ(parsed->physical_width_mm, options->physical_width_mm);
  EXPECT_EQ(parsed->physical_height_mm, options->physical_height_mm);
  EXPECT_EQ(parsed->refresh_rate_millihz, options->refresh_rate_millihz);
  EXPECT_STREQ(parsed->display_name, options->name.c_str());

  const auto request = vdd::make_permanent_count_request(*options);
  EXPECT_EQ(request.display_count, 2u);
  EXPECT_EQ(request.width, 2560u);
  EXPECT_STREQ(request.display_name, "Desk Display");
}

TEST(VirtualDisplayCliContract, RoutesPublicDisplayCommandsThroughBrokerIpc) {
  const auto status = vdd::cli_broker_route_for_args({"status"});
  EXPECT_EQ(status.action, vdd::CliBrokerRouteAction::RequireBrokerCommand);
  EXPECT_EQ(status.command, "permanent-query");
  EXPECT_TRUE(status.print_payload);

  const auto permanent_query = vdd::cli_broker_route_for_args({"permanent", "query"});
  EXPECT_EQ(permanent_query.action, vdd::CliBrokerRouteAction::RequireBrokerCommand);
  EXPECT_EQ(permanent_query.command, "permanent-query");
  EXPECT_TRUE(permanent_query.print_payload);

  const auto display_query = vdd::cli_broker_route_for_args({"display", "query"});
  EXPECT_EQ(display_query.action, vdd::CliBrokerRouteAction::RequireBrokerCommand);
  EXPECT_EQ(display_query.command, "display-query");
  EXPECT_TRUE(display_query.print_payload);

  const auto permanent_off = vdd::cli_broker_route_for_args({"permanent", "off"});
  EXPECT_EQ(permanent_off.action, vdd::CliBrokerRouteAction::RequireBrokerCommand);
  EXPECT_EQ(permanent_off.command, "permanent-set 0 1920 1080 600 340 60000 Sunshine Display");
  EXPECT_TRUE(permanent_off.print_payload);

  const auto spawn = vdd::cli_broker_route_for_args({
    "spawn",
    "--width",
    "2560",
    "--height",
    "1440",
    "--name",
    "Desk Display"
  });
  EXPECT_EQ(spawn.action, vdd::CliBrokerRouteAction::RequireBrokerCommand);
  EXPECT_EQ(spawn.command, "permanent-set 1 2560 1440 600 340 60000 Desk Display");
  EXPECT_TRUE(spawn.print_payload);

  const auto permanent_set = vdd::cli_broker_route_for_args({
    "permanent",
    "set",
    "--count",
    "2",
    "--width",
    "2560",
    "--height",
    "1440",
    "--name",
    "Desk Display"
  });
  EXPECT_EQ(permanent_set.action, vdd::CliBrokerRouteAction::RequireBrokerCommand);
  EXPECT_EQ(permanent_set.command, "permanent-set 2 2560 1440 600 340 60000 Desk Display");
  EXPECT_TRUE(permanent_set.print_payload);

  const auto profile_set = vdd::cli_broker_route_for_args({
    "permanent",
    "profile",
    "set",
    "--slot",
    "3",
    "--width",
    "3840",
    "--height",
    "2160",
    "--refresh",
    "120",
    "--layout",
    "persist",
    "--x",
    "-1920",
    "--y",
    "40",
    "--hdr",
    "0",
    "--name",
    "Profile Display"
  });
  EXPECT_EQ(profile_set.action, vdd::CliBrokerRouteAction::RequireBrokerCommand);
  EXPECT_EQ(
    profile_set.command,
    "manifest-profile-set 3 3840 2160 600 340 120000 0 2 -1920 40 Profile Display"
  );
  EXPECT_TRUE(profile_set.print_payload);
}

TEST(VirtualDisplayCliContract, PermanentOptionsRejectInvalidUserInput) {
  const std::vector<std::string> missing_count {"permanent", "set", "--width", "1920"};
  EXPECT_FALSE(vdd::parse_permanent_options(missing_count, 2, true).has_value());

  const std::vector<std::string> unknown_option {"spawn", "--bogus", "1"};
  EXPECT_FALSE(vdd::parse_permanent_options(unknown_option, 1, false).has_value());

  const std::vector<std::string> unsafe_name {"spawn", "--name", "bad\nname"};
  EXPECT_FALSE(vdd::parse_permanent_options(unsafe_name, 1, false).has_value());

  const std::vector<std::string> invalid_refresh {"spawn", "--refresh", "60 hz"};
  EXPECT_FALSE(vdd::parse_permanent_options(invalid_refresh, 1, false).has_value());
}

TEST(VirtualDisplayCliContract, ParsesProfileOptionsIntoBrokerCommand) {
  const std::vector<std::string> args {
    "permanent",
    "profile",
    "set",
    "--slot",
    "3",
    "--width",
    "3840",
    "--height",
    "2160",
    "--refresh",
    "120",
    "--layout",
    "persist",
    "--x",
    "-1920",
    "--y",
    "40",
    "--hdr",
    "0",
    "--name",
    "Profile Display"
  };

  const auto options = vdd::parse_profile_options(args, 3);
  ASSERT_TRUE(options.has_value());
  EXPECT_EQ(options->slot, 3u);
  EXPECT_EQ(options->refresh_rate_millihz, 120000u);
  EXPECT_EQ(options->layout_policy, vdd::kDisplayManifestLayoutPolicyApplyAndPersist);
  EXPECT_EQ(options->position_x, -1920);
  EXPECT_EQ(options->position_y, 40);
  EXPECT_EQ(options->hdr_supported, 0u);

  // Round-trip the CLI-emitted manifest command through the broker parser so the two stay
  // compatible (the exact literal is pinned in test_broker_contract.cpp::ParsesManifestProfileSetCommand).
  const auto command = vdd::manifest_profile_set_broker_command(*options);
  const auto parsed = vdd::parse_manifest_profile_set_command(command);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->connector_index, options->slot);
  ASSERT_GE(parsed->allowed_mode_count, 1u);
  EXPECT_EQ(parsed->allowed_modes[0].width, options->width);
  EXPECT_EQ(parsed->allowed_modes[0].height, options->height);
  EXPECT_EQ(parsed->allowed_modes[0].refresh_rate_millihz, options->refresh_rate_millihz);
  EXPECT_EQ(parsed->physical_width_mm, options->physical_width_mm);
  EXPECT_EQ(parsed->physical_height_mm, options->physical_height_mm);
  EXPECT_EQ(parsed->layout_policy, options->layout_policy);
  EXPECT_EQ(parsed->position_x, options->position_x);
  EXPECT_EQ(parsed->position_y, options->position_y);
  EXPECT_EQ((parsed->flags & vdd::kDisplayManifestProfileFlagHdrSupported) != 0u, options->hdr_supported != 0u);
  EXPECT_STREQ(parsed->display_name, options->name.c_str());
}

TEST(VirtualDisplayCliContract, ProfileOptionsRejectMissingSlotAndInvalidLayout) {
  const std::vector<std::string> missing_slot {"permanent", "profile", "set", "--width", "1920"};
  EXPECT_FALSE(vdd::parse_profile_options(missing_slot, 3).has_value());

  const std::vector<std::string> invalid_layout {"permanent", "profile", "set", "--slot", "0", "--layout", "mirror"};
  EXPECT_FALSE(vdd::parse_profile_options(invalid_layout, 3).has_value());

  const std::vector<std::string> invalid_hdr {"permanent", "profile", "set", "--slot", "0", "--hdr", "2"};
  EXPECT_FALSE(vdd::parse_profile_options(invalid_hdr, 3).has_value());
}

TEST(VirtualDisplayCliContract, BuildsBrokerHelperCommands) {
  const std::vector<std::string> color_args {
    "broker",
    "helper-associate-color-profile",
    "12:34",
    "56",
    "C:\\Profiles\\display profile.icc",
    "--advanced-color",
    "--default"
  };
  EXPECT_EQ(
    vdd::color_profile_association_broker_command(color_args),
    "helper-associate-color-profile 12:34 56 advanced default C:\\Profiles\\display profile.icc"
  );

  const std::vector<std::string> color_args_default_mode {
    "broker",
    "helper-associate-color-profile",
    "12:34",
    "56",
    "profile.icc"
  };
  EXPECT_EQ(
    vdd::color_profile_association_broker_command(color_args_default_mode),
    "helper-associate-color-profile 12:34 56 standard nodefault profile.icc"
  );

  const std::vector<std::string> stress_default {"broker", "helper-stress-capture-remove"};
  EXPECT_EQ(vdd::stress_capture_remove_broker_command(stress_default), "helper-stress-capture-remove");

  const std::vector<std::string> stress_custom {"broker", "helper-stress-capture-remove", "5", "1920", "1080", "60"};
  EXPECT_EQ(vdd::stress_capture_remove_broker_command(stress_custom), "helper-stress-capture-remove 5 1920 1080 60");
}

TEST(VirtualDisplayCliContract, RejectsInvalidBrokerHelperCommands) {
  const std::vector<std::string> color_missing_profile {"broker", "helper-associate-color-profile", "12:34", "56"};
  EXPECT_FALSE(vdd::color_profile_association_broker_command(color_missing_profile).has_value());

  const std::vector<std::string> color_unknown_flag {
    "broker",
    "helper-associate-color-profile",
    "12:34",
    "56",
    "profile.icc",
    "--global"
  };
  EXPECT_FALSE(vdd::color_profile_association_broker_command(color_unknown_flag).has_value());

  const std::vector<std::string> stress_zero_iterations {
    "broker",
    "helper-stress-capture-remove",
    "0",
    "1920",
    "1080",
    "60"
  };
  EXPECT_FALSE(vdd::stress_capture_remove_broker_command(stress_zero_iterations).has_value());

  const std::vector<std::string> stress_bad_number {
    "broker",
    "helper-stress-capture-remove",
    "5",
    "wide",
    "1080",
    "60"
  };
  EXPECT_FALSE(vdd::stress_capture_remove_broker_command(stress_bad_number).has_value());
}

TEST(VirtualDisplayCliContract, RoutesBrokerHelperCommandsThroughBrokerIpc) {
  const auto protocol = vdd::cli_broker_route_for_args({"broker", "protocol"});
  EXPECT_EQ(protocol.action, vdd::CliBrokerRouteAction::QueryBrokerCommand);
  EXPECT_EQ(protocol.command, "protocol");

  const auto query_state = vdd::cli_broker_route_for_args({"broker", "query-state"});
  EXPECT_EQ(query_state.action, vdd::CliBrokerRouteAction::QueryBrokerCommand);
  EXPECT_EQ(query_state.command, "query-state");

  const auto query_manifest = vdd::cli_broker_route_for_args({"broker", "query-manifest"});
  EXPECT_EQ(query_manifest.action, vdd::CliBrokerRouteAction::QueryBrokerCommand);
  EXPECT_EQ(query_manifest.command, "query-manifest");

  const auto diagnose = vdd::cli_broker_route_for_args({"broker", "helper-diagnose"});
  EXPECT_EQ(diagnose.action, vdd::CliBrokerRouteAction::QueryBrokerCommand);
  EXPECT_EQ(diagnose.command, "helper-diagnose");

  const auto extended_topology = vdd::cli_broker_route_for_args({"broker", "helper-apply-extended-topology"});
  EXPECT_EQ(extended_topology.action, vdd::CliBrokerRouteAction::QueryBrokerCommand);
  EXPECT_EQ(extended_topology.command, "helper-apply-extended-topology");

  const auto manifest_topology = vdd::cli_broker_route_for_args({"broker", "helper-apply-manifest-topology"});
  EXPECT_EQ(manifest_topology.action, vdd::CliBrokerRouteAction::QueryBrokerCommand);
  EXPECT_EQ(manifest_topology.command, "helper-apply-manifest-topology");

  const auto color_profiles = vdd::cli_broker_route_for_args({"broker", "helper-query-color-profiles"});
  EXPECT_EQ(color_profiles.action, vdd::CliBrokerRouteAction::QueryBrokerCommand);
  EXPECT_EQ(color_profiles.command, "helper-query-color-profiles");

  const auto helper = vdd::cli_broker_route_for_args({
    "broker",
    "helper-associate-color-profile",
    "12:34",
    "56",
    "C:\\Profiles\\display profile.icc",
    "--advanced-color",
    "--default"
  });
  EXPECT_EQ(helper.action, vdd::CliBrokerRouteAction::QueryBrokerCommand);
  EXPECT_EQ(helper.command, "helper-associate-color-profile 12:34 56 advanced default C:\\Profiles\\display profile.icc");

  const auto stress_helper = vdd::cli_broker_route_for_args({
    "broker",
    "helper-stress-capture-remove",
    "5",
    "1920",
    "1080",
    "60"
  });
  EXPECT_EQ(stress_helper.action, vdd::CliBrokerRouteAction::QueryBrokerCommand);
  EXPECT_EQ(stress_helper.command, "helper-stress-capture-remove 5 1920 1080 60");

  const auto invalid_helper = vdd::cli_broker_route_for_args({
    "broker",
    "helper-stress-capture-remove",
    "0",
    "1920",
    "1080",
    "60"
  });
  EXPECT_EQ(invalid_helper.action, vdd::CliBrokerRouteAction::InvalidArguments);
}
