#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include "virtual_display/driver/ioctl_dispatcher.h"
#include "virtual_display/driver/hdr_capabilities.h"
#include "virtual_display/driver/lease_store.h"
#include "virtual_display/driver/windows_control_protocol.h"

#include <Windows.h>
#include <avrt.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <TraceLoggingProvider.h>
#include <wdf.h>
#include <IddCx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vdd = virtual_display::driver;

TRACELOGGING_DEFINE_PROVIDER(
  g_trace_provider,
  "Sunshine.VirtualDisplayDriver",
  (0x3d5d3bd9, 0x8500, 0x4523, 0x93, 0x34, 0x58, 0x3f, 0x4b, 0x5e, 0x6f, 0x80)
);

#define WPP_CONTROL_GUIDS \
  WPP_DEFINE_CONTROL_GUID( \
    SunshineVirtualDisplayDriverWpp, \
    (b0dcb744, 045b, 463b, 9c2f, 6a3c897d3458), \
    WPP_DEFINE_BIT(TRACE_DRIVER) \
    WPP_DEFINE_BIT(TRACE_DEVICE) \
    WPP_DEFINE_BIT(TRACE_SWAPCHAIN))

#define WPP_LEVEL_FLAGS_LOGGER(level, flags) WPP_LEVEL_LOGGER(flags)
#define WPP_LEVEL_FLAGS_ENABLED(level, flags) \
  (WPP_LEVEL_ENABLED(flags) && WPP_CONTROL(WPP_BIT_##flags).Level >= level)

// begin_wpp config
// FUNC TraceEvents(LEVEL, FLAGS, MSG, ...);
// end_wpp
#include "driver_main.tmh"

namespace {
  constexpr std::uint32_t kMaxPermanentDisplays = 4;
  constexpr std::uint32_t kMaxTemporaryDisplays = 8;
  constexpr std::uint32_t kPersistentStateSchemaVersion = 1;
  constexpr wchar_t kSwapchainMmcssTask[] = L"DisplayPostProcessing";
  constexpr wchar_t kTemporaryDisplayProfilesValue[] = L"TemporaryDisplayProfiles";
  constexpr std::size_t kTemporaryDisplayProfileBytes = 36;
  constexpr std::size_t kTemporaryDisplayProfilesHeaderBytes = 8;
  constexpr std::size_t kTemporaryDisplayProfilesMaxBytes =
    kTemporaryDisplayProfilesHeaderBytes + kTemporaryDisplayProfileBytes * kMaxTemporaryDisplays;
  const GUID kControlInterfaceGuid = vdd::to_windows_guid(vdd::kDeviceInterfaceGuid);

  class IddCxBackend;
  class SwapChainProcessor;

  struct AdapterContext {
    IddCxBackend *backend {};
  };

  struct MonitorContext {
    IddCxBackend *backend {};
    std::uint64_t display_id {};
  };

  struct DeviceContext {
    class DeviceState *state {};
  };

  WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AdapterContext, GetAdapterContext);
  WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorContext, GetMonitorContext);
  WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetDeviceContext);

  struct MonitorRecord {
    vdd::DisplayDescriptor descriptor {};
    IDDCX_MONITOR monitor {};
    std::unique_ptr<SwapChainProcessor> swapchain_processor {};
    std::vector<std::unique_ptr<SwapChainProcessor>> retired_swapchain_processors {};
    bool permanent {};
    bool departing {};
    std::uint32_t assign_callbacks_in_flight {};
    IDDCX_DEFAULT_HDR_METADATA_TYPE default_hdr_metadata_type {IDDCX_HDRMETADATA_TYPE_UNINITIALIZED};
    UINT default_hdr_metadata_size {};
    IDDCX_GAMMARAMP_TYPE gamma_ramp_type {IDDCX_GAMMARAMP_TYPE_UNINITIALIZED};
    UINT gamma_ramp_size {};
  };

  struct ModeShape {
    std::uint32_t width {1920};
    std::uint32_t height {1080};
    // Use standard 1080p CVT/CTA-ish totals for fallback paths. Requested
    // per-monitor modes are normalized to active-only timings below for
    // high-refresh target-mode reporting.
    std::uint32_t total_width {2200};
    std::uint32_t total_height {1125};
    std::uint64_t pixel_rate {148'500'000ull};
    std::uint32_t refresh_rate_millihz {60'000};
  };

  struct ModeSpec {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t refresh_rate_millihz;
  };

  constexpr std::array<ModeSpec, 55> kDefaultModes {{
    {800, 600, 30'000},
    {800, 600, 59'940},
    {800, 600, 60'000},
    {800, 600, 72'000},
    {800, 600, 90'000},
    {800, 600, 120'000},
    {800, 600, 144'000},
    {800, 600, 240'000},
    {1280, 720, 30'000},
    {1280, 720, 59'940},
    {1280, 720, 60'000},
    {1280, 720, 72'000},
    {1280, 720, 90'000},
    {1280, 720, 120'000},
    {1280, 720, 144'000},
    {1366, 768, 30'000},
    {1366, 768, 59'940},
    {1366, 768, 60'000},
    {1366, 768, 72'000},
    {1366, 768, 90'000},
    {1366, 768, 120'000},
    {1366, 768, 144'000},
    {1366, 768, 240'000},
    {1920, 1080, 30'000},
    {1920, 1080, 59'940},
    {1920, 1080, 60'000},
    {1920, 1080, 72'000},
    {1920, 1080, 90'000},
    {1920, 1080, 120'000},
    {1920, 1080, 144'000},
    {1920, 1080, 240'000},
    {2560, 1440, 30'000},
    {2560, 1440, 59'940},
    {2560, 1440, 60'000},
    {2560, 1440, 72'000},
    {2560, 1440, 90'000},
    {2560, 1440, 120'000},
    {2560, 1440, 144'000},
    {2560, 1440, 240'000},
    {3840, 2160, 30'000},
    {3840, 2160, 59'940},
    {3840, 2160, 60'000},
    {3840, 2160, 72'000},
    {3840, 2160, 90'000},
    {3840, 2160, 120'000},
    {3840, 2160, 144'000},
    {3840, 2160, 240'000},
    {5120, 1440, 30'000},
    {5120, 1440, 59'940},
    {5120, 1440, 60'000},
    {5120, 1440, 120'000},
    {5120, 1440, 144'000},
    {5120, 1440, 175'000},
    {5120, 1440, 240'000},
    {5120, 1440, 480'000}
  }};

  std::uint32_t clamp_u32(const std::uint64_t value) {
    return static_cast<std::uint32_t>(
      (std::min<std::uint64_t>)(value, (std::numeric_limits<std::uint32_t>::max)())
    );
  }

  ModeShape active_mode_shape(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_rate_millihz
  ) {
    return {
      width,
      height,
      width,
      height,
      static_cast<std::uint64_t>(width) *
        static_cast<std::uint64_t>(height) *
        static_cast<std::uint64_t>((std::max)(refresh_rate_millihz, 1u)) /
        1000ull,
      refresh_rate_millihz
    };
  }

  ModeShape mode_shape_from_spec(const ModeSpec &spec) {
    return active_mode_shape(spec.width, spec.height, spec.refresh_rate_millihz);
  }

  std::uint32_t default_preferred_mode_index(const std::vector<ModeShape> &modes) {
    for (std::size_t index = 0; index < modes.size(); ++index) {
      const auto &mode = modes[index];
      if (mode.width == 1920 && mode.height == 1080 && mode.refresh_rate_millihz == 60'000) {
        return static_cast<std::uint32_t>(index);
      }
    }

    return 0;
  }

  std::pair<std::vector<ModeShape>, std::uint32_t> build_mode_shapes(
    const std::optional<ModeShape> &preferred
  ) {
    std::vector<ModeShape> modes;
    modes.reserve(kDefaultModes.size() + (preferred ? 1u : 0u));

    for (const auto &spec: kDefaultModes) {
      modes.push_back(mode_shape_from_spec(spec));
    }

    std::uint32_t preferred_index = default_preferred_mode_index(modes);
    if (preferred) {
      preferred_index = static_cast<std::uint32_t>(modes.size());
      modes.push_back(*preferred);
    }

    return {std::move(modes), preferred_index};
  }

  UNICODE_STRING unicode_string(const wchar_t *value) {
    UNICODE_STRING result {};
    RtlInitUnicodeString(&result, value);
    return result;
  }

  struct TemporaryDisplayProfile {
    std::uint64_t display_id {};
    std::uint32_t connector_index {};
    GUID container_id {};
    std::uint32_t edid_product_code {};
    std::uint32_t edid_serial_number {};
  };

  void append_u32(std::vector<std::uint8_t> &blob, const std::uint32_t value) {
    blob.push_back(static_cast<std::uint8_t>(value & 0xffu));
    blob.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
    blob.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
    blob.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
  }

  void append_u64(std::vector<std::uint8_t> &blob, const std::uint64_t value) {
    append_u32(blob, static_cast<std::uint32_t>(value & 0xffffffffull));
    append_u32(blob, static_cast<std::uint32_t>((value >> 32ull) & 0xffffffffull));
  }

  void append_guid(std::vector<std::uint8_t> &blob, const GUID &value) {
    append_u32(blob, value.Data1);
    blob.push_back(static_cast<std::uint8_t>(value.Data2 & 0xffu));
    blob.push_back(static_cast<std::uint8_t>((value.Data2 >> 8u) & 0xffu));
    blob.push_back(static_cast<std::uint8_t>(value.Data3 & 0xffu));
    blob.push_back(static_cast<std::uint8_t>((value.Data3 >> 8u) & 0xffu));
    blob.insert(blob.end(), std::begin(value.Data4), std::end(value.Data4));
  }

  bool has_bytes(const std::vector<std::uint8_t> &blob, const std::size_t offset, const std::size_t count) {
    return offset <= blob.size() && count <= blob.size() - offset;
  }

  bool read_u32(const std::vector<std::uint8_t> &blob, std::size_t &offset, std::uint32_t &value) {
    if (!has_bytes(blob, offset, sizeof(std::uint32_t))) {
      return false;
    }

    value =
      static_cast<std::uint32_t>(blob[offset]) |
      (static_cast<std::uint32_t>(blob[offset + 1]) << 8u) |
      (static_cast<std::uint32_t>(blob[offset + 2]) << 16u) |
      (static_cast<std::uint32_t>(blob[offset + 3]) << 24u);
    offset += sizeof(std::uint32_t);
    return true;
  }

  bool read_u64(const std::vector<std::uint8_t> &blob, std::size_t &offset, std::uint64_t &value) {
    std::uint32_t low {};
    std::uint32_t high {};
    if (!read_u32(blob, offset, low) || !read_u32(blob, offset, high)) {
      return false;
    }

    value = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32ull);
    return true;
  }

  bool read_guid(const std::vector<std::uint8_t> &blob, std::size_t &offset, GUID &value) {
    if (!has_bytes(blob, offset, sizeof(GUID))) {
      return false;
    }

    std::uint32_t data1 {};
    std::uint32_t data2 {};
    std::uint32_t data3 {};
    if (!read_u32(blob, offset, data1)) {
      return false;
    }
    if (!has_bytes(blob, offset, 12)) {
      return false;
    }

    data2 = static_cast<std::uint32_t>(blob[offset]) | (static_cast<std::uint32_t>(blob[offset + 1]) << 8u);
    offset += sizeof(std::uint16_t);
    data3 = static_cast<std::uint32_t>(blob[offset]) | (static_cast<std::uint32_t>(blob[offset + 1]) << 8u);
    offset += sizeof(std::uint16_t);

    value.Data1 = data1;
    value.Data2 = static_cast<unsigned short>(data2);
    value.Data3 = static_cast<unsigned short>(data3);
    std::copy_n(blob.begin() + static_cast<std::ptrdiff_t>(offset), std::size(value.Data4), std::begin(value.Data4));
    offset += std::size(value.Data4);
    return true;
  }

  class RegistryKey {
  public:
    RegistryKey() = default;
    explicit RegistryKey(WDFKEY key):
        key_ {key} {
    }
    RegistryKey(const RegistryKey &) = delete;
    RegistryKey &operator=(const RegistryKey &) = delete;
    RegistryKey(RegistryKey &&other) noexcept:
        key_ {std::exchange(other.key_, nullptr)} {
    }
    RegistryKey &operator=(RegistryKey &&other) noexcept {
      if (this != &other) {
        reset(std::exchange(other.key_, nullptr));
      }
      return *this;
    }
    ~RegistryKey() {
      reset();
    }

    WDFKEY get() const {
      return key_;
    }

    WDFKEY *put() {
      reset();
      return &key_;
    }

    void reset(WDFKEY key = nullptr) {
      if (key_) {
        WdfRegistryClose(key_);
      }
      key_ = key;
    }

  private:
    WDFKEY key_ {};
  };

  NTSTATUS open_driver_state_key(WDFDRIVER driver, WDFDEVICE device, ACCESS_MASK desired_access, RegistryKey &key) {
    if (!driver && !device) {
      return STATUS_INVALID_PARAMETER;
    }

    if (driver) {
      const auto status = WdfDriverOpenPersistentStateRegistryKey(driver, desired_access, WDF_NO_OBJECT_ATTRIBUTES, key.put());
      if (NT_SUCCESS(status)) {
        return status;
      }
    }

    if (!device) {
      return STATUS_INVALID_PARAMETER;
    }

    return WdfDeviceOpenRegistryKey(
      device,
      PLUGPLAY_REGKEY_DEVICE | WDF_REGKEY_DEVICE_SUBKEY,
      desired_access,
      WDF_NO_OBJECT_ATTRIBUTES,
      key.put()
    );
  }

  bool valid_temporary_profile(const std::uint64_t display_id, const std::uint32_t connector_index) {
    return display_id != 0 &&
           connector_index >= kMaxPermanentDisplays &&
           connector_index < kMaxPermanentDisplays + kMaxTemporaryDisplays;
  }

  std::vector<TemporaryDisplayProfile> load_temporary_display_profiles(WDFDRIVER driver, WDFDEVICE device) {
    std::vector<TemporaryDisplayProfile> profiles;
    RegistryKey state_key;
    if (!NT_SUCCESS(open_driver_state_key(driver, device, KEY_READ, state_key))) {
      return profiles;
    }

    std::vector<std::uint8_t> blob(kTemporaryDisplayProfilesMaxBytes);
    auto value_name = unicode_string(kTemporaryDisplayProfilesValue);
    ULONG value_length {};
    ULONG value_type {};
    const auto status = WdfRegistryQueryValue(
      state_key.get(),
      &value_name,
      static_cast<ULONG>(blob.size()),
      blob.data(),
      &value_length,
      &value_type
    );
    if (!NT_SUCCESS(status) || value_type != REG_BINARY || value_length < kTemporaryDisplayProfilesHeaderBytes ||
        value_length > blob.size()) {
      return profiles;
    }

    blob.resize(value_length);

    std::size_t offset {};
    std::uint32_t schema_version {};
    std::uint32_t profile_count {};
    if (!read_u32(blob, offset, schema_version) ||
        !read_u32(blob, offset, profile_count) ||
        schema_version != kPersistentStateSchemaVersion ||
        profile_count > kMaxTemporaryDisplays ||
        blob.size() != kTemporaryDisplayProfilesHeaderBytes + profile_count * kTemporaryDisplayProfileBytes) {
      return {};
    }

    profiles.reserve(profile_count);
    for (std::uint32_t index = 0; index < profile_count; ++index) {
      TemporaryDisplayProfile profile {};
      if (!read_u64(blob, offset, profile.display_id) ||
          !read_u32(blob, offset, profile.connector_index) ||
          !read_guid(blob, offset, profile.container_id) ||
          !read_u32(blob, offset, profile.edid_product_code) ||
          !read_u32(blob, offset, profile.edid_serial_number) ||
          !valid_temporary_profile(profile.display_id, profile.connector_index)) {
        return {};
      }

      profiles.push_back(profile);
    }

    return profiles;
  }

  std::map<std::uint64_t, std::uint32_t> load_temporary_connector_reservations(WDFDRIVER driver, WDFDEVICE device) {
    std::map<std::uint64_t, std::uint32_t> reservations;
    for (const auto &profile: load_temporary_display_profiles(driver, device)) {
      reservations.emplace(profile.display_id, profile.connector_index);
    }
    return reservations;
  }

  template <typename T>
  void write_registry_value_if_success(
    WDFKEY key,
    const wchar_t *value_name,
    const ULONG type,
    const T &value,
    NTSTATUS &status
  ) {
    if (NT_SUCCESS(status)) {
      auto name = unicode_string(value_name);
      auto value_copy = value;
      status = WdfRegistryAssignValue(key, &name, type, sizeof(value_copy), &value_copy);
    }
  }

  void write_registry_ulong_if_success(WDFKEY key, const wchar_t *value_name, const ULONG value, NTSTATUS &status) {
    if (NT_SUCCESS(status)) {
      auto name = unicode_string(value_name);
      status = WdfRegistryAssignULong(key, &name, value);
    }
  }

  vdd::BackendError save_temporary_display_profile(
    WDFDRIVER driver,
    WDFDEVICE device,
    const vdd::DisplayDescriptor &descriptor
  ) {
    if (!valid_temporary_profile(descriptor.display_id, descriptor.connector_index)) {
      return vdd::BackendError::Failed;
    }

    RegistryKey state_key;
    auto status = open_driver_state_key(driver, device, KEY_READ | KEY_SET_VALUE, state_key);
    if (!NT_SUCCESS(status)) {
      return vdd::BackendError::Failed;
    }

    auto profiles = load_temporary_display_profiles(driver, device);
    const TemporaryDisplayProfile profile {
      descriptor.display_id,
      descriptor.connector_index,
      vdd::to_windows_guid(descriptor.container_id),
      vdd::read_product_code(descriptor.edid),
      vdd::read_serial_number(descriptor.edid)
    };

    profiles.erase(
      std::remove_if(
        profiles.begin(),
        profiles.end(),
        [&](const TemporaryDisplayProfile &entry) {
          return entry.connector_index == descriptor.connector_index &&
                 entry.display_id != descriptor.display_id;
        }
      ),
      profiles.end()
    );

    const auto existing = std::find_if(
      profiles.begin(),
      profiles.end(),
      [&](const TemporaryDisplayProfile &entry) {
        return entry.display_id == descriptor.display_id;
      }
    );
    if (existing != profiles.end()) {
      *existing = profile;
    } else {
      if (profiles.size() >= kMaxTemporaryDisplays) {
        return vdd::BackendError::Failed;
      }
      profiles.push_back(profile);
    }

    std::vector<std::uint8_t> blob;
    blob.reserve(kTemporaryDisplayProfilesHeaderBytes + profiles.size() * kTemporaryDisplayProfileBytes);
    append_u32(blob, kPersistentStateSchemaVersion);
    append_u32(blob, static_cast<std::uint32_t>(profiles.size()));
    for (const auto &entry: profiles) {
      if (!valid_temporary_profile(entry.display_id, entry.connector_index)) {
        return vdd::BackendError::Failed;
      }
      append_u64(blob, entry.display_id);
      append_u32(blob, entry.connector_index);
      append_guid(blob, entry.container_id);
      append_u32(blob, entry.edid_product_code);
      append_u32(blob, entry.edid_serial_number);
    }

    auto value_name = unicode_string(kTemporaryDisplayProfilesValue);
    status = WdfRegistryAssignValue(
      state_key.get(),
      &value_name,
      REG_BINARY,
      static_cast<ULONG>(blob.size()),
      blob.data()
    );

    return NT_SUCCESS(status) ? vdd::BackendError::None : vdd::BackendError::Failed;
  }

  vdd::BackendError remove_temporary_display_profile(
    WDFDRIVER driver,
    WDFDEVICE device,
    const std::uint64_t display_id
  ) {
    RegistryKey state_key;
    auto status = open_driver_state_key(driver, device, KEY_READ | KEY_SET_VALUE, state_key);
    if (!NT_SUCCESS(status)) {
      return vdd::BackendError::Failed;
    }

    auto profiles = load_temporary_display_profiles(driver, device);
    const auto original_size = profiles.size();
    profiles.erase(
      std::remove_if(
        profiles.begin(),
        profiles.end(),
        [display_id](const TemporaryDisplayProfile &entry) {
          return entry.display_id == display_id;
        }
      ),
      profiles.end()
    );
    if (profiles.size() == original_size) {
      return vdd::BackendError::None;
    }

    std::vector<std::uint8_t> blob;
    blob.reserve(kTemporaryDisplayProfilesHeaderBytes + profiles.size() * kTemporaryDisplayProfileBytes);
    append_u32(blob, kPersistentStateSchemaVersion);
    append_u32(blob, static_cast<std::uint32_t>(profiles.size()));
    for (const auto &entry: profiles) {
      append_u64(blob, entry.display_id);
      append_u32(blob, entry.connector_index);
      append_guid(blob, entry.container_id);
      append_u32(blob, entry.edid_product_code);
      append_u32(blob, entry.edid_serial_number);
    }

    auto value_name = unicode_string(kTemporaryDisplayProfilesValue);
    status = WdfRegistryAssignValue(
      state_key.get(),
      &value_name,
      REG_BINARY,
      static_cast<ULONG>(blob.size()),
      blob.data()
    );

    return NT_SUCCESS(status) ? vdd::BackendError::None : vdd::BackendError::Failed;
  }

  bool has_hdr_iddcx_ddi() {
    return IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo);
  }

  bool runtime_hdr_supported() {
    return has_hdr_iddcx_ddi() &&
           vdd::supports_windows_hdr_toggle(vdd::hdr_output_capabilities());
  }

  vdd::DisplayDescriptor make_permanent_descriptor(
    const std::uint32_t index,
    const vdd::PermanentDisplayCountRequest &settings
  ) {
    const auto display_id = vdd::permanent_display_id(index);

    vdd::EdidOptions options {};
    options.manufacturer_id = vdd::kSunshineDriverManufacturerId;
    options.product_code = vdd::permanent_product_code(index);
    options.serial_number = vdd::serial_number_from_display_id(display_id);
    options.width = settings.width;
    options.height = settings.height;
    options.physical_width_mm = settings.physical_width_mm;
    options.physical_height_mm = settings.physical_height_mm;
    options.refresh_rate_millihz = settings.refresh_rate_millihz;
    options.monitor_name = vdd::trim_display_name(settings.display_name);
    options.hdr_supported = runtime_hdr_supported();

    vdd::DisplayDescriptor descriptor {};
    descriptor.display_id = display_id;
    descriptor.container_id = vdd::container_guid_from_display_id(display_id);
    descriptor.connector_index = index;
    descriptor.width = options.width;
    descriptor.height = options.height;
    descriptor.physical_width_mm = options.physical_width_mm;
    descriptor.physical_height_mm = options.physical_height_mm;
    descriptor.refresh_rate_millihz = options.refresh_rate_millihz;
    descriptor.edid = vdd::create_edid(options);
    return descriptor;
  }

  vdd::DisplayDescriptor make_permanent_descriptor(const vdd::DisplayManifestProfile &profile) {
    const auto &mode = profile.allowed_modes[profile.native_mode_index];

    vdd::EdidOptions options {};
    options.manufacturer_id = {
      profile.manufacturer_id[0],
      profile.manufacturer_id[1],
      profile.manufacturer_id[2]
    };
    options.product_code = profile.product_code > 0xffffu ?
      0xffffu :
      static_cast<std::uint16_t>(profile.product_code);
    options.serial_number = profile.serial_number;
    options.width = mode.width;
    options.height = mode.height;
    options.physical_width_mm = profile.physical_width_mm;
    options.physical_height_mm = profile.physical_height_mm;
    options.refresh_rate_millihz = mode.refresh_rate_millihz;
    options.monitor_name = vdd::trim_display_name(profile.display_name);
    options.hdr_supported =
      (profile.flags & vdd::kDisplayManifestProfileFlagHdrSupported) != 0 &&
      runtime_hdr_supported();

    vdd::DisplayDescriptor descriptor {};
    descriptor.display_id = profile.display_id;
    descriptor.container_id = profile.container_id;
    descriptor.connector_index = profile.connector_index;
    descriptor.width = options.width;
    descriptor.height = options.height;
    descriptor.physical_width_mm = options.physical_width_mm;
    descriptor.physical_height_mm = options.physical_height_mm;
    descriptor.refresh_rate_millihz = options.refresh_rate_millihz;
    descriptor.edid = vdd::create_edid(options);
    return descriptor;
  }

  std::optional<ModeShape> mode_shape_from_description(const IDDCX_MONITOR_DESCRIPTION &description) {
    if (description.Type != IDDCX_MONITOR_DESCRIPTION_TYPE_EDID ||
        !description.pData ||
        description.DataSize < vdd::kEdidSize) {
      return std::nullopt;
    }

    const auto *edid_data = static_cast<const std::byte *>(description.pData);
    const std::span<const std::byte, vdd::kEdidSize> edid {edid_data, vdd::kEdidSize};
    const auto timing = vdd::read_preferred_timing(edid);
    if (timing.horizontal_active == 0 || timing.vertical_active == 0 || timing.pixel_clock_10khz == 0) {
      return std::nullopt;
    }

    ModeShape shape {
      timing.horizontal_active,
      timing.vertical_active,
      static_cast<std::uint32_t>(timing.horizontal_active + timing.horizontal_blanking),
      static_cast<std::uint32_t>(timing.vertical_active + timing.vertical_blanking),
      static_cast<std::uint64_t>(timing.pixel_clock_10khz) * 10'000ull,
      60'000
    };

    const auto total_pixels =
      static_cast<std::uint64_t>((std::max)(shape.total_width, 1u)) *
      static_cast<std::uint64_t>((std::max)(shape.total_height, 1u));
    const auto derived_refresh_millihz =
      total_pixels == 0 ? 0 : (shape.pixel_rate * 1000ull) / total_pixels;
    shape.refresh_rate_millihz = clamp_u32(derived_refresh_millihz);

    return shape;
  }

  ModeShape mode_shape_from_descriptor(const vdd::DisplayDescriptor &descriptor) {
    if (descriptor.width == 0 || descriptor.height == 0 || descriptor.refresh_rate_millihz == 0) {
      return {};
    }

    return active_mode_shape(descriptor.width, descriptor.height, descriptor.refresh_rate_millihz);
  }

  std::optional<ModeShape> preferred_mode_shape_from_description(const IDDCX_MONITOR_DESCRIPTION &description) {
    return mode_shape_from_description(description);
  }

  IDDCX_BITS_PER_COMPONENT supported_hdr_bits_per_component() {
    const auto capabilities = vdd::hdr_output_capabilities();
    UINT bits = 0;
    if (capabilities.output_bits.rgb_8bpc) {
      bits |= static_cast<UINT>(IDDCX_BITS_PER_COMPONENT_8);
    }
    if (capabilities.output_bits.rgb_10bpc) {
      bits |= static_cast<UINT>(IDDCX_BITS_PER_COMPONENT_10);
    }
    return static_cast<IDDCX_BITS_PER_COMPONENT>(
      bits
    );
  }

  void populate_rgb_wire_bits(IDDCX_WIRE_BITS_PER_COMPONENT &bits, const IDDCX_BITS_PER_COMPONENT rgb_bits) {
    bits = {};
    bits.Rgb = rgb_bits;
    bits.YCbCr444 = IDDCX_BITS_PER_COMPONENT_NONE;
    bits.YCbCr422 = IDDCX_BITS_PER_COMPONENT_NONE;
    bits.YCbCr420 = IDDCX_BITS_PER_COMPONENT_NONE;
  }

  vdd::DisplayDescriptor descriptor_with_runtime_hdr_policy(const vdd::DisplayDescriptor &descriptor) {
    if (runtime_hdr_supported() || !vdd::has_hdr_static_metadata(descriptor.edid)) {
      return descriptor;
    }

    vdd::EdidOptions options {};
    options.manufacturer_id = vdd::read_manufacturer_id(descriptor.edid).value_or(vdd::kSunshineDriverManufacturerId);
    options.product_code = vdd::read_product_code(descriptor.edid);
    options.serial_number = vdd::read_serial_number(descriptor.edid);
    options.width = descriptor.width;
    options.height = descriptor.height;
    options.physical_width_mm = descriptor.physical_width_mm;
    options.physical_height_mm = descriptor.physical_height_mm;
    options.refresh_rate_millihz = descriptor.refresh_rate_millihz;
    const auto monitor_name = vdd::read_monitor_name(descriptor.edid);
    options.monitor_name = monitor_name.value_or("Sunshine Display");
    options.hdr_supported = false;

    auto downgraded = descriptor;
    downgraded.edid = vdd::create_edid(options);
    return downgraded;
  }

  DISPLAYCONFIG_VIDEO_SIGNAL_INFO make_signal_info(
    const ModeShape &shape,
    const bool monitor_mode
  ) {
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO signal {};
    signal.pixelRate = shape.pixel_rate;
    signal.activeSize.cx = shape.width;
    signal.activeSize.cy = shape.height;
    signal.totalSize.cx = (std::max)(shape.total_width, shape.width);
    signal.totalSize.cy = (std::max)(shape.total_height, shape.height);
    signal.vSyncFreq.Numerator = (std::max)(shape.refresh_rate_millihz, 1u);
    signal.vSyncFreq.Denominator = 1000;
    signal.hSyncFreq.Numerator = clamp_u32(
      static_cast<std::uint64_t>(signal.vSyncFreq.Numerator) *
      static_cast<std::uint64_t>((std::max)(signal.totalSize.cy, 1u))
    );
    signal.hSyncFreq.Denominator = signal.vSyncFreq.Denominator;
    // DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY_OTHER is not accepted here; 255 is
    // the documented "not initialized" value Windows itself uses for EDID modes.
    signal.AdditionalSignalInfo.videoStandard = 255;
    signal.AdditionalSignalInfo.vSyncFreqDivider = monitor_mode ? 0 : 1;
    signal.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    return signal;
  }

  IDDCX_MONITOR_MODE make_monitor_mode(
    const ModeShape &shape,
    const IDDCX_MONITOR_MODE_ORIGIN origin
  ) {
    IDDCX_MONITOR_MODE mode {};
    mode.Size = sizeof(mode);
    mode.Origin = origin;
    mode.MonitorVideoSignalInfo = make_signal_info(shape, true);
    return mode;
  }

  IDDCX_MONITOR_MODE2 make_monitor_mode2(
    const ModeShape &shape,
    const IDDCX_MONITOR_MODE_ORIGIN origin
  ) {
    IDDCX_MONITOR_MODE2 mode {};
    mode.Size = sizeof(mode);
    mode.Origin = origin;
    mode.MonitorVideoSignalInfo = make_signal_info(shape, true);
    populate_rgb_wire_bits(mode.BitsPerComponent, supported_hdr_bits_per_component());
    return mode;
  }

  IDDCX_TARGET_MODE make_target_mode(const ModeShape &shape) {
    IDDCX_TARGET_MODE mode {};
    mode.Size = sizeof(mode);
    mode.TargetVideoSignalInfo.targetVideoSignalInfo = make_signal_info(shape, false);
    return mode;
  }

  IDDCX_TARGET_MODE2 make_target_mode2(const ModeShape &shape) {
    IDDCX_TARGET_MODE2 mode {};
    mode.Size = sizeof(mode);
    mode.TargetVideoSignalInfo.targetVideoSignalInfo = make_signal_info(shape, false);
    populate_rgb_wire_bits(mode.BitsPerComponent, supported_hdr_bits_per_component());
    return mode;
  }

  NTSTATUS fill_monitor_modes(
    const IDARG_IN_PARSEMONITORDESCRIPTION *input,
    IDARG_OUT_PARSEMONITORDESCRIPTION *output
  ) {
    if (!input || !output) {
      return STATUS_INVALID_PARAMETER;
    }

    const auto [modes, preferred_index] = build_mode_shapes(preferred_mode_shape_from_description(input->MonitorDescription));
    output->MonitorModeBufferOutputCount = static_cast<UINT>(modes.size());
    output->PreferredMonitorModeIdx = preferred_index;
    if (input->MonitorModeBufferInputCount == 0) {
      return STATUS_SUCCESS;
    }
    if (!input->pMonitorModes || input->MonitorModeBufferInputCount < modes.size()) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    for (std::size_t index = 0; index < modes.size(); ++index) {
      input->pMonitorModes[index] = make_monitor_mode(modes[index], IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    }

    return STATUS_SUCCESS;
  }

  NTSTATUS fill_monitor_modes2(
    const IDARG_IN_PARSEMONITORDESCRIPTION2 *input,
    IDARG_OUT_PARSEMONITORDESCRIPTION *output
  ) {
    if (!input || !output) {
      return STATUS_INVALID_PARAMETER;
    }

    const auto [modes, preferred_index] = build_mode_shapes(preferred_mode_shape_from_description(input->MonitorDescription));
    output->MonitorModeBufferOutputCount = static_cast<UINT>(modes.size());
    output->PreferredMonitorModeIdx = preferred_index;
    if (input->MonitorModeBufferInputCount == 0) {
      return STATUS_SUCCESS;
    }
    if (!input->pMonitorModes || input->MonitorModeBufferInputCount < modes.size()) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    for (std::size_t index = 0; index < modes.size(); ++index) {
      input->pMonitorModes[index] = make_monitor_mode2(modes[index], IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    }

    return STATUS_SUCCESS;
  }

  NTSTATUS fill_default_monitor_modes(
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES *input,
    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES *output
  ) {
    if (!input || !output) {
      return STATUS_INVALID_PARAMETER;
    }

    const auto [modes, preferred_index] = build_mode_shapes(std::nullopt);
    output->DefaultMonitorModeBufferOutputCount = static_cast<UINT>(modes.size());
    output->PreferredMonitorModeIdx = preferred_index;
    if (input->DefaultMonitorModeBufferInputCount == 0) {
      return STATUS_SUCCESS;
    }
    if (!input->pDefaultMonitorModes || input->DefaultMonitorModeBufferInputCount < modes.size()) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    for (std::size_t index = 0; index < modes.size(); ++index) {
      input->pDefaultMonitorModes[index] = make_monitor_mode(modes[index], IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
    }

    return STATUS_SUCCESS;
  }

  NTSTATUS fill_target_modes(
    const IDARG_IN_QUERYTARGETMODES *input,
    IDARG_OUT_QUERYTARGETMODES *output,
    const ModeShape *requested_shape = nullptr
  ) {
    if (!input || !output) {
      return STATUS_INVALID_PARAMETER;
    }

    const auto [modes, preferred_index] = build_mode_shapes(
      requested_shape ? std::optional<ModeShape> {*requested_shape} : mode_shape_from_description(input->MonitorDescription)
    );
    (void) preferred_index;
    output->TargetModeBufferOutputCount = static_cast<UINT>(modes.size());
    if (input->TargetModeBufferInputCount == 0) {
      return STATUS_SUCCESS;
    }
    if (!input->pTargetModes || input->TargetModeBufferInputCount < modes.size()) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    for (std::size_t index = 0; index < modes.size(); ++index) {
      input->pTargetModes[index] = make_target_mode(modes[index]);
    }

    return STATUS_SUCCESS;
  }

  NTSTATUS fill_target_modes2(
    const IDARG_IN_QUERYTARGETMODES2 *input,
    IDARG_OUT_QUERYTARGETMODES *output,
    const ModeShape *requested_shape = nullptr
  ) {
    if (!input || !output) {
      return STATUS_INVALID_PARAMETER;
    }

    const auto [modes, preferred_index] = build_mode_shapes(
      requested_shape ? std::optional<ModeShape> {*requested_shape} : mode_shape_from_description(input->MonitorDescription)
    );
    (void) preferred_index;
    output->TargetModeBufferOutputCount = static_cast<UINT>(modes.size());
    if (input->TargetModeBufferInputCount == 0) {
      return STATUS_SUCCESS;
    }
    if (!input->pTargetModes || input->TargetModeBufferInputCount < modes.size()) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    for (std::size_t index = 0; index < modes.size(); ++index) {
      input->pTargetModes[index] = make_target_mode2(modes[index]);
    }

    return STATUS_SUCCESS;
  }

  HRESULT create_dxgi_device_for_luid(
    const LUID &adapter_luid,
    Microsoft::WRL::ComPtr<ID3D11Device> &device,
    Microsoft::WRL::ComPtr<IDXGIDevice> &dxgi_device
  ) {
    static constexpr D3D_FEATURE_LEVEL kFeatureLevels[] {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0
    };

    const auto create_device = [&](IDXGIAdapter *adapter, const D3D_DRIVER_TYPE driver_type) {
      D3D_FEATURE_LEVEL selected_feature_level {};
      Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
      device.Reset();
      dxgi_device.Reset();
      const HRESULT hr = D3D11CreateDevice(
        adapter,
        driver_type,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        kFeatureLevels,
        static_cast<UINT>(std::size(kFeatureLevels)),
        D3D11_SDK_VERSION,
        &device,
        &selected_feature_level,
        &context
      );
      if (FAILED(hr)) {
        device.Reset();
        return hr;
      }

      const HRESULT as_hr = device.As(&dxgi_device);
      if (FAILED(as_hr)) {
        dxgi_device.Reset();
        device.Reset();
      }
      return as_hr;
    };

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
      Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
      hr = factory->EnumAdapterByLuid(adapter_luid, IID_PPV_ARGS(&adapter));
      if (SUCCEEDED(hr)) {
        hr = create_device(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN);
        if (SUCCEEDED(hr)) {
          TraceLoggingWrite(g_trace_provider, "RenderDeviceCreated", TraceLoggingString("Hardware", "DriverType"));
          TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_SWAPCHAIN, "RenderDeviceCreated");
          return hr;
        }
      }
    }

    TraceLoggingWrite(
      g_trace_provider,
      "RenderDeviceFallback",
      TraceLoggingUInt32(static_cast<std::uint32_t>(hr), "AdapterHResult")
    );
    hr = create_device(nullptr, D3D_DRIVER_TYPE_WARP);
    if (FAILED(hr)) {
      TraceLoggingWrite(
        g_trace_provider,
        "RenderDeviceFallbackFailed",
        TraceLoggingUInt32(static_cast<std::uint32_t>(hr), "HResult")
      );
      return hr;
    }

    TraceLoggingWrite(g_trace_provider, "RenderDeviceCreated", TraceLoggingString("WARP", "DriverType"));
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_SWAPCHAIN, "RenderDeviceCreated");
    return S_OK;
  }

  bool is_device_lost_hresult(const HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED ||
           hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
  }

  class SwapChainProcessor {
  public:
    SwapChainProcessor(IDDCX_SWAPCHAIN swapchain, HANDLE next_surface_available):
        swapchain_ {swapchain},
        next_surface_available_ {next_surface_available} {
    }

    ~SwapChainProcessor() {
      stop();
      delete_swapchain();
    }

    SwapChainProcessor(const SwapChainProcessor &) = delete;
    SwapChainProcessor &operator=(const SwapChainProcessor &) = delete;

    HRESULT start(const LUID &render_adapter_luid) {
      try {
        worker_ = std::thread([this, render_adapter_luid]() {
          try {
            process_frames(render_adapter_luid);
          } catch (...) {
            publish_startup_result(E_FAIL);
          }
        });
      } catch (...) {
        return E_OUTOFMEMORY;
      }

      std::unique_lock lock {startup_mutex_};
      startup_ready_.wait(lock, [this]() {
        return startup_result_.has_value();
      });
      const auto result = *startup_result_;
      lock.unlock();

      if (FAILED(result)) {
        stop();
      }

      return result;
    }

    void stop() {
      stop_requested_.store(true, std::memory_order_release);
      if (next_surface_available_) {
        SetEvent(next_surface_available_);
      }
      if (worker_.joinable()) {
        worker_.join();
      }
      dxgi_device_.Reset();
      device_.Reset();
    }

    void abandon_swapchain() {
      swapchain_ = nullptr;
    }

  private:
    class MmcssRegistration {
    public:
      explicit MmcssRegistration(const wchar_t *task_name) {
        handle_ = AvSetMmThreadCharacteristicsW(task_name, &task_index_);
      }

      ~MmcssRegistration() {
        if (handle_) {
          AvRevertMmThreadCharacteristics(handle_);
        }
      }

      MmcssRegistration(const MmcssRegistration &) = delete;
      MmcssRegistration &operator=(const MmcssRegistration &) = delete;

    private:
      DWORD task_index_ {};
      HANDLE handle_ {};
    };

    void delete_swapchain() {
      if (swapchain_) {
        // Match the IddCx sample by closing the swapchain when processing
        // stops. Waiting until after monitor departure can leave us deleting
        // a UMDF object that IddCx has already invalidated.
        WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapchain_));
        swapchain_ = nullptr;
      }
    }

    HRESULT assign_swapchain_device() {
      if (!dxgi_device_) {
        return E_FAIL;
      }

      IDARG_IN_SWAPCHAINSETDEVICE set_device {};
      set_device.pDevice = dxgi_device_.Get();
      // HandleNewSwapChain still owns IddCx's internal OPM cleanup while it
      // invokes AssignSwapChain. Setting the DXGI device from the worker thread
      // matches the WDK sample flow and avoids re-entering that cleanup path.
      return IddCxSwapChainSetDevice(swapchain_, &set_device);
    }

    HRESULT reset_render_device(const LUID &render_adapter_luid) {
      dxgi_device_.Reset();
      device_.Reset();

      HRESULT hr = create_dxgi_device_for_luid(render_adapter_luid, device_, dxgi_device_);
      if (FAILED(hr)) {
        TraceLoggingWrite(
          g_trace_provider,
          "RenderDeviceCreateFailed",
          TraceLoggingUInt32(static_cast<std::uint32_t>(hr), "HResult")
        );
        return hr;
      }

      hr = assign_swapchain_device();
      if (FAILED(hr)) {
        TraceLoggingWrite(
          g_trace_provider,
          "SwapChainSetDeviceFailed",
          TraceLoggingUInt32(static_cast<std::uint32_t>(hr), "HResult")
        );
      }
      return hr;
    }

    void process_frames(const LUID render_adapter_luid) {
      MmcssRegistration mmcss {kSwapchainMmcssTask};

      HRESULT hr = reset_render_device(render_adapter_luid);
      publish_startup_result(hr);
      if (FAILED(hr) || stop_requested_.load(std::memory_order_acquire)) {
        return;
      }

      while (!stop_requested_.load(std::memory_order_acquire)) {
        const DWORD wait_result = WaitForSingleObject(next_surface_available_, 1000);
        if (stop_requested_.load(std::memory_order_acquire)) {
          break;
        }
        if (wait_result == WAIT_TIMEOUT) {
          continue;
        }
        if (wait_result != WAIT_OBJECT_0) {
          TraceLoggingWrite(
            g_trace_provider,
            "SwapChainWaitFailed",
            TraceLoggingUInt32(wait_result, "WaitResult"),
            TraceLoggingUInt32(GetLastError(), "LastError")
          );
          delete_swapchain();
          return;
        }

        for (;;) {
          IDXGIResource *surface_ptr = nullptr;
          HRESULT acquire_result = E_FAIL;
          if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
            IDARG_IN_RELEASEANDACQUIREBUFFER2 input {};
            input.Size = sizeof(input);
            IDARG_OUT_RELEASEANDACQUIREBUFFER2 acquired {};
            acquired.MetaData.Size = sizeof(acquired.MetaData);
            acquire_result = IddCxSwapChainReleaseAndAcquireBuffer2(swapchain_, &input, &acquired);
            surface_ptr = acquired.MetaData.pSurface;
          } else {
            IDARG_OUT_RELEASEANDACQUIREBUFFER acquired {};
            acquire_result = IddCxSwapChainReleaseAndAcquireBuffer(swapchain_, &acquired);
            surface_ptr = acquired.MetaData.pSurface;
          }
          if (acquire_result == E_PENDING) {
            break;
          }
          if (FAILED(acquire_result)) {
            if (is_device_lost_hresult(acquire_result)) {
              TraceLoggingWrite(
                g_trace_provider,
                "RenderDeviceLost",
                TraceLoggingUInt32(static_cast<std::uint32_t>(acquire_result), "HResult")
              );
              TraceEvents(TRACE_LEVEL_WARNING, TRACE_SWAPCHAIN, "RenderDeviceLost");
              if (FAILED(reset_render_device(render_adapter_luid))) {
                delete_swapchain();
                return;
              }
              continue;
            }
            TraceLoggingWrite(
              g_trace_provider,
              "SwapChainProcessingFailed",
              TraceLoggingUInt32(static_cast<std::uint32_t>(acquire_result), "HResult")
            );
            delete_swapchain();
            return;
          }

          Microsoft::WRL::ComPtr<IDXGIResource> surface;
          surface.Attach(surface_ptr);
          // Drop the acquired surface before reporting the frame complete so
          // IddCx can reclaim the buffer during unassign/departure.
          surface.Reset();
          const HRESULT finished_result = IddCxSwapChainFinishedProcessingFrame(swapchain_);
          if (FAILED(finished_result)) {
            TraceLoggingWrite(
              g_trace_provider,
              "SwapChainFinishedFrameFailed",
              TraceLoggingUInt32(static_cast<std::uint32_t>(finished_result), "HResult")
            );
            delete_swapchain();
            return;
          }

          if (stop_requested_.load(std::memory_order_acquire)) {
            break;
          }
        }
      }

    }

    void publish_startup_result(const HRESULT hr) {
      {
        std::lock_guard lock {startup_mutex_};
        if (startup_result_) {
          return;
        }
        startup_result_ = hr;
      }
      startup_ready_.notify_all();
    }

    IDDCX_SWAPCHAIN swapchain_ {};
    HANDLE next_surface_available_ {};
    std::atomic<bool> stop_requested_ {false};
    std::thread worker_ {};
    std::mutex startup_mutex_ {};
    std::condition_variable startup_ready_ {};
    std::optional<HRESULT> startup_result_ {};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device_;
  };

  void stop_swapchain_processor_without_delete(std::unique_ptr<SwapChainProcessor> &processor) {
    if (!processor) {
      return;
    }

    processor->stop();
    processor->abandon_swapchain();
    processor.reset();
  }

  void stop_swapchain_processors_without_delete(std::vector<std::unique_ptr<SwapChainProcessor>> &processors) {
    for (auto &processor: processors) {
      stop_swapchain_processor_without_delete(processor);
    }
    processors.clear();
  }

  class IddCxBackend: public vdd::DisplayDriverBackend {
  public:
    IddCxBackend(WDFDRIVER driver, WDFDEVICE device):
        driver_ {driver},
        device_ {device} {
    }

    NTSTATUS initialize_adapter(WDFDEVICE device) {
      {
        std::lock_guard lock {mutex_};
        if (adapter_ready_) {
          return STATUS_SUCCESS;
        }
        if (adapter_) {
          return NT_SUCCESS(adapter_init_status_) ? STATUS_DEVICE_NOT_READY : adapter_init_status_;
        }
      }

      IDDCX_ENDPOINT_VERSION endpoint_version {};
      endpoint_version.Size = sizeof(endpoint_version);
      endpoint_version.MajorVer = 1;
      endpoint_version.MinorVer = 0;

      const auto hdr_capabilities = vdd::hdr_output_capabilities();
      IDDCX_ADAPTER_CAPS caps {};
      caps.Size = sizeof(caps);
      caps.Flags = has_hdr_iddcx_ddi() && hdr_capabilities.fp16_swapchain ?
        IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16 :
        IDDCX_ADAPTER_FLAGS_NONE;
      caps.MaxMonitorsSupported = kMaxPermanentDisplays + kMaxTemporaryDisplays;
      caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
      // IddCx expects gamma support to be advertised when exposing high color
      // space; accepting SetGammaRamp below is enough for our software path.
      caps.EndPointDiagnostics.GammaSupport = hdr_capabilities.high_color_space ?
        IDDCX_FEATURE_IMPLEMENTATION_SOFTWARE :
        IDDCX_FEATURE_IMPLEMENTATION_NONE;
      caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
      caps.EndPointDiagnostics.pEndPointFriendlyName = const_cast<PWSTR>(L"Sunshine Virtual Display Adapter");
      caps.EndPointDiagnostics.pEndPointManufacturerName = const_cast<PWSTR>(L"Sunshine");
      caps.EndPointDiagnostics.pEndPointModelName = const_cast<PWSTR>(L"SunshineVirtualDisplay");
      caps.EndPointDiagnostics.pHardwareVersion = &endpoint_version;
      caps.EndPointDiagnostics.pFirmwareVersion = &endpoint_version;

      WDF_OBJECT_ATTRIBUTES adapter_attributes;
      WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&adapter_attributes, AdapterContext);

      IDARG_IN_ADAPTER_INIT adapter_init {};
      adapter_init.WdfDevice = device;
      adapter_init.pCaps = &caps;
      adapter_init.ObjectAttributes = &adapter_attributes;

      IDARG_OUT_ADAPTER_INIT adapter_out {};
      const auto status = IddCxAdapterInitAsync(&adapter_init, &adapter_out);
      if (!NT_SUCCESS(status)) {
        return status;
      }
      if (!adapter_out.AdapterObject) {
        return STATUS_DEVICE_NOT_READY;
      }

      auto *context = GetAdapterContext(adapter_out.AdapterObject);
      context->backend = this;
      {
        std::lock_guard lock {mutex_};
        adapter_ = adapter_out.AdapterObject;
        adapter_ready_ = false;
        adapter_init_status_ = STATUS_PENDING;
      }

      return STATUS_SUCCESS;
    }

    vdd::BackendDisplayResult arrive_temporary_display(const vdd::DisplayDescriptor &descriptor) override {
      return arrive_display(descriptor, false);
    }

    vdd::BackendError reserve_temporary_display_identity(const vdd::DisplayDescriptor &descriptor) override {
      return save_temporary_display_profile(driver_, device_, descriptor);
    }

    vdd::BackendError unreserve_temporary_display_identity(const std::uint64_t display_id) override {
      return remove_temporary_display_profile(driver_, device_, display_id);
    }

    vdd::BackendError depart_temporary_display(const std::uint64_t display_id) override {
      return depart_display(display_id);
    }

    vdd::BackendError set_permanent_display_count(const vdd::PermanentDisplayCountRequest &request) override {
      auto normalized = request;
      vdd::set_default_permanent_display_settings(normalized);
      if (vdd::validate_permanent_display_count(normalized, kMaxPermanentDisplays) != vdd::ValidationError::None) {
        return vdd::BackendError::Failed;
      }

      return apply_display_manifest(vdd::display_manifest_from_permanent_settings(normalized, kMaxPermanentDisplays));
    }

    vdd::BackendError apply_display_manifest(const vdd::DisplayManifest &manifest) override {
      if (vdd::validate_display_manifest(manifest, kMaxPermanentDisplays) != vdd::ValidationError::None) {
        return vdd::BackendError::Failed;
      }

      std::vector<vdd::DisplayDescriptor> active_permanent_descriptors;
      {
        std::lock_guard lock {mutex_};
        active_permanent_descriptors.reserve(monitors_.size());
        for (const auto &[display_id, record]: monitors_) {
          if (record.permanent) {
            active_permanent_descriptors.push_back(record.descriptor);
          }
        }
      }

      for (const auto &descriptor: active_permanent_descriptors) {
        if (depart_display(descriptor.display_id) != vdd::BackendError::None) {
          for (const auto &restore_descriptor: active_permanent_descriptors) {
            (void) arrive_display(restore_descriptor, true);
          }
          return vdd::BackendError::Failed;
        }
      }

      std::vector<std::uint64_t> added;
      for (std::uint32_t index = 0; index < manifest.profile_count; ++index) {
        const auto descriptor = make_permanent_descriptor(manifest.profiles[index]);
        const auto result = arrive_display(descriptor, true);
        if (result.error != vdd::BackendError::None) {
          for (const auto display_id: added) {
            (void) depart_display(display_id);
          }
          for (const auto &restore_descriptor: active_permanent_descriptors) {
            (void) arrive_display(restore_descriptor, true);
          }
          return vdd::BackendError::Failed;
        }
        added.push_back(descriptor.display_id);
      }

      return vdd::BackendError::None;
    }

    vdd::BackendError set_render_adapter(const vdd::SetRenderAdapterRequest &request) override {
      const auto preferred_render_adapter = vdd::to_windows_luid(request.adapter_luid);
      IDDCX_ADAPTER adapter {};
      {
        std::lock_guard lock {mutex_};
        preferred_render_adapter_luid_ = preferred_render_adapter;
        adapter = adapter_;
      }

      if (!adapter) {
        return vdd::BackendError::None;
      }

      IDARG_IN_ADAPTERSETRENDERADAPTER args {};
      args.PreferredRenderAdapter = preferred_render_adapter;
      IddCxAdapterSetRenderAdapter(adapter, &args);

      TraceLoggingWrite(
        g_trace_provider,
        "RenderAdapterPreferenceSet",
        TraceLoggingInt32(preferred_render_adapter.HighPart, "AdapterHigh"),
        TraceLoggingUInt32(preferred_render_adapter.LowPart, "AdapterLow")
      );
      TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "RenderAdapterPreferenceSet");
      return vdd::BackendError::None;
    }

    NTSTATUS adapter_init_finished(const IDARG_IN_ADAPTER_INIT_FINISHED *args) {
      if (!args) {
        return STATUS_INVALID_PARAMETER;
      }

      // The async callback status is the point where IddCx says monitor arrival
      // is legal. Keep DeviceAdd successful, but block display creation until then.
      LUID preferred_render_adapter {};
      IDDCX_ADAPTER adapter {};
      bool adapter_ready = false;
      {
        std::lock_guard lock {mutex_};
        adapter_init_status_ = args->AdapterInitStatus;
        adapter_ready_ = NT_SUCCESS(adapter_init_status_);
        adapter_ready = adapter_ready_;
        adapter = adapter_;
        preferred_render_adapter = preferred_render_adapter_luid_;
      }
      if (!adapter_ready) {
        TraceLoggingWrite(
          g_trace_provider,
          "AdapterInitFailed",
          TraceLoggingInt32(args->AdapterInitStatus, "Status")
        );
        return STATUS_SUCCESS;
      }
      if (adapter && (preferred_render_adapter.HighPart != 0 || preferred_render_adapter.LowPart != 0)) {
        IDARG_IN_ADAPTERSETRENDERADAPTER set_render_adapter {};
        set_render_adapter.PreferredRenderAdapter = preferred_render_adapter;
        IddCxAdapterSetRenderAdapter(adapter, &set_render_adapter);
      }
      return STATUS_SUCCESS;
    }

  private:
    auto find_current_monitor_locked(const std::uint64_t display_id, const IDDCX_MONITOR monitor) {
      const auto record = monitors_.find(display_id);
      if (record == monitors_.end() || record->second.monitor != monitor) {
        return monitors_.end();
      }

      return record;
    }

    void finish_assign_callback(const std::uint64_t display_id, const IDDCX_MONITOR monitor) {
      {
        std::lock_guard lock {mutex_};
        const auto record = find_current_monitor_locked(display_id, monitor);
        if (record != monitors_.end() && record->second.assign_callbacks_in_flight > 0) {
          --record->second.assign_callbacks_in_flight;
        }
      }
      departure_cv_.notify_all();
    }

    class AssignCallbackScope {
    public:
      AssignCallbackScope(IddCxBackend &backend, const std::uint64_t display_id, const IDDCX_MONITOR monitor):
          backend_ {backend},
          display_id_ {display_id},
          monitor_ {monitor} {
      }

      ~AssignCallbackScope() {
        backend_.finish_assign_callback(display_id_, monitor_);
      }

      AssignCallbackScope(const AssignCallbackScope &) = delete;
      AssignCallbackScope &operator=(const AssignCallbackScope &) = delete;

    private:
      IddCxBackend &backend_;
      std::uint64_t display_id_ {};
      IDDCX_MONITOR monitor_ {};
    };

    vdd::BackendDisplayResult arrive_display(const vdd::DisplayDescriptor &requested_descriptor, const bool permanent) {
      const auto descriptor = descriptor_with_runtime_hdr_policy(requested_descriptor);
      IDDCX_ADAPTER adapter {};
      {
        std::lock_guard lock {mutex_};
        if (!adapter_ready_) {
          return {vdd::BackendError::Failed, {}, 0};
        }
        if (!adapter_ || monitors_.contains(descriptor.display_id)) {
          return {vdd::BackendError::Failed, {}, 0};
        }
        adapter = adapter_;
      }

      MonitorRecord record {};
      record.descriptor = descriptor;
      record.permanent = permanent;

      WDF_OBJECT_ATTRIBUTES monitor_attributes;
      WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&monitor_attributes, MonitorContext);

      IDDCX_MONITOR_INFO monitor_info {};
      monitor_info.Size = sizeof(monitor_info);
      // Use a digital sink type so Windows avoids WCG-only classification for
      // HDR, but avoid HDMI's legacy bandwidth ceiling that rejects 4K
      // high-refresh virtual modes.
      monitor_info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL;
      monitor_info.ConnectorIndex = descriptor.connector_index;
      monitor_info.MonitorContainerId = vdd::to_windows_guid(descriptor.container_id);
      monitor_info.MonitorDescription.Size = sizeof(monitor_info.MonitorDescription);
      monitor_info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
      monitor_info.MonitorDescription.DataSize = static_cast<UINT>(record.descriptor.edid.size());
      monitor_info.MonitorDescription.pData = record.descriptor.edid.data();

      IDARG_IN_MONITORCREATE create_args {};
      create_args.ObjectAttributes = &monitor_attributes;
      create_args.pMonitorInfo = &monitor_info;

      IDARG_OUT_MONITORCREATE create_out {};
      auto status = IddCxMonitorCreate(adapter, &create_args, &create_out);
      if (!NT_SUCCESS(status)) {
        return {vdd::BackendError::Failed, {}, 0};
      }

      record.monitor = create_out.MonitorObject;
      auto *monitor_context = GetMonitorContext(record.monitor);
      monitor_context->backend = this;
      monitor_context->display_id = descriptor.display_id;

      {
        try {
          std::lock_guard lock {mutex_};
          if (!adapter_ready_ || adapter_ != adapter || monitors_.contains(descriptor.display_id)) {
            status = STATUS_DEVICE_NOT_READY;
          } else {
            monitors_.emplace(descriptor.display_id, std::move(record));
            status = STATUS_SUCCESS;
          }
        } catch (...) {
          WdfObjectDelete(create_out.MonitorObject);
          return {vdd::BackendError::Failed, {}, 0};
        }
      }
      if (!NT_SUCCESS(status)) {
        WdfObjectDelete(create_out.MonitorObject);
        return {vdd::BackendError::Failed, {}, 0};
      }

      IDARG_OUT_MONITORARRIVAL arrival_out {};
      status = IddCxMonitorArrival(create_out.MonitorObject, &arrival_out);
      if (!NT_SUCCESS(status)) {
        {
          std::lock_guard lock {mutex_};
          monitors_.erase(descriptor.display_id);
        }
        WdfObjectDelete(create_out.MonitorObject);
        return {vdd::BackendError::Failed, {}, 0};
      }

      TraceLoggingWrite(
        g_trace_provider,
        "MonitorArrived",
        TraceLoggingUInt64(descriptor.display_id, "DisplayId"),
        TraceLoggingUInt32(descriptor.connector_index, "ConnectorIndex"),
        TraceLoggingBool(permanent, "Permanent"),
        TraceLoggingUInt32(arrival_out.OsTargetId, "OsTargetId")
      );
      TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "MonitorArrived");
      return {
        vdd::BackendError::None,
        vdd::from_windows_luid(arrival_out.OsAdapterLuid),
        arrival_out.OsTargetId
      };
    }

    vdd::BackendError depart_display(const std::uint64_t display_id) {
      IDDCX_MONITOR monitor_handle {};
      std::unique_ptr<SwapChainProcessor> processor_to_stop;
      std::vector<std::unique_ptr<SwapChainProcessor>> retired_processors_to_stop;
      {
        std::unique_lock lock {mutex_};
        auto monitor = monitors_.find(display_id);
        if (monitor == monitors_.end() || monitor->second.departing) {
          return vdd::BackendError::None;
        }

        monitor->second.departing = true;
        monitor_handle = monitor->second.monitor;
        if (monitor->second.assign_callbacks_in_flight > 0) {
          // DisplayConfig can remove a just-activated path while IddCx is still
          // unwinding HandleNewSwapChain. Mark departure first, then wait only
          // for known in-flight assign callbacks to observe teardown.
          departure_cv_.wait_for(lock, std::chrono::milliseconds(250), [&]() {
            const auto current = monitors_.find(display_id);
            return current == monitors_.end() ||
                   current->second.monitor != monitor_handle ||
                   current->second.assign_callbacks_in_flight == 0;
          });
        }

        monitor = monitors_.find(display_id);
        if (monitor == monitors_.end() || monitor->second.monitor != monitor_handle) {
          return vdd::BackendError::None;
        }
        processor_to_stop = std::move(monitor->second.swapchain_processor);
        retired_processors_to_stop = std::move(monitor->second.retired_swapchain_processors);
      }

      // Stop frame processing and release local swapchain references before
      // departure.
      // IddCxMonitorDeparture can invalidate swapchain objects before local
      // processors leave scope during temporary-display removal.
      stop_swapchain_processor_without_delete(processor_to_stop);
      stop_swapchain_processors_without_delete(retired_processors_to_stop);

      // IddCx can synchronously or asynchronously issue swapchain callbacks
      // during departure. Calling it outside the backend mutex keeps those
      // callbacks from re-entering a locked monitor map.
      const auto status = IddCxMonitorDeparture(monitor_handle);
      if (!NT_SUCCESS(status)) {
        TraceLoggingWrite(
          g_trace_provider,
          "MonitorDepartureFailed",
          TraceLoggingUInt64(display_id, "DisplayId"),
          TraceLoggingInt32(status, "Status")
        );
        std::lock_guard lock {mutex_};
        if (const auto monitor = monitors_.find(display_id);
            monitor != monitors_.end() &&
            monitor->second.monitor == monitor_handle) {
          monitor->second.departing = false;
        }
        return vdd::BackendError::Failed;
      }

      std::lock_guard lock {mutex_};
      if (const auto monitor = monitors_.find(display_id);
          monitor != monitors_.end() &&
          monitor->second.monitor == monitor_handle) {
        monitors_.erase(monitor);
      }
      TraceLoggingWrite(g_trace_provider, "MonitorDeparted", TraceLoggingUInt64(display_id, "DisplayId"));
      TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "MonitorDeparted");
      return vdd::BackendError::None;
    }

  public:
    NTSTATUS assign_swapchain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN *args) {
      if (!monitor || !args || !args->hSwapChain || !args->hNextSurfaceAvailable) {
        return STATUS_INVALID_PARAMETER;
      }

      auto *context = GetMonitorContext(monitor);
      if (!context || !context->backend) {
        return STATUS_DEVICE_NOT_READY;
      }

      {
        std::lock_guard lock {mutex_};
        const auto record = find_current_monitor_locked(context->display_id, monitor);
        if (record == monitors_.end() || record->second.departing) {
          // This status is the IddCx-approved way to decline a swapchain that
          // races with monitor teardown; generic failures trip verifier 0x700.
          return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
        }
        ++record->second.assign_callbacks_in_flight;
      }
      AssignCallbackScope assign_scope {*this, context->display_id, monitor};

      std::unique_ptr<SwapChainProcessor> processor;
      try {
        processor = std::make_unique<SwapChainProcessor>(args->hSwapChain, args->hNextSurfaceAvailable);
      } catch (...) {
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
      }
      const HRESULT hr = processor->start(args->RenderAdapterLuid);
      if (FAILED(hr)) {
        processor->abandon_swapchain();
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
      }

      std::unique_ptr<SwapChainProcessor> previous_processor;
      bool abandon_new_processor = false;
      {
        std::lock_guard lock {mutex_};
        const auto record = find_current_monitor_locked(context->display_id, monitor);
        if (record == monitors_.end() || record->second.departing) {
          abandon_new_processor = true;
        } else {
          previous_processor = std::move(record->second.swapchain_processor);
          record->second.swapchain_processor = std::move(processor);
        }
      }

      if (abandon_new_processor) {
        processor->stop();
        processor->abandon_swapchain();
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
      }

      if (previous_processor) {
        // IddCx rotates swapchains inside HandleNewSwapChain. Keep the old WDF
        // object alive until monitor teardown, but never join its worker while
        // holding the backend monitor map mutex.
        previous_processor->stop();
        bool retired = false;
        try {
          std::lock_guard lock {mutex_};
          const auto record = find_current_monitor_locked(context->display_id, monitor);
          if (record != monitors_.end()) {
            record->second.retired_swapchain_processors.push_back(std::move(previous_processor));
            retired = true;
          }
        } catch (...) {
        }
        if (!retired && previous_processor) {
          previous_processor->abandon_swapchain();
        }
      }

      TraceLoggingWrite(
        g_trace_provider,
        "SwapChainAssigned",
        TraceLoggingUInt64(context->display_id, "DisplayId"),
        TraceLoggingInt32(args->RenderAdapterLuid.HighPart, "RenderAdapterHigh"),
        TraceLoggingUInt32(args->RenderAdapterLuid.LowPart, "RenderAdapterLow")
      );
      TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_SWAPCHAIN, "SwapChainAssigned");
      return STATUS_SUCCESS;
    }

    NTSTATUS unassign_swapchain(IDDCX_MONITOR monitor) {
      if (!monitor) {
        return STATUS_INVALID_PARAMETER;
      }

      auto *context = GetMonitorContext(monitor);
      if (!context || !context->backend) {
        return STATUS_DEVICE_NOT_READY;
      }

      std::unique_ptr<SwapChainProcessor> processor_to_stop;
      std::vector<std::unique_ptr<SwapChainProcessor>> retired_processors_to_stop;
      {
        std::lock_guard lock {mutex_};
        const auto record = find_current_monitor_locked(context->display_id, monitor);
        if (record == monitors_.end()) {
          // Monitor departure may remove our bookkeeping before IddCx delivers a
          // final unassign callback. The swapchain is already gone in that case.
          return STATUS_SUCCESS;
        }

        processor_to_stop = std::move(record->second.swapchain_processor);
        retired_processors_to_stop = std::move(record->second.retired_swapchain_processors);
      }

      // IddCx calls UnassignSwapChain after the associated swapchain is no
      // longer valid. Do not WdfObjectDelete that handle from our destructor.
      stop_swapchain_processor_without_delete(processor_to_stop);
      stop_swapchain_processors_without_delete(retired_processors_to_stop);
      TraceLoggingWrite(g_trace_provider, "SwapChainUnassigned", TraceLoggingUInt64(context->display_id, "DisplayId"));
      TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_SWAPCHAIN, "SwapChainUnassigned");
      return STATUS_SUCCESS;
    }

    NTSTATUS set_default_hdr_metadata(
      IDDCX_MONITOR monitor,
      const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA *args
    ) {
      if (!monitor || !args || (args->Size > 0 && !args->Data.pHdr10)) {
        return STATUS_INVALID_PARAMETER;
      }

      auto *context = GetMonitorContext(monitor);
      if (!context || !context->backend) {
        return STATUS_DEVICE_NOT_READY;
      }

      {
        std::lock_guard lock {mutex_};
        const auto record = find_current_monitor_locked(context->display_id, monitor);
        if (record == monitors_.end()) {
          return STATUS_DEVICE_NOT_READY;
        }
        record->second.default_hdr_metadata_type = args->Type;
        record->second.default_hdr_metadata_size = args->Size;
      }

      TraceLoggingWrite(
        g_trace_provider,
        "DefaultHdrMetadataSet",
        TraceLoggingUInt64(context->display_id, "DisplayId"),
        TraceLoggingUInt32(static_cast<std::uint32_t>(args->Type), "Type"),
        TraceLoggingUInt32(args->Size, "Size")
      );
      return STATUS_SUCCESS;
    }

    NTSTATUS set_gamma_ramp(IDDCX_MONITOR monitor, const IDARG_IN_SET_GAMMARAMP *args) {
      if (!monitor || !args || (args->GammaRampSizeInBytes > 0 && !args->pGammaRampData)) {
        return STATUS_INVALID_PARAMETER;
      }

      auto *context = GetMonitorContext(monitor);
      if (!context || !context->backend) {
        return STATUS_DEVICE_NOT_READY;
      }

      {
        std::lock_guard lock {mutex_};
        const auto record = find_current_monitor_locked(context->display_id, monitor);
        if (record == monitors_.end()) {
          return STATUS_DEVICE_NOT_READY;
        }
        record->second.gamma_ramp_type = args->Type;
        record->second.gamma_ramp_size = args->GammaRampSizeInBytes;
      }

      TraceLoggingWrite(
        g_trace_provider,
        "GammaRampSet",
        TraceLoggingUInt64(context->display_id, "DisplayId"),
        TraceLoggingUInt32(static_cast<std::uint32_t>(args->Type), "Type"),
        TraceLoggingUInt32(args->GammaRampSizeInBytes, "Size")
      );
      return STATUS_SUCCESS;
    }

    NTSTATUS query_target_modes(
      IDDCX_MONITOR monitor,
      const IDARG_IN_QUERYTARGETMODES *input,
      IDARG_OUT_QUERYTARGETMODES *output
    ) {
      const auto requested_shape = requested_mode_shape(monitor);
      if (requested_shape.has_value()) {
        return fill_target_modes(input, output, &*requested_shape);
      }

      return fill_target_modes(input, output);
    }

    NTSTATUS query_target_modes2(
      IDDCX_MONITOR monitor,
      const IDARG_IN_QUERYTARGETMODES2 *input,
      IDARG_OUT_QUERYTARGETMODES *output
    ) {
      const auto requested_shape = requested_mode_shape(monitor);
      if (requested_shape.has_value()) {
        return fill_target_modes2(input, output, &*requested_shape);
      }

      return fill_target_modes2(input, output);
    }

  private:
    std::optional<ModeShape> requested_mode_shape(IDDCX_MONITOR monitor) {
      if (!monitor) {
        return std::nullopt;
      }

      auto *context = GetMonitorContext(monitor);
      if (!context || !context->backend) {
        return std::nullopt;
      }

      std::lock_guard lock {mutex_};
      const auto record = find_current_monitor_locked(context->display_id, monitor);
      if (record == monitors_.end()) {
        return std::nullopt;
      }

      return mode_shape_from_descriptor(record->second.descriptor);
    }

    std::mutex mutex_ {};
    std::condition_variable departure_cv_ {};
    WDFDRIVER driver_ {};
    WDFDEVICE device_ {};
    IDDCX_ADAPTER adapter_ {};
    LUID preferred_render_adapter_luid_ {};
    bool adapter_ready_ {};
    NTSTATUS adapter_init_status_ {STATUS_DEVICE_NOT_READY};
    std::map<std::uint64_t, MonitorRecord> monitors_ {};
  };

  class DeviceState {
  public:
    DeviceState(WDFDRIVER driver, WDFDEVICE device):
        backend {driver, device},
        controller {
          vdd::DisplayStore {
            kMaxPermanentDisplays,
            kMaxTemporaryDisplays,
            load_temporary_connector_reservations(driver, device)
          },
          backend
        },
        dispatcher {controller} {
      start_reaper();
    }

    ~DeviceState() {
      stop_reaper();
    }

    vdd::IoctlDispatchResult dispatch(
      const ULONG io_control_code,
      void *input,
      const std::size_t input_buffer_length,
      void *output,
      const std::size_t output_buffer_length,
      const std::chrono::steady_clock::time_point now
    ) {
      std::lock_guard lock {controller_mutex};
      return dispatcher.dispatch(
        io_control_code,
        input,
        input_buffer_length,
        output,
        output_buffer_length,
        now
      );
    }

    IddCxBackend backend;
    vdd::DriverController controller;
    vdd::IoctlDispatcher dispatcher;

  private:
    static constexpr auto kReaperInterval = std::chrono::seconds(1);

    void start_reaper() {
      try {
        reaper_thread = std::thread([this]() {
          reaper_loop();
        });
      } catch (...) {
        // The control plane can still remove displays explicitly. If the
        // reaper cannot start, creation still works and lease feeds remain
        // validated by the store.
      }
    }

    void stop_reaper() {
      reaper_stop_requested.store(true, std::memory_order_release);
      reaper_cv.notify_all();
      if (reaper_thread.joinable()) {
        reaper_thread.join();
      }
    }

    void reaper_loop() {
      std::unique_lock wait_lock {reaper_wait_mutex};
      while (!reaper_stop_requested.load(std::memory_order_acquire)) {
        if (reaper_cv.wait_for(wait_lock, kReaperInterval, [this]() {
              return reaper_stop_requested.load(std::memory_order_acquire);
            })) {
          break;
        }

        std::lock_guard controller_lock {controller_mutex};
        (void) controller.reap_expired(std::chrono::steady_clock::now());
      }
    }

    std::mutex controller_mutex {};
    std::atomic<bool> reaper_stop_requested {false};
    std::mutex reaper_wait_mutex {};
    std::condition_variable reaper_cv {};
    std::thread reaper_thread {};
  };

  NTSTATUS ntstatus_from_ioctl_status(const vdd::IoctlStatus status) {
    switch (status) {
      case vdd::IoctlStatus::Success:
        return STATUS_SUCCESS;
      case vdd::IoctlStatus::InvalidIoctl:
        return STATUS_INVALID_DEVICE_REQUEST;
      case vdd::IoctlStatus::InvalidInputBuffer:
      case vdd::IoctlStatus::InvalidRequest:
        return STATUS_INVALID_PARAMETER;
      case vdd::IoctlStatus::InvalidOutputBuffer:
        return STATUS_BUFFER_TOO_SMALL;
      case vdd::IoctlStatus::AlreadyExists:
        return STATUS_DEVICE_BUSY;
      case vdd::IoctlStatus::LimitReached:
        return STATUS_INSUFFICIENT_RESOURCES;
      case vdd::IoctlStatus::NotFound:
        return STATUS_NOT_FOUND;
      case vdd::IoctlStatus::BackendFailed:
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_UNSUCCESSFUL;
  }

  void cleanup_device_context(WDFOBJECT object) {
    auto *context = GetDeviceContext(static_cast<WDFDEVICE>(object));
    delete context->state;
    context->state = nullptr;
  }

  NTSTATUS retrieve_request_buffer(
    WDFREQUEST request,
    const bool output,
    const std::size_t length,
    void **buffer
  ) {
    *buffer = nullptr;
    if (length == 0) {
      return STATUS_SUCCESS;
    }

    return output ?
      WdfRequestRetrieveOutputBuffer(request, length, buffer, nullptr) :
      WdfRequestRetrieveInputBuffer(request, length, buffer, nullptr);
  }
}  // namespace

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_UNLOAD SunshineEvtDriverUnload;
EVT_WDF_DRIVER_DEVICE_ADD SunshineEvtDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY SunshineEvtDeviceD0Entry;
EVT_IDD_CX_DEVICE_IO_CONTROL SunshineEvtIddCxDeviceIoControl;
EVT_IDD_CX_ADAPTER_INIT_FINISHED SunshineEvtAdapterInitFinished;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES SunshineEvtGetDefaultDescriptionModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION SunshineEvtParseMonitorDescription;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES SunshineEvtQueryTargetModes;
EVT_IDD_CX_ADAPTER_COMMIT_MODES SunshineEvtCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION2 SunshineEvtParseMonitorDescription2;
EVT_IDD_CX_ADAPTER_QUERY_TARGET_INFO SunshineEvtAdapterQueryTargetInfo;
EVT_IDD_CX_ADAPTER_COMMIT_MODES2 SunshineEvtCommitModes2;
EVT_IDD_CX_MONITOR_SET_DEFAULT_HDR_METADATA SunshineEvtSetDefaultHdrMetadata;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2 SunshineEvtQueryTargetModes2;
EVT_IDD_CX_MONITOR_SET_GAMMA_RAMP SunshineEvtSetGammaRamp;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN SunshineEvtAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN SunshineEvtUnassignSwapChain;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object, PUNICODE_STRING registry_path) {
  WPP_INIT_TRACING(driver_object, registry_path);
  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "DriverEntry");

  TraceLoggingRegister(g_trace_provider);
  TraceLoggingWrite(g_trace_provider, "DriverEntry");

  WDF_DRIVER_CONFIG config;
  WDF_DRIVER_CONFIG_INIT(&config, SunshineEvtDeviceAdd);
  config.EvtDriverUnload = SunshineEvtDriverUnload;

  const NTSTATUS status = WdfDriverCreate(
    driver_object,
    registry_path,
    WDF_NO_OBJECT_ATTRIBUTES,
    &config,
    WDF_NO_HANDLE
  );
  TraceLoggingWrite(
    g_trace_provider,
    "DriverEntryResult",
    TraceLoggingInt32(status, "Status")
  );
  if (!NT_SUCCESS(status)) {
    TraceLoggingUnregister(g_trace_provider);
    WPP_CLEANUP(driver_object);
  }
  return status;
}

void SunshineEvtDriverUnload(WDFDRIVER driver) {
  TraceLoggingWrite(g_trace_provider, "DriverUnload");
  TraceLoggingUnregister(g_trace_provider);
  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "DriverUnload");
  WPP_CLEANUP(WdfDriverWdmGetDriverObject(driver));
}

NTSTATUS SunshineEvtDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT device_init) {
  TraceLoggingWrite(g_trace_provider, "DeviceAdd");
  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "DeviceAdd");

  WDF_PNPPOWER_EVENT_CALLBACKS pnp_callbacks;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp_callbacks);
  pnp_callbacks.EvtDeviceD0Entry = SunshineEvtDeviceD0Entry;
  WdfDeviceInitSetPnpPowerEventCallbacks(device_init, &pnp_callbacks);

  IDD_CX_CLIENT_CONFIG idd_config;
  IDD_CX_CLIENT_CONFIG_INIT(&idd_config);
  idd_config.EvtIddCxDeviceIoControl = SunshineEvtIddCxDeviceIoControl;
  idd_config.EvtIddCxAdapterInitFinished = SunshineEvtAdapterInitFinished;
  idd_config.EvtIddCxMonitorGetDefaultDescriptionModes = SunshineEvtGetDefaultDescriptionModes;
  idd_config.EvtIddCxMonitorAssignSwapChain = SunshineEvtAssignSwapChain;
  idd_config.EvtIddCxMonitorUnassignSwapChain = SunshineEvtUnassignSwapChain;
  if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorSetGammaRamp)) {
    idd_config.EvtIddCxMonitorSetGammaRamp = SunshineEvtSetGammaRamp;
  }
  if (has_hdr_iddcx_ddi()) {
    idd_config.EvtIddCxParseMonitorDescription2 = SunshineEvtParseMonitorDescription2;
    idd_config.EvtIddCxAdapterQueryTargetInfo = SunshineEvtAdapterQueryTargetInfo;
    idd_config.EvtIddCxAdapterCommitModes2 = SunshineEvtCommitModes2;
    idd_config.EvtIddCxMonitorSetDefaultHdrMetaData = SunshineEvtSetDefaultHdrMetadata;
    idd_config.EvtIddCxMonitorQueryTargetModes2 = SunshineEvtQueryTargetModes2;
  } else {
    idd_config.EvtIddCxParseMonitorDescription = SunshineEvtParseMonitorDescription;
    idd_config.EvtIddCxMonitorQueryTargetModes = SunshineEvtQueryTargetModes;
    idd_config.EvtIddCxAdapterCommitModes = SunshineEvtCommitModes;
  }

  NTSTATUS status = IddCxDeviceInitConfig(device_init, &idd_config);
  if (!NT_SUCCESS(status)) {
    TraceLoggingWrite(g_trace_provider, "DeviceAddFailed", TraceLoggingInt32(status, "Status"));
    return status;
  }

  WDF_OBJECT_ATTRIBUTES device_attributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&device_attributes, DeviceContext);
  device_attributes.EvtCleanupCallback = cleanup_device_context;

  WDFDEVICE device = nullptr;
  status = WdfDeviceCreate(&device_init, &device_attributes, &device);
  if (!NT_SUCCESS(status)) {
    TraceLoggingWrite(g_trace_provider, "DeviceAddFailed", TraceLoggingInt32(status, "Status"));
    return status;
  }

  auto *context = GetDeviceContext(device);
  try {
    context->state = new DeviceState(driver, device);
  } catch (...) {
    context->state = nullptr;
    TraceLoggingWrite(
      g_trace_provider,
      "DeviceAddFailed",
      TraceLoggingInt32(STATUS_INSUFFICIENT_RESOURCES, "Status")
    );
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  status = WdfDeviceCreateDeviceInterface(device, &kControlInterfaceGuid, nullptr);
  if (!NT_SUCCESS(status)) {
    TraceLoggingWrite(g_trace_provider, "DeviceAddFailed", TraceLoggingInt32(status, "Status"));
    return status;
  }

  status = IddCxDeviceInitialize(device);
  if (!NT_SUCCESS(status)) {
    TraceLoggingWrite(g_trace_provider, "DeviceAddFailed", TraceLoggingInt32(status, "Status"));
    return status;
  }

  TraceLoggingWrite(g_trace_provider, "DeviceAddSucceeded");
  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "DeviceAddSucceeded");
  return STATUS_SUCCESS;
}

NTSTATUS SunshineEvtDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
  auto *context = GetDeviceContext(device);
  if (!context || !context->state) {
    return STATUS_DEVICE_NOT_READY;
  }

  // IddCx adapter init requires the WDF device to be powered. Doing this in
  // DeviceAdd leaves the PDO installed but the adapter unusable on restart.
  return context->state->backend.initialize_adapter(device);
}

NTSTATUS SunshineEvtAdapterInitFinished(
  IDDCX_ADAPTER adapter,
  const IDARG_IN_ADAPTER_INIT_FINISHED *args
) {
  auto *context = GetAdapterContext(adapter);
  if (!context || !context->backend) {
    return STATUS_DEVICE_NOT_READY;
  }

  return context->backend->adapter_init_finished(args);
}

NTSTATUS SunshineEvtParseMonitorDescription(
  const IDARG_IN_PARSEMONITORDESCRIPTION *input,
  IDARG_OUT_PARSEMONITORDESCRIPTION *output
) {
  return fill_monitor_modes(input, output);
}

NTSTATUS SunshineEvtGetDefaultDescriptionModes(
  IDDCX_MONITOR,
  const IDARG_IN_GETDEFAULTDESCRIPTIONMODES *input,
  IDARG_OUT_GETDEFAULTDESCRIPTIONMODES *output
) {
  return fill_default_monitor_modes(input, output);
}

NTSTATUS SunshineEvtQueryTargetModes(
  IDDCX_MONITOR monitor,
  const IDARG_IN_QUERYTARGETMODES *input,
  IDARG_OUT_QUERYTARGETMODES *output
) {
  if (!monitor) {
    return STATUS_INVALID_PARAMETER;
  }

  auto *context = GetMonitorContext(monitor);
  if (context && context->backend) {
    return context->backend->query_target_modes(monitor, input, output);
  }

  return fill_target_modes(input, output);
}

NTSTATUS SunshineEvtCommitModes(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES *) {
  return STATUS_SUCCESS;
}

NTSTATUS SunshineEvtParseMonitorDescription2(
  const IDARG_IN_PARSEMONITORDESCRIPTION2 *input,
  IDARG_OUT_PARSEMONITORDESCRIPTION *output
) {
  return fill_monitor_modes2(input, output);
}

NTSTATUS SunshineEvtAdapterQueryTargetInfo(
  IDDCX_ADAPTER,
  IDARG_IN_QUERYTARGET_INFO *,
  IDARG_OUT_QUERYTARGET_INFO *output
) {
  if (!output) {
    return STATUS_INVALID_PARAMETER;
  }

  output->TargetCaps = static_cast<IDDCX_TARGET_CAPS>(
    static_cast<UINT>(IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE) |
    static_cast<UINT>(IDDCX_TARGET_CAPS_WIDE_COLOR_SPACE)
  );
  populate_rgb_wire_bits(output->DitheringSupport, IDDCX_BITS_PER_COMPONENT_10);
  return STATUS_SUCCESS;
}

NTSTATUS SunshineEvtCommitModes2(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES2 *) {
  return STATUS_SUCCESS;
}

NTSTATUS SunshineEvtSetDefaultHdrMetadata(
  IDDCX_MONITOR monitor,
  const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA *args
) {
  if (!monitor) {
    return STATUS_INVALID_PARAMETER;
  }

  auto *context = GetMonitorContext(monitor);
  if (!context || !context->backend) {
    return STATUS_DEVICE_NOT_READY;
  }

  return context->backend->set_default_hdr_metadata(monitor, args);
}

NTSTATUS SunshineEvtSetGammaRamp(
  IDDCX_MONITOR monitor,
  const IDARG_IN_SET_GAMMARAMP *args
) {
  if (!monitor) {
    return STATUS_INVALID_PARAMETER;
  }

  auto *context = GetMonitorContext(monitor);
  if (!context || !context->backend) {
    return STATUS_DEVICE_NOT_READY;
  }

  return context->backend->set_gamma_ramp(monitor, args);
}

NTSTATUS SunshineEvtQueryTargetModes2(
  IDDCX_MONITOR monitor,
  const IDARG_IN_QUERYTARGETMODES2 *input,
  IDARG_OUT_QUERYTARGETMODES *output
) {
  if (!monitor) {
    return STATUS_INVALID_PARAMETER;
  }

  auto *context = GetMonitorContext(monitor);
  if (context && context->backend) {
    return context->backend->query_target_modes2(monitor, input, output);
  }

  return fill_target_modes2(input, output);
}

NTSTATUS SunshineEvtAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN *args) {
  if (!monitor) {
    return STATUS_INVALID_PARAMETER;
  }

  auto *context = GetMonitorContext(monitor);
  if (!context || !context->backend) {
    return STATUS_DEVICE_NOT_READY;
  }

  return context->backend->assign_swapchain(monitor, args);
}

NTSTATUS SunshineEvtUnassignSwapChain(IDDCX_MONITOR monitor) {
  if (!monitor) {
    return STATUS_INVALID_PARAMETER;
  }

  auto *context = GetMonitorContext(monitor);
  if (!context || !context->backend) {
    return STATUS_DEVICE_NOT_READY;
  }

  return context->backend->unassign_swapchain(monitor);
}

void SunshineEvtIddCxDeviceIoControl(
  WDFDEVICE device,
  WDFREQUEST request,
  const std::size_t output_buffer_length,
  const std::size_t input_buffer_length,
  const ULONG io_control_code
) {
  auto *context = GetDeviceContext(device);
  if (!context || !context->state) {
    WdfRequestComplete(request, STATUS_DEVICE_NOT_READY);
    return;
  }

  void *input = nullptr;
  NTSTATUS status = retrieve_request_buffer(request, false, input_buffer_length, &input);
  if (!NT_SUCCESS(status)) {
    WdfRequestComplete(request, status);
    return;
  }

  void *output = nullptr;
  status = retrieve_request_buffer(request, true, output_buffer_length, &output);
  if (!NT_SUCCESS(status)) {
    WdfRequestComplete(request, status);
    return;
  }

  const auto result = context->state->dispatch(
    io_control_code,
    input,
    input_buffer_length,
    output,
    output_buffer_length,
    std::chrono::steady_clock::now()
  );
  const auto completion_status = ntstatus_from_ioctl_status(result.status);
  TraceLoggingWrite(
    g_trace_provider,
    "DeviceIoControl",
    TraceLoggingUInt32(io_control_code, "IoControlCode"),
    TraceLoggingUInt32(static_cast<std::uint32_t>(result.status), "DispatchStatus"),
    TraceLoggingUInt64(result.bytes_returned, "BytesReturned"),
    TraceLoggingInt32(completion_status, "Status")
  );
  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "DeviceIoControl");

  WdfRequestCompleteWithInformation(
    request,
    completion_status,
    result.bytes_returned
  );
}
