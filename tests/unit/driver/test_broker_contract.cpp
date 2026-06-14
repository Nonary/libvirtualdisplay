#include <gtest/gtest.h>

#include "virtual_display/driver/broker_commands.h"
#include "virtual_display/driver/display_identity.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace vdd = virtual_display::driver;

namespace {
  void set_display_name(char (&target)[vdd::kDisplayNameChars], const char *name) {
    std::fill(std::begin(target), std::end(target), '\0');
    std::memcpy(target, name, (std::min)(std::strlen(name), static_cast<std::size_t>(vdd::kDisplayNameChars - 1)));
  }
}  // namespace

TEST(VirtualDisplayBrokerContract, TrimsPipeCommands) {
  EXPECT_EQ(vdd::trim_broker_command("  permanent-query\r\n"), "permanent-query");
  EXPECT_EQ(vdd::trim_broker_command("display-query   "), "display-query");
  EXPECT_EQ(vdd::trim_broker_command(""), "");
}

TEST(VirtualDisplayBrokerContract, QuotesHelperArgumentsForWindowsCommandLine) {
  EXPECT_EQ(vdd::quote_broker_argument(L""), L"\"\"");
  EXPECT_EQ(vdd::quote_broker_argument(L"simple"), L"\"simple\"");
  EXPECT_EQ(vdd::quote_broker_argument(L"C:\\Path With Spaces\\profile.icc"), L"\"C:\\Path With Spaces\\profile.icc\"");
  EXPECT_EQ(vdd::quote_broker_argument(L"ends\\"), L"\"ends\\\\\"");
  EXPECT_EQ(vdd::quote_broker_argument(L"say \"hello\""), L"\"say \\\"hello\\\"\"");
  EXPECT_EQ(vdd::quote_broker_argument(L"slash\\\"quote"), L"\"slash\\\\\\\"quote\"");
}

TEST(VirtualDisplayBrokerContract, EscapesBrokerKeyValuePayloadText) {
  EXPECT_EQ(vdd::escape_key_value("plain"), "plain");
  EXPECT_EQ(vdd::escape_key_value("path\\name"), "path\\\\name");
  EXPECT_EQ(vdd::escape_key_value("line\r\nnext\t"), "line\\r\\nnext\\t");
  EXPECT_EQ(vdd::escape_key_value(std::string {"bad\x01\x7f", 5}), "bad\\x01\\x7F");
}

TEST(VirtualDisplayBrokerContract, ParsesPermanentSetCommand) {
  const auto request = vdd::parse_permanent_set_command("permanent-set 2 2560 1440 700 390 144500 Desk Display");
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(request->display_count, 2u);
  EXPECT_EQ(request->width, 2560u);
  EXPECT_EQ(request->height, 1440u);
  EXPECT_EQ(request->physical_width_mm, 700u);
  EXPECT_EQ(request->physical_height_mm, 390u);
  EXPECT_EQ(request->refresh_rate_millihz, 144500u);
  EXPECT_STREQ(request->display_name, "Desk Display");

  EXPECT_FALSE(vdd::parse_permanent_set_command("permanent-set 2 2560 1440 700 390").has_value());
  EXPECT_FALSE(vdd::parse_permanent_set_command("permanent-set two 2560 1440 700 390 144500 Desk Display").has_value());
}

TEST(VirtualDisplayBrokerContract, ParsesManifestProfileSetCommand) {
  const auto profile = vdd::parse_manifest_profile_set_command(
    "manifest-profile-set 3 3840 2160 600 340 120000 0 2 -1920 40 Profile Display"
  );
  ASSERT_TRUE(profile.has_value());
  EXPECT_EQ(profile->connector_index, 3u);
  EXPECT_EQ(profile->display_id, vdd::permanent_display_id(3));
  EXPECT_EQ(profile->container_id, vdd::container_guid_from_display_id(profile->display_id));
  EXPECT_EQ(profile->product_code, vdd::permanent_product_code(3));
  EXPECT_EQ(profile->serial_number, vdd::serial_number_from_display_id(profile->display_id));
  EXPECT_EQ(profile->allowed_mode_count, 1u);
  EXPECT_EQ(profile->allowed_modes[0].width, 3840u);
  EXPECT_EQ(profile->allowed_modes[0].height, 2160u);
  EXPECT_EQ(profile->allowed_modes[0].refresh_rate_millihz, 120000u);
  EXPECT_EQ(profile->layout_policy, vdd::kDisplayManifestLayoutPolicyApplyAndPersist);
  EXPECT_EQ(profile->position_x, -1920);
  EXPECT_EQ(profile->position_y, 40);
  EXPECT_EQ(profile->flags, vdd::kDisplayManifestProfileFlagRetainIdentity | vdd::kDisplayManifestProfileFlagPermanentIdentity);
  EXPECT_STREQ(profile->display_name, "Profile Display");

  EXPECT_FALSE(vdd::parse_manifest_profile_set_command("manifest-profile-set 3 3840").has_value());
  EXPECT_FALSE(vdd::parse_manifest_profile_set_command(
    "manifest-profile-set 3 3840 2160 600 340 120000 0 2 west 40 Profile Display"
  ).has_value());
}

