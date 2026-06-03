#include <gtest/gtest.h>
#include "virtual_display/driver/control_protocol.h"
#include "virtual_display/driver/display_identity.h"
#include "virtual_display/driver/windows_control_protocol.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace vdd = virtual_display::driver;

namespace {
  constexpr std::uint64_t lease_id(const std::uint64_t suffix) {
    return vdd::kMinOpaqueLeaseId | suffix;
  }

  void set_manifest_profile_identity(
    vdd::DisplayManifestProfile &profile,
    const std::uint32_t connector_index
  ) {
    profile.connector_index = connector_index;
    profile.display_id = vdd::permanent_display_id(connector_index);
    profile.container_id = vdd::container_guid_from_display_id(profile.display_id);
    profile.product_code = vdd::permanent_product_code(connector_index);
    profile.serial_number = vdd::serial_number_from_display_id(profile.display_id);
  }

  vdd::CreateTemporaryDisplayRequest valid_create_request() {
    vdd::CreateTemporaryDisplayRequest request {};
    request.lease_id = lease_id(10);
    request.display_id = 20;
    request.width = 2560;
    request.height = 1440;
    request.physical_width_mm = 600;
    request.physical_height_mm = 340;
    request.refresh_rate_millihz = 120'000;
    request.requested_timeout_ms = 30'000;
    std::memcpy(request.display_name, "Sunshine Display", 16);
    return request;
  }

