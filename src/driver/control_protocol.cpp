#include "virtual_display/driver/control_protocol.h"

#include <algorithm>

namespace virtual_display::driver {
  namespace {
    constexpr std::uint64_t kMaxModePixels = 7680ull * 4320ull;

    bool valid_manufacturer_id(const char (&manufacturer_id)[4]) {
      return manufacturer_id[0] >= 'A' && manufacturer_id[0] <= 'Z' &&
             manufacturer_id[1] >= 'A' && manufacturer_id[1] <= 'Z' &&
             manufacturer_id[2] >= 'A' && manufacturer_id[2] <= 'Z' &&
             manufacturer_id[3] == '\0';
    }

    bool is_zero_guid(const Guid &guid) {
      return guid == Guid {};
    }

    ValidationError validate_display_mode(
      const std::uint32_t width,
      const std::uint32_t height,
      const std::uint32_t refresh_rate_millihz
    ) {
      if (width < kMinWidth || width > kMaxWidth) {
        return ValidationError::InvalidWidth;
      }
      if (height < kMinHeight || height > kMaxHeight) {
        return ValidationError::InvalidHeight;
      }
      if (refresh_rate_millihz < kMinRefreshRateMilliHz) {
        return ValidationError::InvalidRefreshRate;
      }

      const auto pixels = static_cast<std::uint64_t>(width) * height;
      if (pixels == 0 || pixels > kMaxModePixels) {
        return ValidationError::InvalidWidth;
      }

      return ValidationError::None;
    }
  }  // namespace

  bool is_valid_api_namespace(const Guid &guid) {
    return guid == kApiNamespaceGuid;
  }

  void set_default_permanent_display_settings(PermanentDisplayCountRequest &request) {
    if (request.width == 0) {
      request.width = 1920;
    }
    if (request.height == 0) {
      request.height = 1080;
    }
    if (request.physical_width_mm == 0) {
      request.physical_width_mm = kDefaultPhysicalWidthMillimeters;
    }
    if (request.physical_height_mm == 0) {
      request.physical_height_mm = kDefaultPhysicalHeightMillimeters;
    }
    if (request.refresh_rate_millihz == 0) {
      request.refresh_rate_millihz = 60'000;
    }
    if (trim_display_name(request.display_name).empty()) {
      std::fill(std::begin(request.display_name), std::end(request.display_name), '\0');
      constexpr char kDefaultName[] = "Sunshine Display";
      std::copy_n(kDefaultName, sizeof(kDefaultName) - 1, request.display_name);
      return;
    }

    char canonical[kDisplayNameChars] {};
    if (canonicalize_display_name(request.display_name, canonical)) {
      std::copy(std::begin(canonical), std::end(canonical), std::begin(request.display_name));
    }
  }

  std::uint32_t normalize_timeout_ms(const std::uint32_t requested_timeout_ms) {
    if (requested_timeout_ms == 0) {
      return kDefaultLeaseTimeoutMs;
    }

    return std::clamp(requested_timeout_ms, kMinLeaseTimeoutMs, kMaxLeaseTimeoutMs);
  }

  std::string_view trim_display_name(const char (&display_name)[kDisplayNameChars]) {
    const auto end = std::find(std::begin(display_name), std::end(display_name), '\0');
    const auto raw_size = static_cast<std::size_t>(std::distance(std::begin(display_name), end));
    std::string_view value {display_name, raw_size};

    while (!value.empty() && value.back() == ' ') {
      value.remove_suffix(1);
    }

    return value;
  }

  bool canonicalize_display_name(
    const char (&input)[kDisplayNameChars],
    char (&output)[kDisplayNameChars]
  ) {
    const auto end = std::find(std::begin(input), std::end(input), '\0');
    auto size = static_cast<std::size_t>(std::distance(std::begin(input), end));
    while (size > 0 && input[size - 1] == ' ') {
      --size;
    }

    if (size == 0 || size >= kDisplayNameChars) {
      return false;
    }

    for (std::size_t index = 0; index < size; ++index) {
      const auto ch = static_cast<unsigned char>(input[index]);
      if (ch < 0x20 || ch > 0x7e) {
        return false;
      }
    }

    std::fill(std::begin(output), std::end(output), '\0');
    std::copy_n(input, size, output);
    return true;
  }

