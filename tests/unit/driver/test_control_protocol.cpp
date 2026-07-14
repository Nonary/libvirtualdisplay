#include <gtest/gtest.h>
#include "virtual_display/driver/control_protocol.h"
#include "virtual_display/driver/device_identity.h"
#include "virtual_display/driver/display_identity.h"
#include "virtual_display/driver/windows_control_protocol.h"

#include <cstring>
#include <set>
#include <string>
#include <string_view>

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

TEST(VirtualDisplayDriverControlProtocol, ProtocolVersionStringFormatsConstants) {
  // Behavioral replacement for the test that scraped README.md for the version prose: it
  // asserts the formatter that is the single source of truth for the dotted version, rather
  // than that a string happens to appear in a checked-in document.
  EXPECT_EQ(vdd::protocol_version_string(), "3.6.0");
}

TEST(VirtualDisplayDriverControlProtocol, WindowsGuidAdapterPreservesProtocolGuid) {
#ifdef _WIN32
  const auto win_guid = vdd::to_windows_guid(vdd::kApiNamespaceGuid);

  EXPECT_EQ(vdd::from_windows_guid(win_guid), vdd::kApiNamespaceGuid);
#endif
}

TEST(VirtualDisplayDriverDeviceIdentity, FormatsControlInterfaceGuidAsInfBraceString) {
  EXPECT_EQ(
    vdd::format_inf_guid(vdd::kDeviceInterfaceGuid),
    "{5f894d6c-3a69-48a2-86ef-e4c671932d63}"
  );

  // Field grouping: the clock-seq bytes (data4[0..1]) follow the third hyphen and the node
  // bytes (data4[2..7]) the fourth, all lower-cased and zero-padded.
  EXPECT_EQ(
    vdd::format_inf_guid(
      vdd::Guid {0x01020304, 0x0506, 0x0708, {0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10}}
    ),
    "{01020304-0506-0708-090a-0b0c0d0e0f10}"
  );
}

TEST(VirtualDisplayDriverDeviceIdentity, DerivesBrokerServiceSidFromServiceName) {
  // The control device is openable only by SYSTEM and the broker service. Deriving the SID
  // from the service NAME (Windows' own per-service SID algorithm) instead of pinning a magic
  // literal means a broker rename is caught here and the generated INF grant stays correct.
  EXPECT_EQ(
    vdd::derive_service_sid(vdd::kBrokerServiceName),
    "S-1-5-80-2333729190-1599198784-3320592948-2337414441-3098439965"
  );

  // Windows derives service SIDs from the upper-cased name, so derivation is case-insensitive.
  EXPECT_EQ(
    vdd::derive_service_sid("sunshinevirtualdisplaybroker"),
    vdd::derive_service_sid(vdd::kBrokerServiceName)
  );

  // A different service name must yield a different principal.
  EXPECT_NE(
    vdd::derive_service_sid("SomeOtherService"),
    vdd::derive_service_sid(vdd::kBrokerServiceName)
  );
}

TEST(VirtualDisplayDriverDeviceIdentity, BuildsProtectedSystemAndBrokerOnlySecurityDescriptor) {
  const auto sddl = vdd::control_interface_security_descriptor();

  // Behavioral replacement for the two tests that scraped the INF for this SDDL. The same
  // descriptor feeds both INF Security lines through gen_driver_inf, and the committed INF is
  // verified against it by the driver_inf_in_sync ctest -- so the producer is asserted here.
  EXPECT_EQ(
    sddl,
    "D:P(A;;GA;;;SY)(A;;GA;;;S-1-5-80-2333729190-1599198784-3320592948-2337414441-3098439965)"
  );

  // Composed from named principals (not a frozen blob): a protected DACL granting GENERIC_ALL
  // to LocalSystem and the broker service only.
  EXPECT_EQ(
    sddl,
    "D:P(A;;GA;;;SY)(A;;GA;;;" + vdd::derive_service_sid(vdd::kBrokerServiceName) + ")"
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
  EXPECT_EQ(validated.request.hdr_max_luminance_nits, vdd::kDefaultHdrMaxLuminanceNits);
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

TEST(VirtualDisplayDriverControlProtocol, ValidatesCreateHdrLuminance) {
  auto request = valid_create_request();
  request.hdr_max_luminance_nits = vdd::kMaxHdrMaxLuminanceNits;
  vdd::ValidatedCreateTemporaryDisplay validated {};

  ASSERT_EQ(vdd::validate_create_temporary_display(request, &validated), vdd::ValidationError::None);
  EXPECT_EQ(validated.request.hdr_max_luminance_nits, vdd::kMaxHdrMaxLuminanceNits);

  request.hdr_max_luminance_nits = vdd::kMinHdrMaxLuminanceNits - 1;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidHdrLuminance);

  request.hdr_max_luminance_nits = vdd::kMaxHdrMaxLuminanceNits + 1;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidHdrLuminance);
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
}

TEST(VirtualDisplayDriverControlProtocol, ValidationErrorNamesAreExhaustiveAndDistinct) {
  // Replaces per-arm to_string() literal mirrors with one structural guarantee: every
  // ValidationError maps to a unique, non-fallback diagnostic name. This catches a missing
  // switch arm (the real defect) without re-pinning each string the implementation returns.
  std::set<std::string_view> seen;
  for (int value = 0; value <= static_cast<int>(vdd::ValidationError::InvalidSerialNumber); ++value) {
    const auto *name = vdd::to_string(static_cast<vdd::ValidationError>(value));
    EXPECT_STRNE(name, "unknown") << "missing to_string arm for ValidationError " << value;
    EXPECT_TRUE(seen.insert(name).second) << "duplicate ValidationError name: " << name;
  }
}
