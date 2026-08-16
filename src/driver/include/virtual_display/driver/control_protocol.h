#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace virtual_display::driver {
  struct Guid {
    std::uint32_t data1 {};
    std::uint16_t data2 {};
    std::uint16_t data3 {};
    std::array<std::uint8_t, 8> data4 {};

    friend constexpr bool operator==(const Guid &lhs, const Guid &rhs) = default;
  };

  inline constexpr Guid kDeviceInterfaceGuid {
    0x5f894d6c,
    0x3a69,
    0x48a2,
    {0x86, 0xef, 0xe4, 0xc6, 0x71, 0x93, 0x2d, 0x63}
  };

  // Remote-session IddCx devices are enumerated once per WTS session. Keep
  // their control interface distinct from the root/console device so a
  // machine-level broker cannot accidentally mutate the console display while
  // opening a seat device (or vice versa).
  inline constexpr Guid kRemoteDeviceInterfaceGuid {
    0x2e82b34d,
    0x53b8,
    0x49c2,
    {0x9d, 0x8d, 0x13, 0x46, 0x79, 0x91, 0xc4, 0x87}
  };

  inline constexpr Guid kApiNamespaceGuid {
    0xa2864284,
    0x77fe,
    0x4336,
    {0xa8, 0x28, 0x00, 0xfe, 0xec, 0x89, 0xeb, 0xac}
  };

  inline constexpr std::uint16_t kProtocolVersionMajor = 3;
  inline constexpr std::uint16_t kProtocolVersionMinor = 9;
  inline constexpr std::uint16_t kProtocolVersionPatch = 0;
  inline constexpr std::uint16_t kMinimumCompatibleProtocolVersionMinor = 6;

  inline constexpr std::uint32_t kDisplayNameChars = 32;
  inline constexpr std::uint32_t kDefaultLeaseTimeoutMs = 10'000;
  inline constexpr std::uint32_t kMinLeaseTimeoutMs = 3'000;
  inline constexpr std::uint32_t kMaxLeaseTimeoutMs = 300'000;
  inline constexpr std::uint64_t kMinOpaqueLeaseId = 0x6000000000000000ull;
  inline constexpr std::size_t kOwnerCapabilityBytes = 32;

  inline constexpr std::uint32_t kMinWidth = 320;
  inline constexpr std::uint32_t kMinHeight = 200;
  inline constexpr std::uint32_t kMaxWidth = 7680;
  inline constexpr std::uint32_t kMaxHeight = 4320;
  inline constexpr std::uint32_t kMinRefreshRateMilliHz = 1;
  inline constexpr std::uint32_t kMaxRefreshRateMilliHz = (std::numeric_limits<std::uint32_t>::max)();
  inline constexpr std::uint32_t kDefaultPhysicalWidthMillimeters = 600;
  inline constexpr std::uint32_t kDefaultPhysicalHeightMillimeters = 340;
  inline constexpr std::uint32_t kMinPhysicalSizeMillimeters = 10;
  inline constexpr std::uint32_t kMaxPhysicalSizeMillimeters = 2550;
  inline constexpr std::uint32_t kDefaultHdrMaxLuminanceNits = 1000;
  inline constexpr std::uint32_t kMinHdrMaxLuminanceNits = 400;
  inline constexpr std::uint32_t kMaxHdrMaxLuminanceNits = 2000;
  inline constexpr std::uint32_t kDefaultSdrWhiteLevelNits = 80;
  inline constexpr std::uint32_t kMinSdrWhiteLevelNits = 80;
  inline constexpr std::uint32_t kMaxSdrWhiteLevelNits = 480;
  inline constexpr std::uint32_t kCreateTemporaryDisplayFlagEphemeralIdentity = 0x00000001u;
  // Proof-only remote-session experiment: request the first remote display
  // configuration as HDR before the monitor is reported present to IddCx.
  // The console IDD deliberately ignores this flag.
  inline constexpr std::uint32_t kCreateTemporaryDisplayFlagInitialRemoteHdr = 0x00000002u;
  inline constexpr std::uint32_t kCreateTemporaryDisplayKnownFlags =
    kCreateTemporaryDisplayFlagEphemeralIdentity |
    kCreateTemporaryDisplayFlagInitialRemoteHdr;
  inline constexpr std::uint32_t kPermanentDisplayCountKnownFlags = 0;
  inline constexpr std::uint32_t kMaxDisplayStateEntries = 16;
  inline constexpr std::uint32_t kDisplayStateKindPermanent = 1;
  inline constexpr std::uint32_t kDisplayStateKindTemporary = 2;
  inline constexpr std::uint32_t kDisplayStateFlagHdrSupported = 0x00000001u;
  inline constexpr std::uint32_t kDisplayStateFlagRetainIdentity = 0x00000002u;
  inline constexpr std::uint32_t kDisplayStateFlagHdrEnabled = 0x00000004u;
  inline constexpr std::uint32_t kDisplayManifestVersion = 3;
  inline constexpr std::uint32_t kMaxPermanentDisplayProfiles = 8;
  inline constexpr std::uint32_t kMaxAllowedModesPerProfile = 4;
  inline constexpr std::uint32_t kDisplayManifestProfileFlagHdrSupported = 0x00000001u;
  inline constexpr std::uint32_t kDisplayManifestProfileFlagRetainIdentity = 0x00000002u;
  inline constexpr std::uint32_t kDisplayManifestProfileFlagPermanentIdentity = 0x00000004u;
  // Proof-only: initialize a remote session's first display-config path as HDR.
  // This is meaningful only to the remote IDD and remains off in normal manifests.
  inline constexpr std::uint32_t kDisplayManifestProfileFlagInitialRemoteHdr = 0x00000008u;
  inline constexpr std::uint32_t kDisplayManifestProfileKnownFlags =
    kDisplayManifestProfileFlagHdrSupported |
    kDisplayManifestProfileFlagRetainIdentity |
    kDisplayManifestProfileFlagPermanentIdentity |
    kDisplayManifestProfileFlagInitialRemoteHdr;
  inline constexpr std::uint32_t kDisplayManifestLayoutPolicyNone = 0;
  inline constexpr std::uint32_t kDisplayManifestLayoutPolicyApply = 1;
  inline constexpr std::uint32_t kDisplayManifestLayoutPolicyApplyAndPersist = 2;
  inline constexpr std::uint32_t kDisplayManifestOrientationDefault = 0;

  enum class IoctlFunction : std::uint32_t {
    GetProtocolVersion = 0x900,
    CreateTemporaryDisplay = 0x901,
    RemoveTemporaryDisplay = 0x902,
    FeedLease = 0x903,
    ReleaseLease = 0x904,
    QueryLease = 0x905,
    SetPermanentDisplayCount = 0x906,
    QueryPermanentDisplayCount = 0x907,
    QueryDisplayState = 0x908,
    SetDisplayManifest = 0x909,
    QueryDisplayManifest = 0x90a,
    SetRenderAdapter = 0x90b,
    CreateTemporaryDisplayOwned = 0x90c,
    ReclaimTemporaryDisplay = 0x90d,
    SetDisplayHdrState = 0x90e,
    SetDisplayMode = 0x90f,
  };

  enum class IoctlAccess : std::uint32_t {
    Any = 0,
    Read = 1,
    Write = 2,
    ReadWrite = 3,
  };

  inline constexpr std::uint32_t ioctl_code(IoctlFunction function, IoctlAccess access) {
    constexpr std::uint32_t file_device_unknown = 0x00000022;
    constexpr std::uint32_t method_buffered = 0;
    return (file_device_unknown << 16) |
           (static_cast<std::uint32_t>(access) << 14) |
           (static_cast<std::uint32_t>(function) << 2) |
           method_buffered;
  }

  inline constexpr std::uint32_t kIoctlGetProtocolVersion =
    ioctl_code(IoctlFunction::GetProtocolVersion, IoctlAccess::Any);
  inline constexpr std::uint32_t kIoctlCreateTemporaryDisplay =
    ioctl_code(IoctlFunction::CreateTemporaryDisplay, IoctlAccess::ReadWrite);
  inline constexpr std::uint32_t kIoctlRemoveTemporaryDisplay =
    ioctl_code(IoctlFunction::RemoveTemporaryDisplay, IoctlAccess::ReadWrite);
  inline constexpr std::uint32_t kIoctlFeedLease =
    ioctl_code(IoctlFunction::FeedLease, IoctlAccess::ReadWrite);
  inline constexpr std::uint32_t kIoctlReleaseLease =
    ioctl_code(IoctlFunction::ReleaseLease, IoctlAccess::ReadWrite);
  inline constexpr std::uint32_t kIoctlQueryLease =
    ioctl_code(IoctlFunction::QueryLease, IoctlAccess::Read);
  inline constexpr std::uint32_t kIoctlSetPermanentDisplayCount =
    ioctl_code(IoctlFunction::SetPermanentDisplayCount, IoctlAccess::ReadWrite);
  inline constexpr std::uint32_t kIoctlQueryPermanentDisplayCount =
    ioctl_code(IoctlFunction::QueryPermanentDisplayCount, IoctlAccess::Read);
  inline constexpr std::uint32_t kIoctlQueryDisplayState =
    ioctl_code(IoctlFunction::QueryDisplayState, IoctlAccess::Read);
  inline constexpr std::uint32_t kIoctlSetDisplayManifest =
    ioctl_code(IoctlFunction::SetDisplayManifest, IoctlAccess::ReadWrite);
  inline constexpr std::uint32_t kIoctlQueryDisplayManifest =
    ioctl_code(IoctlFunction::QueryDisplayManifest, IoctlAccess::Read);
  inline constexpr std::uint32_t kIoctlSetRenderAdapter =
    ioctl_code(IoctlFunction::SetRenderAdapter, IoctlAccess::Write);
  inline constexpr std::uint32_t kIoctlCreateTemporaryDisplayOwned =
    ioctl_code(IoctlFunction::CreateTemporaryDisplayOwned, IoctlAccess::ReadWrite);
  inline constexpr std::uint32_t kIoctlReclaimTemporaryDisplay =
    ioctl_code(IoctlFunction::ReclaimTemporaryDisplay, IoctlAccess::ReadWrite);
  inline constexpr std::uint32_t kIoctlSetDisplayHdrState =
    ioctl_code(IoctlFunction::SetDisplayHdrState, IoctlAccess::ReadWrite);
  inline constexpr std::uint32_t kIoctlSetDisplayMode =
    ioctl_code(IoctlFunction::SetDisplayMode, IoctlAccess::ReadWrite);

  struct ProtocolVersion {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint16_t major {kProtocolVersionMajor};
    std::uint16_t minor {kProtocolVersionMinor};
    std::uint16_t patch {kProtocolVersionPatch};
    std::uint16_t reserved {};
  };

  struct AdapterLuid {
    std::uint32_t low_part {};
    std::int32_t high_part {};

    friend constexpr bool operator==(const AdapterLuid &lhs, const AdapterLuid &rhs) = default;
  };

  struct CreateTemporaryDisplayRequest {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint64_t lease_id {};
    std::uint64_t display_id {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t physical_width_mm {};
    std::uint32_t physical_height_mm {};
    std::uint32_t refresh_rate_millihz {};
    std::uint32_t requested_timeout_ms {};
    char display_name[kDisplayNameChars] {};
    std::uint32_t flags {};
    // Zero preserves compatibility with protocol 3.5 clients and selects the default.
    std::uint32_t hdr_max_luminance_nits {};
  };

  struct CreateTemporaryDisplayResult {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint64_t lease_id {};
    std::uint64_t display_id {};
    AdapterLuid os_adapter_luid {};
    std::uint32_t target_id {};
    std::uint32_t connector_index {};
    std::uint32_t effective_timeout_ms {};
    std::uint32_t reserved {};
  };

  struct SetRenderAdapterRequest {
    Guid api_namespace {kApiNamespaceGuid};
    AdapterLuid adapter_luid {};
    std::uint32_t flags {};
    std::uint32_t reserved {};
  };

  struct SetDisplayHdrStateRequest {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint64_t display_id {};
    std::uint32_t enabled {};
    std::uint32_t sdr_white_level_nits {kDefaultSdrWhiteLevelNits};
    std::uint32_t flags {};
    std::uint32_t reserved {};
  };

  struct SetDisplayModeRequest {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint64_t display_id {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t refresh_rate_millihz {};
    std::uint32_t flags {};
    std::uint32_t reserved {};
  };

  struct OwnerCapability {
    std::array<std::uint8_t, kOwnerCapabilityBytes> bytes {};

    friend constexpr bool operator==(const OwnerCapability &lhs, const OwnerCapability &rhs) = default;
  };

  struct CreateTemporaryDisplayOwnedRequest {
    CreateTemporaryDisplayRequest display {};
    OwnerCapability owner_capability {};
  };

  struct ReclaimTemporaryDisplayRequest {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint64_t display_id {};
    std::uint64_t new_lease_id {};
    std::uint32_t requested_timeout_ms {};
    std::uint32_t reserved {};
    OwnerCapability owner_capability {};
  };

  struct ReclaimTemporaryDisplayResult {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint64_t lease_id {};
    std::uint64_t display_id {};
    std::uint32_t connector_index {};
    std::uint32_t effective_timeout_ms {};
    std::uint32_t temporary_display_count {};
    std::uint32_t reserved {};
  };

  struct LeaseDisplayRequest {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint64_t lease_id {};
    std::uint64_t display_id {};
  };

  struct LeaseRequest {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint64_t lease_id {};
    std::uint32_t requested_timeout_ms {};
    std::uint32_t reserved {};
  };

  struct QueryLeaseResult {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint64_t lease_id {};
    std::uint32_t temporary_display_count {};
    std::uint32_t effective_timeout_ms {};
    std::uint32_t remaining_ms {};
    std::uint32_t lease_exists {};
    std::uint32_t pending_departure_count {};
  };

  struct PermanentDisplayCountRequest {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint32_t display_count {};
    std::uint32_t flags {};
    std::uint32_t width {1920};
    std::uint32_t height {1080};
    std::uint32_t physical_width_mm {kDefaultPhysicalWidthMillimeters};
    std::uint32_t physical_height_mm {kDefaultPhysicalHeightMillimeters};
    std::uint32_t refresh_rate_millihz {60'000};
    char display_name[kDisplayNameChars] {"Sunshine Display"};
  };

  struct PermanentDisplayCountResult {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint32_t current_display_count {};
    std::uint32_t max_display_count {};
    std::uint32_t temporary_display_count {};
    std::uint32_t width {1920};
    std::uint32_t height {1080};
    std::uint32_t physical_width_mm {kDefaultPhysicalWidthMillimeters};
    std::uint32_t physical_height_mm {kDefaultPhysicalHeightMillimeters};
    std::uint32_t refresh_rate_millihz {60'000};
    char display_name[kDisplayNameChars] {"Sunshine Display"};
  };

  struct DisplayStateEntry {
    std::uint32_t kind {};
    std::uint32_t flags {};
    std::uint64_t lease_id {};
    std::uint64_t display_id {};
    Guid container_id {};
    std::uint32_t connector_index {};
    std::uint32_t product_code {};
    std::uint32_t serial_number {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t physical_width_mm {};
    std::uint32_t physical_height_mm {};
    std::uint32_t refresh_rate_millihz {};
    char display_name[kDisplayNameChars] {};
  };

  struct QueryDisplayStateResult {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint32_t permanent_display_count {};
    std::uint32_t temporary_display_count {};
    std::uint32_t entry_count {};
    std::uint32_t reserved {};
    DisplayStateEntry entries[kMaxDisplayStateEntries] {};
  };

  struct DisplayMode {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t refresh_rate_millihz {};
  };

  struct DisplayManifestProfile {
    std::uint32_t flags {};
    std::uint32_t connector_index {};
    std::uint64_t display_id {};
    Guid container_id {};
    char manufacturer_id[4] {"SDD"};
    std::uint32_t product_code {};
    std::uint32_t serial_number {};
    std::uint32_t physical_width_mm {};
    std::uint32_t physical_height_mm {};
    std::uint32_t native_mode_index {};
    std::uint32_t allowed_mode_count {};
    std::uint32_t layout_policy {};
    std::int32_t position_x {};
    std::int32_t position_y {};
    std::uint32_t orientation {};
    DisplayMode allowed_modes[kMaxAllowedModesPerProfile] {};
    char display_name[kDisplayNameChars] {};
  };

  struct DisplayManifest {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint32_t version {kDisplayManifestVersion};
    std::uint32_t profile_count {};
    std::uint32_t max_profile_count {};
    std::uint32_t reserved {};
    DisplayManifestProfile profiles[kMaxPermanentDisplayProfiles] {};
  };

  enum class ValidationError {
    None,
    WrongApiNamespace,
    MissingLeaseId,
    MissingDisplayId,
    MissingOwnerCapability,
    InvalidFlags,
    InvalidWidth,
    InvalidHeight,
    InvalidPhysicalSize,
    InvalidHdrLuminance,
    InvalidHdrState,
    InvalidSdrWhiteLevel,
    InvalidRefreshRate,
    InvalidDisplayName,
    PermanentDisplayCountTooHigh,
    InvalidManifestVersion,
    InvalidConnectorIndex,
    InvalidModeCount,
    InvalidLayoutPolicy,
    InvalidOrientation,
    InvalidManufacturerId,
    InvalidProductCode,
    DuplicateManifestIdentity,
    InvalidReservedField,
    InvalidContainerId,
    InvalidSerialNumber,
  };

  struct ValidatedCreateTemporaryDisplay {
    CreateTemporaryDisplayRequest request {};
    std::uint32_t effective_timeout_ms {};
    std::string_view display_name {};

    ValidatedCreateTemporaryDisplay() = default;
    ValidatedCreateTemporaryDisplay(const ValidatedCreateTemporaryDisplay &other):
        request {other.request},
        effective_timeout_ms {other.effective_timeout_ms} {
      rebind_display_name(other.display_name.size());
    }
    ValidatedCreateTemporaryDisplay &operator=(const ValidatedCreateTemporaryDisplay &other) {
      if (this != &other) {
        request = other.request;
        effective_timeout_ms = other.effective_timeout_ms;
        rebind_display_name(other.display_name.size());
      }
      return *this;
    }
    ValidatedCreateTemporaryDisplay(ValidatedCreateTemporaryDisplay &&other) noexcept:
        request {other.request},
        effective_timeout_ms {other.effective_timeout_ms} {
      rebind_display_name(other.display_name.size());
    }
    ValidatedCreateTemporaryDisplay &operator=(ValidatedCreateTemporaryDisplay &&other) noexcept {
      if (this != &other) {
        request = other.request;
        effective_timeout_ms = other.effective_timeout_ms;
        rebind_display_name(other.display_name.size());
      }
      return *this;
    }

    void rebind_display_name(std::size_t size) {
      display_name = std::string_view {
        request.display_name,
        std::min(size, static_cast<std::size_t>(kDisplayNameChars))
      };
    }
  };

  bool is_valid_api_namespace(const Guid &guid);
  void set_default_permanent_display_settings(PermanentDisplayCountRequest &request);
  std::uint32_t normalize_timeout_ms(std::uint32_t requested_timeout_ms);
  std::string_view trim_display_name(const char (&display_name)[kDisplayNameChars]);
  bool canonicalize_display_name(
    const char (&input)[kDisplayNameChars],
    char (&output)[kDisplayNameChars]
  );
  ValidationError validate_create_temporary_display(
    const CreateTemporaryDisplayRequest &request,
    ValidatedCreateTemporaryDisplay *validated = nullptr
  );
  ValidationError validate_create_temporary_display_owned(
    const CreateTemporaryDisplayOwnedRequest &request,
    ValidatedCreateTemporaryDisplay *validated = nullptr
  );
  ValidationError validate_reclaim_temporary_display(const ReclaimTemporaryDisplayRequest &request);
  ValidationError validate_set_display_hdr_state(const SetDisplayHdrStateRequest &request);
  ValidationError validate_set_display_mode(const SetDisplayModeRequest &request);
  ValidationError validate_lease_display_request(const LeaseDisplayRequest &request);
  ValidationError validate_lease_request(const LeaseRequest &request);
  ValidationError validate_permanent_display_count(
    const PermanentDisplayCountRequest &request,
    std::uint32_t max_display_count
  );
  ValidationError validate_display_manifest(
    const DisplayManifest &manifest,
    std::uint32_t max_display_count
  );
  const char *to_string(ValidationError error);

  static_assert(std::is_standard_layout_v<Guid>);
  static_assert(sizeof(Guid) == 16);
  static_assert(std::is_standard_layout_v<AdapterLuid>);
  static_assert(sizeof(AdapterLuid) == 8);
  static_assert(sizeof(ProtocolVersion) == 24);
  static_assert(sizeof(CreateTemporaryDisplayRequest) == 96);
  static_assert(sizeof(CreateTemporaryDisplayResult) == 56);
  static_assert(std::is_standard_layout_v<OwnerCapability>);
  static_assert(sizeof(OwnerCapability) == kOwnerCapabilityBytes);
  static_assert(sizeof(CreateTemporaryDisplayOwnedRequest) == 128);
  static_assert(sizeof(ReclaimTemporaryDisplayRequest) == 72);
  static_assert(sizeof(ReclaimTemporaryDisplayResult) == 48);
  static_assert(sizeof(SetRenderAdapterRequest) == 32);
  static_assert(sizeof(SetDisplayHdrStateRequest) == 40);
  static_assert(sizeof(SetDisplayModeRequest) == 48);
  static_assert(sizeof(LeaseDisplayRequest) == 32);
  static_assert(sizeof(LeaseRequest) == 32);
  static_assert(sizeof(QueryLeaseResult) == 48);
  static_assert(sizeof(PermanentDisplayCountRequest) == 76);
  static_assert(sizeof(PermanentDisplayCountResult) == 80);
  static_assert(sizeof(DisplayStateEntry) == 104);
  static_assert(sizeof(QueryDisplayStateResult) == 1696);
  static_assert(sizeof(DisplayMode) == 12);
  static_assert(sizeof(DisplayManifestProfile) == 160);
  static_assert(sizeof(DisplayManifest) == 1312);
}  // namespace virtual_display::driver