  vdd::DisplayManifest valid_display_manifest() {
    vdd::DisplayManifest manifest {};
    manifest.profile_count = 1;
    manifest.max_profile_count = 2;
    auto &profile = manifest.profiles[0];
    profile.flags = vdd::kDisplayManifestProfileFlagHdrSupported |
      vdd::kDisplayManifestProfileFlagRetainIdentity |
      vdd::kDisplayManifestProfileFlagPermanentIdentity;
    set_manifest_profile_identity(profile, 0);
    std::memcpy(profile.manufacturer_id, "SDD", 4);
    profile.physical_width_mm = 700;
    profile.physical_height_mm = 390;
    profile.native_mode_index = 0;
    profile.allowed_mode_count = 2;
    profile.layout_policy = vdd::kDisplayManifestLayoutPolicyApplyAndPersist;
    profile.position_x = 1920;
    profile.position_y = -100;
    profile.orientation = vdd::kDisplayManifestOrientationDefault;
    profile.allowed_modes[0] = {3840, 2160, 144'000};
    profile.allowed_modes[1] = {2560, 1440, 120'000};
    std::memcpy(profile.display_name, "Desk Display", 13);
    return manifest;
  }

  std::string read_readme() {
    const std::string readme_path =
      std::string {LIBVIRTUALDISPLAY_SOURCE_DIR} +
      "/README.md";
    std::ifstream readme_file {readme_path};
    if (!readme_file.is_open()) {
      ADD_FAILURE() << readme_path;
      return {};
    }

    std::ostringstream buffer;
    buffer << readme_file.rdbuf();
    return buffer.str();
  }

  std::string read_driver_inf() {
    const std::string inf_path =
      std::string {LIBVIRTUALDISPLAY_SOURCE_DIR} +
      "/src/driver/windows_driver/SunshineVirtualDisplayDriver.inf";
    std::ifstream inf_file {inf_path};
    if (!inf_file.is_open()) {
      ADD_FAILURE() << inf_path;
      return {};
    }

    std::ostringstream buffer;
    buffer << inf_file.rdbuf();
    return buffer.str();
  }

  std::string_view trim_line(std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
      line.remove_prefix(1);
    }
    return line;
  }

  std::string_view inf_section(const std::string &inf, const std::string_view name) {
    const auto header = "[" + std::string {name} + "]";
    const auto content = std::string_view {inf};
    std::size_t cursor = 0;
    std::size_t body_begin = std::string_view::npos;

    while (cursor < content.size()) {
      const auto line_begin = cursor;
      const auto line_end = content.find('\n', cursor);
      const auto next_cursor = line_end == std::string_view::npos ? content.size() : line_end + 1;
      const auto line = content.substr(
        line_begin,
        line_end == std::string_view::npos ? std::string_view::npos : line_end - line_begin
      );
      const auto trimmed = trim_line(line);
      if (!trimmed.starts_with(';') && trimmed == header) {
        body_begin = next_cursor;
        break;
      }
      cursor = next_cursor;
    }

    if (body_begin == std::string_view::npos) {
      ADD_FAILURE() << "missing INF section: " << name;
      return {};
    }

    cursor = body_begin;
    while (cursor < content.size()) {
      const auto line_begin = cursor;
      const auto line_end = content.find('\n', cursor);
      const auto next_cursor = line_end == std::string_view::npos ? content.size() : line_end + 1;
      const auto line = content.substr(
        line_begin,
        line_end == std::string_view::npos ? std::string_view::npos : line_end - line_begin
      );
      const auto trimmed = trim_line(line);
      if (!trimmed.starts_with(';') && trimmed.starts_with('[')) {
        return content.substr(body_begin, line_begin - body_begin);
      }
      cursor = next_cursor;
    }

    return content.substr(body_begin);
  }

  std::vector<std::string_view> section_security_lines(const std::string_view section) {
    std::vector<std::string_view> lines;
    std::size_t cursor = 0;
    while (cursor < section.size()) {
      const auto line_end = section.find('\n', cursor);
      auto line = section.substr(
        cursor,
        line_end == std::string_view::npos ? std::string_view::npos : line_end - cursor
      );
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      if (line.starts_with("HKR,,Security,,")) {
        lines.push_back(line);
      }
      if (line_end == std::string_view::npos) {
        break;
      }
      cursor = line_end + 1;
    }
    return lines;
  }

  void expect_exact_section_security(
    const std::string_view section,
    const std::string_view expected_sddl
  ) {
    const auto expected_line = "HKR,,Security,,\"" + std::string {expected_sddl} + "\"";
    const auto security_lines = section_security_lines(section);
    ASSERT_EQ(security_lines.size(), 1u);
    EXPECT_EQ(security_lines[0], expected_line);
  }
}  // namespace

TEST(VirtualDisplayDriverControlProtocol, ComputesBufferedUnknownDeviceIoctlCodes) {
  EXPECT_EQ(vdd::kIoctlGetProtocolVersion, 0x00222400u);
  EXPECT_EQ(vdd::kIoctlCreateTemporaryDisplay, 0x0022e404u);
  EXPECT_EQ(vdd::kIoctlRemoveTemporaryDisplay, 0x0022e408u);
  EXPECT_EQ(vdd::kIoctlFeedLease, 0x0022e40cu);
  EXPECT_EQ(vdd::kIoctlReleaseLease, 0x0022e410u);
  EXPECT_EQ(vdd::kIoctlQueryLease, 0x00226414u);
  EXPECT_EQ(vdd::kIoctlSetPermanentDisplayCount, 0x0022e418u);
  EXPECT_EQ(vdd::kIoctlQueryPermanentDisplayCount, 0x0022641cu);
  EXPECT_EQ(vdd::kIoctlQueryDisplayState, 0x00226420u);
  EXPECT_EQ(vdd::kIoctlSetDisplayManifest, 0x0022e424u);
  EXPECT_EQ(vdd::kIoctlQueryDisplayManifest, 0x00226428u);
  EXPECT_EQ(vdd::kIoctlSetRenderAdapter, 0x0022a42cu);
}

TEST(VirtualDisplayDriverControlProtocol, ProtocolVersionUsesDedicatedNamespace) {
  const vdd::ProtocolVersion version {};

  EXPECT_EQ(version.api_namespace, vdd::kApiNamespaceGuid);
  EXPECT_EQ(version.major, vdd::kProtocolVersionMajor);
  EXPECT_EQ(version.minor, vdd::kProtocolVersionMinor);
  EXPECT_EQ(version.patch, vdd::kProtocolVersionPatch);
}

TEST(VirtualDisplayDriverControlProtocol, ReadmeDocumentsCurrentProtocolVersion) {
  const auto readme = read_readme();
  const std::string expected_version =
    std::to_string(vdd::kProtocolVersionMajor) + "." +
    std::to_string(vdd::kProtocolVersionMinor) + "." +
    std::to_string(vdd::kProtocolVersionPatch);

  EXPECT_NE(
    readme.find("The current protocol version is `" + expected_version + "`."),
    std::string::npos
  );
}

TEST(VirtualDisplayDriverControlProtocol, WindowsGuidAdapterPreservesProtocolGuid) {
#ifdef _WIN32
  const auto win_guid = vdd::to_windows_guid(vdd::kApiNamespaceGuid);

  EXPECT_EQ(vdd::from_windows_guid(win_guid), vdd::kApiNamespaceGuid);
#endif
}

TEST(VirtualDisplayDriverControlProtocol, InfRegistersControlInterfaceWithServiceOnlyAccess) {
  const auto inf = read_driver_inf();
  const auto control_section = inf_section(inf, "ControlInterface_AddReg");

  EXPECT_NE(
    inf.find("AddInterface={5f894d6c-3a69-48a2-86ef-e4c671932d63},,ControlInterface"),
    std::string::npos
  );
  expect_exact_section_security(
    control_section,
    "D:P(A;;GA;;;SY)(A;;GA;;;S-1-5-80-2333729190-1599198784-3320592948-2337414441-3098439965)"
  );
}

TEST(VirtualDisplayDriverControlProtocol, InfRestrictsDeviceSecurityToSystemAndBrokerService) {
  const auto inf = read_driver_inf();
  const auto device_section = inf_section(inf, "Device_Install_Hw_AddReg");

  expect_exact_section_security(
    device_section,
    "D:P(A;;GA;;;SY)(A;;GA;;;S-1-5-80-2333729190-1599198784-3320592948-2337414441-3098439965)"
  );
}

TEST(VirtualDisplayDriverControlProtocol, NormalizesLeaseTimeouts) {
  EXPECT_EQ(vdd::normalize_timeout_ms(0), vdd::kDefaultLeaseTimeoutMs);
  EXPECT_EQ(vdd::normalize_timeout_ms(1), vdd::kMinLeaseTimeoutMs);
  EXPECT_EQ(vdd::normalize_timeout_ms(vdd::kMinLeaseTimeoutMs + 1), vdd::kMinLeaseTimeoutMs + 1);
  EXPECT_EQ(vdd::normalize_timeout_ms(vdd::kMaxLeaseTimeoutMs + 1), vdd::kMaxLeaseTimeoutMs);
}

TEST(VirtualDisplayDriverControlProtocol, ValidatesCreateRequest) {
  const auto request = valid_create_request();
  vdd::ValidatedCreateTemporaryDisplay validated {};

  EXPECT_EQ(vdd::validate_create_temporary_display(request, &validated), vdd::ValidationError::None);
  EXPECT_EQ(validated.effective_timeout_ms, request.requested_timeout_ms);
  EXPECT_EQ(validated.request.physical_width_mm, 600u);
  EXPECT_EQ(validated.request.physical_height_mm, 340u);
  EXPECT_EQ(validated.display_name, "Sunshine Display");
}

TEST(VirtualDisplayDriverControlProtocol, CanonicalizesDisplayNames) {
  auto request = valid_create_request();
  std::fill(std::begin(request.display_name), std::end(request.display_name), '\0');
  std::memcpy(request.display_name, "Desk Display   ", 15);
  vdd::ValidatedCreateTemporaryDisplay validated {};

  EXPECT_EQ(vdd::validate_create_temporary_display(request, &validated), vdd::ValidationError::None);
  EXPECT_EQ(validated.display_name, "Desk Display");
  EXPECT_EQ(validated.request.display_name[12], '\0');
}

TEST(VirtualDisplayDriverControlProtocol, ValidatedCreateCopiesRebindDisplayNameView) {
  const auto request = valid_create_request();
  vdd::ValidatedCreateTemporaryDisplay validated {};
  ASSERT_EQ(vdd::validate_create_temporary_display(request, &validated), vdd::ValidationError::None);

  const auto copied = validated;
  ASSERT_EQ(copied.display_name, "Sunshine Display");
  EXPECT_EQ(copied.display_name.data(), copied.request.display_name);
  EXPECT_NE(copied.display_name.data(), validated.display_name.data());

  auto assigned = vdd::ValidatedCreateTemporaryDisplay {};
  assigned = validated;
  ASSERT_EQ(assigned.display_name, "Sunshine Display");
  EXPECT_EQ(assigned.display_name.data(), assigned.request.display_name);
  EXPECT_NE(assigned.display_name.data(), validated.display_name.data());
}

TEST(VirtualDisplayDriverControlProtocol, DefaultsCreatePhysicalSize) {
  auto request = valid_create_request();
  request.physical_width_mm = 0;
  request.physical_height_mm = 0;
  vdd::ValidatedCreateTemporaryDisplay validated {};

  EXPECT_EQ(vdd::validate_create_temporary_display(request, &validated), vdd::ValidationError::None);
  EXPECT_EQ(validated.request.physical_width_mm, vdd::kDefaultPhysicalWidthMillimeters);
  EXPECT_EQ(validated.request.physical_height_mm, vdd::kDefaultPhysicalHeightMillimeters);
}

TEST(VirtualDisplayDriverControlProtocol, RejectsWrongNamespace) {
  auto request = valid_create_request();
  request.api_namespace.data1 ^= 1;

  EXPECT_EQ(
    vdd::validate_create_temporary_display(request),
    vdd::ValidationError::WrongApiNamespace
  );
}

TEST(VirtualDisplayDriverControlProtocol, RejectsMissingIdentifiers) {
  auto request = valid_create_request();
  request.lease_id = 0;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::MissingLeaseId);