TEST(VirtualDisplayBrokerContract, BuildsHelperArgumentsForClientSessionProbe) {
  EXPECT_EQ(vdd::helper_arguments_for_broker_command("helper-diagnose"), L"--diagnose");
  EXPECT_EQ(vdd::helper_arguments_for_broker_command("helper-apply-extended-topology"), L"--apply-extended-topology");
  EXPECT_EQ(vdd::helper_arguments_for_broker_command("helper-apply-manifest-topology"), L"--apply-manifest-topology");
  EXPECT_EQ(vdd::helper_arguments_for_broker_command("helper-query-color-profiles"), L"--query-color-profiles");
  EXPECT_EQ(vdd::helper_arguments_for_broker_command("helper-stress-capture-remove"), L"--stress-capture-remove");
  EXPECT_EQ(
    vdd::helper_arguments_for_broker_command("helper-stress-capture-remove 5 1920 1080 60"),
    L"\"--stress-capture-remove\" \"5\" \"1920\" \"1080\" \"60\""
  );
  EXPECT_EQ(
    vdd::helper_arguments_for_broker_command(
      "helper-associate-color-profile -1:123 44 advanced default C:\\Profiles\\display profile.icc"
    ),
    L"\"--associate-color-profile\" \"-1:123\" \"44\" \"C:\\Profiles\\display profile.icc\" --advanced-color --default"
  );

  EXPECT_FALSE(vdd::helper_arguments_for_broker_command("helper-stress-capture-remove 5 wide 1080 60").has_value());
  EXPECT_FALSE(vdd::helper_arguments_for_broker_command(
    "helper-associate-color-profile luid 44 advanced default profile.icc"
  ).has_value());
}

TEST(VirtualDisplayBrokerContract, FormatsPermanentStateForBrokerPayload) {
  vdd::PermanentDisplayCountResult state {};
  state.current_display_count = 2;
  state.max_display_count = 8;
  state.temporary_display_count = 1;
  state.width = 2560;
  state.height = 1440;
  state.refresh_rate_millihz = 144500;
  state.physical_width_mm = 700;
  state.physical_height_mm = 390;
  set_display_name(state.display_name, "Desk\nOne");

  EXPECT_EQ(
    vdd::format_permanent_state(state),
    "permanent_displays=2\n"
    "max_permanent_displays=8\n"
    "temporary_displays=1\n"
    "mode=2560x1440@144.5Hz\n"
    "physical_size_mm=700x390\n"
    "name=Desk\\nOne\n"
  );
}

TEST(VirtualDisplayBrokerContract, FormatsDisplayStateForBrokerPayload) {
  vdd::QueryDisplayStateResult state {};
  state.permanent_display_count = 1;
  state.temporary_display_count = 1;
  state.entry_count = 1;
  auto &entry = state.entries[0];
  entry.kind = vdd::kDisplayStateKindTemporary;
  entry.display_id = 0x6000000000001234ull;
  entry.lease_id = 0x6000000000005678ull;
  entry.connector_index = 10;
  entry.container_id = vdd::Guid {0x11223344, 0x5566, 0x7788, {0x90, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67}};
  entry.product_code = 77;
  entry.serial_number = 88;
  entry.width = 1920;
  entry.height = 1080;
  entry.refresh_rate_millihz = 60000;
  entry.physical_width_mm = 600;
  entry.physical_height_mm = 340;
  entry.flags = vdd::kDisplayStateFlagHdrSupported | vdd::kDisplayStateFlagRetainIdentity;
  set_display_name(entry.display_name, "Desk\tOne");

  const auto output = vdd::format_display_state(state);
  EXPECT_EQ(
    output,
    "permanent_displays=1\n"
    "temporary_displays=1\n"
    "display_entries=1\n"
    "display.0.kind=temporary\n"
    "display.0.display_id=6917529027641086516\n"
    "display.0.lease_id=6917529027641103992\n"
    "display.0.connector_index=10\n"
    "display.0.container_id=11223344-5566-7788-90ab-cdef01234567\n"
    "display.0.product_code=77\n"
    "display.0.serial_number=88\n"
    "display.0.mode=1920x1080@60Hz\n"
    "display.0.physical_size_mm=600x340\n"
    "display.0.hdr_supported=1\n"
    "display.0.retain_identity=1\n"
    "display.0.name=Desk\\tOne\n"
  );
}