  ValidationError validate_create_temporary_display(
    const CreateTemporaryDisplayRequest &request,
    ValidatedCreateTemporaryDisplay *validated
  ) {
    if (!is_valid_api_namespace(request.api_namespace)) {
      return ValidationError::WrongApiNamespace;
    }
    if (request.lease_id < kMinOpaqueLeaseId) {
      return ValidationError::MissingLeaseId;
    }
    if (request.display_id == 0) {
      return ValidationError::MissingDisplayId;
    }
    if ((request.flags & ~kCreateTemporaryDisplayKnownFlags) != 0) {
      return ValidationError::InvalidFlags;
    }
    if (const auto mode_validation = validate_display_mode(
          request.width,
          request.height,
          request.refresh_rate_millihz
        );
        mode_validation != ValidationError::None) {
      return mode_validation;
    }
    const auto physical_width_mm = request.physical_width_mm == 0 ?
      kDefaultPhysicalWidthMillimeters :
      request.physical_width_mm;
    const auto physical_height_mm = request.physical_height_mm == 0 ?
      kDefaultPhysicalHeightMillimeters :
      request.physical_height_mm;
    if (physical_width_mm < kMinPhysicalSizeMillimeters ||
        physical_width_mm > kMaxPhysicalSizeMillimeters ||
        physical_height_mm < kMinPhysicalSizeMillimeters ||
        physical_height_mm > kMaxPhysicalSizeMillimeters) {
      return ValidationError::InvalidPhysicalSize;
    }
    const auto hdr_max_luminance_nits = request.hdr_max_luminance_nits == 0 ?
      kDefaultHdrMaxLuminanceNits :
      request.hdr_max_luminance_nits;
    if (hdr_max_luminance_nits < kMinHdrMaxLuminanceNits ||
        hdr_max_luminance_nits > kMaxHdrMaxLuminanceNits) {
      return ValidationError::InvalidHdrLuminance;
    }
    char canonical_display_name[kDisplayNameChars] {};
    if (!canonicalize_display_name(request.display_name, canonical_display_name)) {
      return ValidationError::InvalidDisplayName;
    }

    if (validated) {
      validated->request = request;
      validated->request.physical_width_mm = physical_width_mm;
      validated->request.physical_height_mm = physical_height_mm;
      validated->request.hdr_max_luminance_nits = hdr_max_luminance_nits;
      std::copy(
        std::begin(canonical_display_name),
        std::end(canonical_display_name),
        std::begin(validated->request.display_name)
      );
      validated->effective_timeout_ms = normalize_timeout_ms(request.requested_timeout_ms);
      validated->rebind_display_name(trim_display_name(validated->request.display_name).size());
    }

    return ValidationError::None;
  }

  ValidationError validate_create_temporary_display_owned(
    const CreateTemporaryDisplayOwnedRequest &request,
    ValidatedCreateTemporaryDisplay *validated
  ) {
    if (const auto validation = validate_create_temporary_display(request.display, validated);
        validation != ValidationError::None) {
      return validation;
    }

    if (std::all_of(
          request.owner_capability.bytes.begin(),
          request.owner_capability.bytes.end(),
          [](const std::uint8_t byte) {
            return byte == 0;
          }
        )) {
      return ValidationError::MissingOwnerCapability;
    }

    return ValidationError::None;
  }

  ValidationError validate_reclaim_temporary_display(const ReclaimTemporaryDisplayRequest &request) {
    if (!is_valid_api_namespace(request.api_namespace)) {
      return ValidationError::WrongApiNamespace;
    }
    if (request.new_lease_id < kMinOpaqueLeaseId) {
      return ValidationError::MissingLeaseId;
    }
    if (request.display_id == 0) {
      return ValidationError::MissingDisplayId;
    }
    if (request.reserved != 0) {
      return ValidationError::InvalidReservedField;
    }
    if (std::all_of(
          request.owner_capability.bytes.begin(),
          request.owner_capability.bytes.end(),
          [](const std::uint8_t byte) {
            return byte == 0;
          }
        )) {
      return ValidationError::MissingOwnerCapability;
    }

    return ValidationError::None;
  }

  ValidationError validate_lease_display_request(const LeaseDisplayRequest &request) {
    if (!is_valid_api_namespace(request.api_namespace)) {
      return ValidationError::WrongApiNamespace;
    }
    if (request.lease_id < kMinOpaqueLeaseId) {
      return ValidationError::MissingLeaseId;
    }
    if (request.display_id == 0) {
      return ValidationError::MissingDisplayId;
    }

    return ValidationError::None;
  }