  request = valid_create_request();
  request.display_id = 0;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::MissingDisplayId);
}

TEST(VirtualDisplayDriverControlProtocol, RejectsOutOfRangeMode) {
  auto request = valid_create_request();
  request.flags = 0x80000000u;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidFlags);

  request = valid_create_request();
  request.width = vdd::kMinWidth - 1;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidWidth);

  request = valid_create_request();
  request.height = vdd::kMaxHeight + 1;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidHeight);

  request = valid_create_request();
  request.physical_width_mm = vdd::kMaxPhysicalSizeMillimeters + 1;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidPhysicalSize);

  request = valid_create_request();
  request.refresh_rate_millihz = 0;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidRefreshRate);

  request = valid_create_request();
  request.width = vdd::kMaxWidth + 1;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidWidth);

  request = valid_create_request();
  request.width = 3840;
  request.height = 2160;
  request.refresh_rate_millihz = 1'000'000;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::None);

  request = valid_create_request();
  request.width = 7680;
  request.height = 4320;
  request.refresh_rate_millihz = 1'000'000;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::None);
}

TEST(VirtualDisplayDriverControlProtocol, RejectsReservedCreateFields) {
  auto request = valid_create_request();
  request.reserved = 1;

  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidReservedField);
}

TEST(VirtualDisplayDriverControlProtocol, RejectsBlankDisplayName) {
  auto request = valid_create_request();
  std::memset(request.display_name, ' ', sizeof(request.display_name));

  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidDisplayName);
}