TEST(VirtualDisplayBrokerContract, FormatsDisplayManifestForBrokerPayload) {
  vdd::DisplayManifest manifest {};
  manifest.version = 3;
  manifest.profile_count = 2;
  manifest.max_profile_count = 8;

  auto &profile = manifest.profiles[0];
  profile.flags = vdd::kDisplayManifestProfileFlagHdrSupported | vdd::kDisplayManifestProfileFlagRetainIdentity;
  profile.connector_index = 3;
  profile.display_id = 0x7000000000000003ull;
  profile.container_id = vdd::Guid {0x89abcdef, 0x1234, 0x5678, {0x90, 0xab, 0xcd, 0xef, 0x10, 0x32, 0x54, 0x76}};
  profile.product_code = 0x4003;
  profile.serial_number = 3;
  profile.physical_width_mm = 700;
  profile.physical_height_mm = 390;
  profile.native_mode_index = 1;
  profile.allowed_mode_count = 2;
  profile.layout_policy = vdd::kDisplayManifestLayoutPolicyApplyAndPersist;
  profile.position_x = -1920;
  profile.position_y = 40;
  profile.allowed_modes[0] = vdd::DisplayMode {1920, 1080, 60000};
  profile.allowed_modes[1] = vdd::DisplayMode {2560, 1440, 144500};
  set_display_name(profile.display_name, "Profile\tOne");

  auto &invalid_profile = manifest.profiles[1];
  invalid_profile.connector_index = 4;
  invalid_profile.display_id = 0x7000000000000004ull;
  invalid_profile.container_id = vdd::Guid {0x01020304, 0x0506, 0x0708, {0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10}};
  invalid_profile.product_code = 0x4004;
  invalid_profile.serial_number = 4;
  invalid_profile.physical_width_mm = 600;
  invalid_profile.physical_height_mm = 340;
  invalid_profile.native_mode_index = 1;
  invalid_profile.allowed_mode_count = 1;
  set_display_name(invalid_profile.display_name, "Invalid Profile");

  EXPECT_EQ(
    vdd::format_display_manifest(manifest),
    "manifest_version=3\n"
    "manifest_profiles=2\n"
    "manifest_max_profiles=8\n"
    "profile.0.connector_index=3\n"
    "profile.0.display_id=8070450532247928835\n"
    "profile.0.container_id=89abcdef-1234-5678-90ab-cdef10325476\n"
    "profile.0.product_code=16387\n"
    "profile.0.serial_number=3\n"
    "profile.0.mode=2560x1440@144.5Hz\n"
    "profile.0.physical_size_mm=700x390\n"
    "profile.0.hdr_supported=1\n"
    "profile.0.retain_identity=1\n"
    "profile.0.layout_policy=2\n"
    "profile.0.position=-1920,40\n"
    "profile.0.name=Profile\\tOne\n"
    "profile.1.connector_index=4\n"
    "profile.1.display_id=8070450532247928836\n"
    "profile.1.container_id=01020304-0506-0708-090a-0b0c0d0e0f10\n"
    "profile.1.product_code=16388\n"
    "profile.1.serial_number=4\n"
    "profile.1.mode=invalid\n"
    "profile.1.physical_size_mm=600x340\n"
    "profile.1.hdr_supported=0\n"
    "profile.1.retain_identity=0\n"
    "profile.1.layout_policy=0\n"
    "profile.1.position=0,0\n"
    "profile.1.name=Invalid Profile\n"
  );
}

TEST(VirtualDisplayBrokerContract, BrokerPipePolicyRejectsRemoteAndUnauthorizedClients) {
  const auto policy = vdd::broker_pipe_server_policy();

  EXPECT_TRUE(policy.reject_remote_clients);
  EXPECT_TRUE(policy.require_client_authorization);
  EXPECT_EQ(
    vdd::broker_pipe_mode(policy),
    vdd::kBrokerPipeTypeMessage |
      vdd::kBrokerPipeReadmodeMessage |
      vdd::kBrokerPipeWait |
      vdd::kBrokerPipeRejectRemoteClients
  );
  EXPECT_TRUE(vdd::broker_pipe_should_serve_client(policy, true));
  EXPECT_FALSE(vdd::broker_pipe_should_serve_client(policy, false));
  EXPECT_EQ(vdd::broker_pipe_rejection_response(policy), "error access_denied\n");
  EXPECT_EQ(policy.access_denied_response, "error access_denied\n");
}