  ValidationError validate_lease_request(const LeaseRequest &request) {
    if (!is_valid_api_namespace(request.api_namespace)) {
      return ValidationError::WrongApiNamespace;
    }
    if (request.reserved != 0) {
      return ValidationError::InvalidReservedField;
    }
    if (request.lease_id < kMinOpaqueLeaseId) {
      return ValidationError::MissingLeaseId;
    }

    return ValidationError::None;
  }

  ValidationError validate_permanent_display_count(
    const PermanentDisplayCountRequest &request,
    const std::uint32_t max_display_count
  ) {
    if (!is_valid_api_namespace(request.api_namespace)) {
      return ValidationError::WrongApiNamespace;
    }
    if ((request.flags & ~kPermanentDisplayCountKnownFlags) != 0) {
      return ValidationError::InvalidFlags;
    }
    if (request.display_count > max_display_count) {
      return ValidationError::PermanentDisplayCountTooHigh;
    }
    if (const auto mode_validation = validate_display_mode(
          request.width,
          request.height,
          request.refresh_rate_millihz
        );
        mode_validation != ValidationError::None) {
      return mode_validation;
    }
    if (request.physical_width_mm < kMinPhysicalSizeMillimeters ||
        request.physical_width_mm > kMaxPhysicalSizeMillimeters ||
        request.physical_height_mm < kMinPhysicalSizeMillimeters ||
        request.physical_height_mm > kMaxPhysicalSizeMillimeters) {
      return ValidationError::InvalidPhysicalSize;
    }
    char canonical_display_name[kDisplayNameChars] {};
    if (!canonicalize_display_name(request.display_name, canonical_display_name)) {
      return ValidationError::InvalidDisplayName;
    }

    return ValidationError::None;
  }

  ValidationError validate_display_manifest(
    const DisplayManifest &manifest,
    const std::uint32_t max_display_count
  ) {
    if (!is_valid_api_namespace(manifest.api_namespace)) {
      return ValidationError::WrongApiNamespace;
    }
    if (manifest.reserved != 0) {
      return ValidationError::InvalidReservedField;
    }
    if (manifest.version != kDisplayManifestVersion) {
      return ValidationError::InvalidManifestVersion;
    }
    if (manifest.profile_count > max_display_count ||
        manifest.profile_count > kMaxPermanentDisplayProfiles) {
      return ValidationError::PermanentDisplayCountTooHigh;
    }
    if (manifest.max_profile_count < manifest.profile_count ||
        manifest.max_profile_count > max_display_count ||
        manifest.max_profile_count > kMaxPermanentDisplayProfiles) {
      return ValidationError::PermanentDisplayCountTooHigh;
    }

    std::array<bool, kMaxPermanentDisplayProfiles> used_connectors {};
    std::array<std::uint64_t, kMaxPermanentDisplayProfiles> used_display_ids {};
    std::array<Guid, kMaxPermanentDisplayProfiles> used_container_ids {};
    std::array<std::uint32_t, kMaxPermanentDisplayProfiles> used_product_codes {};
    std::array<std::uint32_t, kMaxPermanentDisplayProfiles> used_serial_numbers {};
    for (std::uint32_t index = 0; index < manifest.profile_count; ++index) {
      const auto &profile = manifest.profiles[index];
      if ((profile.flags & ~kDisplayManifestProfileKnownFlags) != 0) {
        return ValidationError::InvalidFlags;
      }
      if ((profile.flags & kDisplayManifestProfileFlagPermanentIdentity) == 0) {
        return ValidationError::InvalidFlags;
      }
      if (profile.connector_index >= max_display_count ||
          profile.connector_index >= kMaxPermanentDisplayProfiles ||
          used_connectors[profile.connector_index]) {
        return ValidationError::InvalidConnectorIndex;
      }
      used_connectors[profile.connector_index] = true;

      if (profile.display_id == 0) {
        return ValidationError::MissingDisplayId;
      }
      if (is_zero_guid(profile.container_id)) {
        return ValidationError::InvalidContainerId;
      }
      if (profile.product_code == 0) {
        return ValidationError::InvalidProductCode;
      }
      if (profile.serial_number == 0) {
        return ValidationError::InvalidSerialNumber;
      }
      if (profile.product_code > 0xffffu) {
        return ValidationError::InvalidProductCode;
      }
      for (std::uint32_t previous = 0; previous < index; ++previous) {
        if (used_display_ids[previous] == profile.display_id ||
            used_container_ids[previous] == profile.container_id ||
            used_product_codes[previous] == profile.product_code ||
            used_serial_numbers[previous] == profile.serial_number) {
          return ValidationError::DuplicateManifestIdentity;
        }
      }
      used_display_ids[index] = profile.display_id;
      used_container_ids[index] = profile.container_id;
      used_product_codes[index] = profile.product_code;
      used_serial_numbers[index] = profile.serial_number;
      if (!valid_manufacturer_id(profile.manufacturer_id)) {
        return ValidationError::InvalidManufacturerId;
      }
      if (profile.physical_width_mm < kMinPhysicalSizeMillimeters ||
          profile.physical_width_mm > kMaxPhysicalSizeMillimeters ||
          profile.physical_height_mm < kMinPhysicalSizeMillimeters ||
          profile.physical_height_mm > kMaxPhysicalSizeMillimeters) {
        return ValidationError::InvalidPhysicalSize;
      }
      if (profile.allowed_mode_count == 0 ||
          profile.allowed_mode_count > kMaxAllowedModesPerProfile ||
          profile.native_mode_index >= profile.allowed_mode_count) {
        return ValidationError::InvalidModeCount;
      }
      if (profile.layout_policy > kDisplayManifestLayoutPolicyApplyAndPersist) {
        return ValidationError::InvalidLayoutPolicy;
      }
      if (profile.orientation != kDisplayManifestOrientationDefault) {
        return ValidationError::InvalidOrientation;
      }

      for (std::uint32_t mode_index = 0; mode_index < profile.allowed_mode_count; ++mode_index) {
        const auto &mode = profile.allowed_modes[mode_index];
        if (const auto mode_validation = validate_display_mode(
              mode.width,
              mode.height,
              mode.refresh_rate_millihz
            );
            mode_validation != ValidationError::None) {
          return mode_validation;
        }
      }

      char canonical_display_name[kDisplayNameChars] {};
      if (!canonicalize_display_name(profile.display_name, canonical_display_name)) {
        return ValidationError::InvalidDisplayName;
      }
    }

    return ValidationError::None;
  }