TEST(VirtualDisplayDriverControlProtocol, RejectsUnsafeDisplayNames) {
  auto request = valid_create_request();
  std::memset(request.display_name, 'A', sizeof(request.display_name));
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidDisplayName);

  request = valid_create_request();
  std::memcpy(request.display_name, "Desk\nDisplay", 13);
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidDisplayName);
}

TEST(VirtualDisplayDriverControlProtocol, ValidatesLeaseRequests) {
  const vdd::LeaseRequest request {
    vdd::kApiNamespaceGuid,
    lease_id(1),
    0,
    0
  };
  const vdd::LeaseDisplayRequest display_request {
    vdd::kApiNamespaceGuid,
    lease_id(1),
    2
  };

  EXPECT_EQ(vdd::validate_lease_request(request), vdd::ValidationError::None);
  EXPECT_EQ(vdd::validate_lease_display_request(display_request), vdd::ValidationError::None);
}

TEST(VirtualDisplayDriverControlProtocol, RejectsInvalidLeaseRequests) {
  auto request = vdd::LeaseRequest {
    vdd::kApiNamespaceGuid,
    lease_id(1),
    0,
    0
  };
  request.api_namespace.data1 ^= 1;
  EXPECT_EQ(vdd::validate_lease_request(request), vdd::ValidationError::WrongApiNamespace);

  request = vdd::LeaseRequest {
    vdd::kApiNamespaceGuid,
    0,
    0,
    0
  };
  EXPECT_EQ(vdd::validate_lease_request(request), vdd::ValidationError::MissingLeaseId);

  request = vdd::LeaseRequest {
    vdd::kApiNamespaceGuid,
    lease_id(1),
    0,
    1
  };
  EXPECT_EQ(vdd::validate_lease_request(request), vdd::ValidationError::InvalidReservedField);

  auto display_request = vdd::LeaseDisplayRequest {
    vdd::kApiNamespaceGuid,
    lease_id(1),
    2
  };
  display_request.api_namespace.data1 ^= 1;
  EXPECT_EQ(vdd::validate_lease_display_request(display_request), vdd::ValidationError::WrongApiNamespace);

  display_request = vdd::LeaseDisplayRequest {
    vdd::kApiNamespaceGuid,
    0,
    2
  };
  EXPECT_EQ(vdd::validate_lease_display_request(display_request), vdd::ValidationError::MissingLeaseId);

  display_request = vdd::LeaseDisplayRequest {
    vdd::kApiNamespaceGuid,
    lease_id(1),
    0
  };
  EXPECT_EQ(vdd::validate_lease_display_request(display_request), vdd::ValidationError::MissingDisplayId);
}

TEST(VirtualDisplayDriverControlProtocol, ValidatesPermanentDisplayCount) {
  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 2;

  EXPECT_EQ(vdd::validate_permanent_display_count(request, 2), vdd::ValidationError::None);

  request.display_count = 3;
  EXPECT_EQ(
    vdd::validate_permanent_display_count(request, 2),
    vdd::ValidationError::PermanentDisplayCountTooHigh
  );

  request = {};
  request.flags = 0x80000000u;
  EXPECT_EQ(
    vdd::validate_permanent_display_count(request, 2),
    vdd::ValidationError::InvalidFlags
  );

  request = {};
  request.api_namespace.data1 ^= 1;
  EXPECT_EQ(
    vdd::validate_permanent_display_count(request, 2),
    vdd::ValidationError::WrongApiNamespace
  );
}

TEST(VirtualDisplayDriverControlProtocol, ValidatesPermanentDisplaySettings) {
  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 1;
  request.width = 3840;
  request.height = 2160;
  request.physical_width_mm = 700;
  request.physical_height_mm = 390;
  request.refresh_rate_millihz = 144'000;
  std::memcpy(request.display_name, "Desk Display", 13);

  EXPECT_EQ(vdd::validate_permanent_display_count(request, 4), vdd::ValidationError::None);

  request.width = vdd::kMinWidth - 1;
  EXPECT_EQ(vdd::validate_permanent_display_count(request, 4), vdd::ValidationError::InvalidWidth);

  request.width = 3840;
  request.physical_height_mm = vdd::kMinPhysicalSizeMillimeters - 1;
  EXPECT_EQ(vdd::validate_permanent_display_count(request, 4), vdd::ValidationError::InvalidPhysicalSize);

  request.physical_height_mm = 390;
  request.refresh_rate_millihz = 0;
  EXPECT_EQ(vdd::validate_permanent_display_count(request, 4), vdd::ValidationError::InvalidRefreshRate);

  request.refresh_rate_millihz = 144'000;
  std::memset(request.display_name, ' ', sizeof(request.display_name));
  EXPECT_EQ(vdd::validate_permanent_display_count(request, 4), vdd::ValidationError::InvalidDisplayName);
}