  const char *to_string(const ValidationError error) {
    switch (error) {
      case ValidationError::None:
        return "none";
      case ValidationError::WrongApiNamespace:
        return "wrong_api_namespace";
      case ValidationError::MissingLeaseId:
        return "missing_lease_id";
      case ValidationError::MissingDisplayId:
        return "missing_display_id";
      case ValidationError::MissingOwnerCapability:
        return "missing_owner_capability";
      case ValidationError::InvalidFlags:
        return "invalid_flags";
      case ValidationError::InvalidWidth:
        return "invalid_width";
      case ValidationError::InvalidHeight:
        return "invalid_height";
      case ValidationError::InvalidPhysicalSize:
        return "invalid_physical_size";
      case ValidationError::InvalidHdrLuminance:
        return "invalid_hdr_luminance";
      case ValidationError::InvalidRefreshRate:
        return "invalid_refresh_rate";
      case ValidationError::InvalidDisplayName:
        return "invalid_display_name";
      case ValidationError::PermanentDisplayCountTooHigh:
        return "permanent_display_count_too_high";
      case ValidationError::InvalidManifestVersion:
        return "invalid_manifest_version";
      case ValidationError::InvalidConnectorIndex:
        return "invalid_connector_index";
      case ValidationError::InvalidModeCount:
        return "invalid_mode_count";
      case ValidationError::InvalidLayoutPolicy:
        return "invalid_layout_policy";
      case ValidationError::InvalidOrientation:
        return "invalid_orientation";
      case ValidationError::InvalidManufacturerId:
        return "invalid_manufacturer_id";
      case ValidationError::InvalidProductCode:
        return "invalid_product_code";
      case ValidationError::DuplicateManifestIdentity:
        return "duplicate_manifest_identity";
      case ValidationError::InvalidReservedField:
        return "invalid_reserved_field";
      case ValidationError::InvalidContainerId:
        return "invalid_container_id";
      case ValidationError::InvalidSerialNumber:
        return "invalid_serial_number";
    }

    return "unknown";
  }
}  // namespace virtual_display::driver