TEST(VirtualDisplayDriverControlProtocol, ValidatesDisplayManifest) {
  auto manifest = valid_display_manifest();

  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::None);

  manifest.version = vdd::kDisplayManifestVersion + 1;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidManifestVersion);

  manifest = valid_display_manifest();
  manifest.reserved = 1;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidReservedField);

  manifest = valid_display_manifest();
  manifest.profiles[0].flags &= ~vdd::kDisplayManifestProfileFlagPermanentIdentity;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidFlags);

  manifest = valid_display_manifest();
  std::memcpy(manifest.profiles[0].manufacturer_id, "sdd", 4);
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidManufacturerId);

  manifest = valid_display_manifest();
  manifest.profiles[0].product_code = 0x1'0000u;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidProductCode);

  manifest = valid_display_manifest();
  manifest.profiles[0].connector_index = 2;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidConnectorIndex);

  manifest = valid_display_manifest();
  manifest.profiles[0].allowed_mode_count = 0;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidModeCount);

  manifest = valid_display_manifest();
  manifest.profiles[0].allowed_modes[0].refresh_rate_millihz = 0;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidRefreshRate);

  manifest = valid_display_manifest();
  manifest.profiles[0].allowed_modes[0] = {3840, 2160, 1'000'000};
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::None);

  manifest = valid_display_manifest();
  manifest.profiles[0].allowed_modes[0] = {vdd::kMaxWidth + 1, 2160, 60'000};
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidWidth);

  manifest = valid_display_manifest();
  std::memcpy(manifest.profiles[0].display_name, "Desk\rDisplay", 13);
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidDisplayName);

  manifest = valid_display_manifest();
  manifest.profiles[0].layout_policy = vdd::kDisplayManifestLayoutPolicyApplyAndPersist + 1;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidLayoutPolicy);

  manifest = valid_display_manifest();
  manifest.profiles[0].orientation = vdd::kDisplayManifestOrientationDefault + 1;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidOrientation);
}

TEST(VirtualDisplayDriverControlProtocol, RejectsDuplicateManifestIdentities) {
  auto manifest = valid_display_manifest();
  manifest.profile_count = 2;
  manifest.max_profile_count = 2;
  manifest.profiles[1] = manifest.profiles[0];
  set_manifest_profile_identity(manifest.profiles[1], 1);

  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::None);

  auto duplicate = manifest;
  duplicate.profiles[1].display_id = duplicate.profiles[0].display_id;
  EXPECT_EQ(vdd::validate_display_manifest(duplicate, 2), vdd::ValidationError::DuplicateManifestIdentity);

  duplicate = manifest;
  duplicate.profiles[1].container_id = duplicate.profiles[0].container_id;
  EXPECT_EQ(vdd::validate_display_manifest(duplicate, 2), vdd::ValidationError::DuplicateManifestIdentity);

  duplicate = manifest;
  duplicate.profiles[1].product_code = duplicate.profiles[0].product_code;
  EXPECT_EQ(vdd::validate_display_manifest(duplicate, 2), vdd::ValidationError::DuplicateManifestIdentity);

  duplicate = manifest;
  duplicate.profiles[1].serial_number = duplicate.profiles[0].serial_number;
  EXPECT_EQ(vdd::validate_display_manifest(duplicate, 2), vdd::ValidationError::DuplicateManifestIdentity);
}

TEST(VirtualDisplayDriverControlProtocol, RejectsMissingManifestIdentityFields) {
  auto manifest = valid_display_manifest();
  manifest.profiles[0].container_id = {};
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidContainerId);

  manifest = valid_display_manifest();
  manifest.profiles[0].product_code = 0;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidProductCode);

  manifest = valid_display_manifest();
  manifest.profiles[0].serial_number = 0;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidSerialNumber);

  EXPECT_STREQ(vdd::to_string(vdd::ValidationError::InvalidContainerId), "invalid_container_id");
  EXPECT_STREQ(vdd::to_string(vdd::ValidationError::InvalidSerialNumber), "invalid_serial_number");
}
