#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include "virtual_display/driver/ioctl_dispatcher.h"
#include "virtual_display/driver/hdr_capabilities.h"
#include "virtual_display/driver/lease_store.h"
#include "virtual_display/driver/windows_control_protocol.h"
#include "virtual_display/driver/windows_driver_modes.h"
#include "virtual_display/driver/windows_driver_state.h"

#include <Windows.h>
#include <avrt.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <sddl.h>
#include <TraceLoggingProvider.h>
#include <Wtsapi32.h>
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
#include <map>
#include <memory>
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
    WPP_DEFINE_BIT(TRACE_SWAPCHAIN) \
    WPP_DEFINE_BIT(TRACE_CURSOR))

#define WPP_LEVEL_FLAGS_LOGGER(level, flags) WPP_LEVEL_LOGGER(flags)
#define WPP_LEVEL_FLAGS_ENABLED(level, flags) \
  (WPP_LEVEL_ENABLED(flags) && WPP_CONTROL(WPP_BIT_##flags).Level >= level)

// begin_wpp config
// FUNC TraceEvents(LEVEL, FLAGS, MSG, ...);
// end_wpp
#include "driver_main.tmh"

namespace {
  constexpr std::uint32_t kMaxPermanentDisplays = vdd::kWindowsDriverMaxPermanentDisplays;
  constexpr std::uint32_t kMaxTemporaryDisplays = vdd::kWindowsDriverMaxTemporaryDisplays;
  constexpr wchar_t kSwapchainMmcssTask[] = L"DisplayPostProcessing";
  constexpr auto kSwapchainProcessorTeardownTimeout = std::chrono::milliseconds(500);
  constexpr auto kCursorProcessorTeardownTimeout = std::chrono::milliseconds(500);
  constexpr auto kMonitorArrivalTimeout = std::chrono::milliseconds(500);
  constexpr auto kMonitorDepartureTimeout = std::chrono::milliseconds(500);
  constexpr auto kRenderAdapterTimeout = std::chrono::milliseconds(500);
  constexpr std::uint32_t kHardwareCursorMaxWidth = 256;
  constexpr std::uint32_t kHardwareCursorMaxHeight = 256;
  constexpr std::size_t kHardwareCursorShapeBufferBytes =
    static_cast<std::size_t>(kHardwareCursorMaxWidth) *
    static_cast<std::size_t>(kHardwareCursorMaxHeight) *
    sizeof(std::uint32_t);
  constexpr wchar_t kTemporaryDisplayProfilesValue[] = L"TemporaryDisplayProfiles";
  const GUID kControlInterfaceGuid = vdd::to_windows_guid(vdd::kDeviceInterfaceGuid);

  class IddCxBackend;
  class CursorProcessor;
  class SwapChainProcessor;

  struct AdapterContext {
    IddCxBackend *backend {};
  };

  struct MonitorContext {
    // Atomic because shutdown nulls this (depart-failed path) while IddCx monitor
    // callbacks on other threads read it. Plain-pointer load/store would be a data
    // race (benign on x64 in practice, but formally UB). std::atomic<T*>'s implicit
    // load/store conversions keep the call sites unchanged.
    std::atomic<IddCxBackend *> backend {};
    std::uint64_t display_id {};
  };

  struct DeviceContext {
    class DeviceState *state {};
  };

  struct HdrProfileRetentionWorkItemContext {
    vdd::DisplayDescriptor descriptor {};
    std::uint32_t target_id {};
    DWORD retained_dpi_value {};
    bool has_retained_dpi_value {};
  };

  WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AdapterContext, GetAdapterContext);
  WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorContext, GetMonitorContext);
  WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetDeviceContext);
  WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(HdrProfileRetentionWorkItemContext, GetHdrProfileRetentionWorkItemContext);

  struct MonitorRecord {
    vdd::DisplayDescriptor descriptor {};
    IDDCX_MONITOR monitor {};
    std::unique_ptr<CursorProcessor> cursor_processor {};
    std::unique_ptr<SwapChainProcessor> swapchain_processor {};
    std::vector<std::unique_ptr<SwapChainProcessor>> retired_swapchain_processors {};
    bool permanent {};
    bool arriving {};
    bool departing {};
    bool orphaned_late_arrival {};
    std::uint32_t assign_callbacks_in_flight {};
    std::uint32_t unassign_callbacks_in_flight {};
    IDDCX_DEFAULT_HDR_METADATA_TYPE default_hdr_metadata_type {IDDCX_HDRMETADATA_TYPE_UNINITIALIZED};
    UINT default_hdr_metadata_size {};
    IDDCX_GAMMARAMP_TYPE gamma_ramp_type {IDDCX_GAMMARAMP_TYPE_UNINITIALIZED};
    UINT gamma_ramp_size {};
  };

  using ModeShape = vdd::WindowsDriverModeShape;

  UNICODE_STRING unicode_string(const wchar_t *value) {
    UNICODE_STRING result {};
    RtlInitUnicodeString(&result, value);
    return result;
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

  class NativeRegistryKey {
  public:
    NativeRegistryKey() = default;
    explicit NativeRegistryKey(HKEY key):
        key_ {key} {
    }
    NativeRegistryKey(const NativeRegistryKey &) = delete;
    NativeRegistryKey &operator=(const NativeRegistryKey &) = delete;
    NativeRegistryKey(NativeRegistryKey &&other) noexcept:
        key_ {std::exchange(other.key_, nullptr)} {
    }
    NativeRegistryKey &operator=(NativeRegistryKey &&other) noexcept {
      if (this != &other) {
        reset(std::exchange(other.key_, nullptr));
      }
      return *this;
    }
    ~NativeRegistryKey() {
      reset();
    }

    HKEY get() const {
      return key_;
    }

    HKEY *put() {
      reset();
      return &key_;
    }

    void reset(HKEY key = nullptr) {
      if (key_) {
        RegCloseKey(key_);
      }
      key_ = key;
    }

  private:
    HKEY key_ {};
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

  std::vector<vdd::TemporaryDisplayProfile> load_temporary_display_profiles(WDFDRIVER driver, WDFDEVICE device) {
    std::vector<vdd::TemporaryDisplayProfile> profiles;
    RegistryKey state_key;
    if (!NT_SUCCESS(open_driver_state_key(driver, device, KEY_READ, state_key))) {
      return profiles;
    }

    std::vector<std::uint8_t> blob(vdd::kTemporaryDisplayProfilesMaxBytes);
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
    if (!NT_SUCCESS(status) || value_type != REG_BINARY || value_length < vdd::kTemporaryDisplayProfilesHeaderBytes ||
        value_length > blob.size()) {
      return profiles;
    }

    blob.resize(value_length);
    const auto parsed = vdd::parse_temporary_display_profiles_blob(blob);
    return parsed.value_or(std::vector<vdd::TemporaryDisplayProfile> {});
  }

  std::map<std::uint64_t, std::uint32_t> load_temporary_connector_reservations(WDFDRIVER driver, WDFDEVICE device) {
    const auto profiles = load_temporary_display_profiles(driver, device);
    return vdd::temporary_connector_reservations(profiles);
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
    RegistryKey state_key;
    auto status = open_driver_state_key(driver, device, KEY_READ | KEY_SET_VALUE, state_key);
    if (!NT_SUCCESS(status)) {
      return vdd::BackendError::Failed;
    }

    auto profiles = load_temporary_display_profiles(driver, device);
    const vdd::TemporaryDisplayProfile profile {
      descriptor.display_id,
      descriptor.connector_index,
      descriptor.container_id,
      vdd::read_product_code(descriptor.edid),
      vdd::read_serial_number(descriptor.edid)
    };
    const auto updated_profiles = vdd::upsert_temporary_display_profile(std::move(profiles), profile);
    if (!updated_profiles) {
      return vdd::BackendError::Failed;
    }
    auto blob = vdd::serialize_temporary_display_profiles(*updated_profiles);

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
    profiles = vdd::remove_temporary_display_profile(std::move(profiles), display_id);
    if (profiles.size() == original_size) {
      return vdd::BackendError::None;
    }

    auto blob = vdd::serialize_temporary_display_profiles(profiles);

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

  bool read_registry_string(HKEY key, const wchar_t *value_name, std::wstring &value) {
    DWORD type {};
    DWORD size {};
    auto status = RegQueryValueExW(key, value_name, nullptr, &type, nullptr, &size);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || size < sizeof(wchar_t)) {
      return false;
    }

    std::wstring buffer(size / sizeof(wchar_t), L'\0');
    status = RegQueryValueExW(key, value_name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()), &size);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
      return false;
    }

    while (!buffer.empty() && buffer.back() == L'\0') {
      buffer.pop_back();
    }
    value = std::move(buffer);
    return true;
  }

  bool read_registry_dword(HKEY key, const wchar_t *value_name, DWORD &value) {
    DWORD type {};
    DWORD size = sizeof(value);
    return RegQueryValueExW(key, value_name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS &&
           type == REG_DWORD &&
           size == sizeof(value);
  }

  std::vector<std::byte> read_registry_profile_value(HKEY key, const wchar_t *value_name, DWORD &type) {
    type = 0;
    DWORD size {};
    auto status = RegQueryValueExW(key, value_name, nullptr, &type, nullptr, &size);
    if (status != ERROR_SUCCESS || size == 0 || (type != REG_MULTI_SZ && type != REG_SZ)) {
      return {};
    }

    std::vector<std::byte> value(size);
    status = RegQueryValueExW(key, value_name, nullptr, &type, reinterpret_cast<LPBYTE>(value.data()), &size);
    if (status != ERROR_SUCCESS || size == 0 || (type != REG_MULTI_SZ && type != REG_SZ)) {
      return {};
    }
    value.resize(size);
    return value;
  }

  std::optional<std::wstring> active_console_user_sid_string() {
    const DWORD session_id = WTSGetActiveConsoleSessionId();
    if (session_id == 0xffffffffu) {
      return std::nullopt;
    }

    HANDLE token {};
    if (!WTSQueryUserToken(session_id, &token)) {
      TraceLoggingWrite(
        g_trace_provider,
        "UserSettingsRetentionUserTokenUnavailable",
        TraceLoggingUInt32(GetLastError(), "NativeError"),
        TraceLoggingUInt32(session_id, "SessionId")
      );
      return std::nullopt;
    }

    DWORD token_user_size {};
    (void) GetTokenInformation(token, TokenUser, nullptr, 0, &token_user_size);
    if (token_user_size == 0) {
      CloseHandle(token);
      return std::nullopt;
    }

    std::vector<std::byte> token_user_buffer(token_user_size);
    if (!GetTokenInformation(token, TokenUser, token_user_buffer.data(), token_user_size, &token_user_size)) {
      CloseHandle(token);
      return std::nullopt;
    }

    CloseHandle(token);

    const auto *token_user = reinterpret_cast<const TOKEN_USER *>(token_user_buffer.data());
    LPWSTR sid_text {};
    if (!ConvertSidToStringSidW(token_user->User.Sid, &sid_text)) {
      return std::nullopt;
    }

    std::wstring result {sid_text};
    LocalFree(sid_text);
    return result;
  }

  std::wstring temporary_monitor_hardware_id(const vdd::DisplayDescriptor &descriptor) {
    wchar_t text[8] {};
    std::swprintf(text, std::size(text), L"SDD%04X", static_cast<unsigned int>(vdd::read_product_code(descriptor.edid)));
    return text;
  }

  std::wstring temporary_per_monitor_settings_prefix(const vdd::DisplayDescriptor &descriptor) {
    return temporary_monitor_hardware_id(descriptor) + std::to_wstring(vdd::read_serial_number(descriptor.edid));
  }

  std::optional<DWORD> read_retained_temporary_dpi_value(
    HKEY per_monitor_settings_key,
    const std::wstring &settings_prefix
  ) {
    // A single virtual display accumulates several PerMonitorSettings subkeys
    // over its lifetime - one per OS source slot it has ever been assigned
    // (the slot churns whenever Windows hands the monitor a new OsTargetId).
    // Windows seeds each freshly minted slot at the recommended scaling
    // (DpiValue == 0), so the old "return the first prefix match" logic was a
    // coin flip that could capture a default-seeded slot instead of the slot
    // still holding the user's customized scaling - and then the apply step
    // would broadcast that default over every sibling, permanently losing the
    // user's value. Prefer the user-customized (non-zero) value, and when
    // several disagree keep the most recently written one.
    bool have_value = false;
    DWORD best_value = 0;
    FILETIME best_written {};
    for (DWORD index = 0;; ++index) {
      wchar_t settings_key_name[256] {};
      DWORD settings_key_name_size = static_cast<DWORD>(std::size(settings_key_name));
      FILETIME last_written {};
      const auto enum_status = RegEnumKeyExW(
        per_monitor_settings_key,
        index,
        settings_key_name,
        &settings_key_name_size,
        nullptr,
        nullptr,
        nullptr,
        &last_written
      );
      if (enum_status == ERROR_NO_MORE_ITEMS) {
        break;
      }
      if (enum_status != ERROR_SUCCESS || wcsncmp(settings_key_name, settings_prefix.c_str(), settings_prefix.size()) != 0) {
        continue;
      }

      NativeRegistryKey settings_key;
      if (RegOpenKeyExW(per_monitor_settings_key, settings_key_name, 0, KEY_QUERY_VALUE, settings_key.put()) != ERROR_SUCCESS) {
        continue;
      }

      DWORD dpi_value {};
      if (!read_registry_dword(settings_key.get(), L"DpiValue", dpi_value)) {
        continue;
      }

      // DpiValue is a signed step relative to the monitor's recommended
      // scaling; 0 means "use recommended" and is exactly what a default-seeded
      // slot holds, so it carries no user intent worth restoring.
      if (dpi_value == 0) {
        continue;
      }

      if (!have_value || CompareFileTime(&last_written, &best_written) > 0) {
        have_value = true;
        best_value = dpi_value;
        best_written = last_written;
      }
    }

    if (!have_value) {
      return std::nullopt;
    }
    return best_value;
  }

  std::optional<DWORD> read_retained_temporary_dpi_value(const vdd::DisplayDescriptor &descriptor) {
    if (!descriptor.retain_identity) {
      return std::nullopt;
    }

    const auto sid = active_console_user_sid_string();
    if (!sid) {
      return std::nullopt;
    }

    NativeRegistryKey per_monitor_settings_key;
    const auto per_monitor_settings_path = *sid + L"\\Control Panel\\Desktop\\PerMonitorSettings";
    if (RegOpenKeyExW(
          HKEY_USERS,
          per_monitor_settings_path.c_str(),
          0,
          KEY_ENUMERATE_SUB_KEYS,
          per_monitor_settings_key.put()
        ) != ERROR_SUCCESS) {
      return std::nullopt;
    }

    const auto settings_prefix = temporary_per_monitor_settings_prefix(descriptor);
    return read_retained_temporary_dpi_value(per_monitor_settings_key.get(), settings_prefix);
  }

  bool apply_retained_temporary_dpi_value(
    const vdd::DisplayDescriptor &descriptor,
    const DWORD dpi_value
  ) {
    if (!descriptor.retain_identity) {
      return false;
    }

    const auto sid = active_console_user_sid_string();
    if (!sid) {
      return false;
    }

    NativeRegistryKey per_monitor_settings_key;
    const auto per_monitor_settings_path = *sid + L"\\Control Panel\\Desktop\\PerMonitorSettings";
    if (RegOpenKeyExW(
          HKEY_USERS,
          per_monitor_settings_path.c_str(),
          0,
          KEY_ENUMERATE_SUB_KEYS,
          per_monitor_settings_key.put()
        ) != ERROR_SUCCESS) {
      return false;
    }

    const auto settings_prefix = temporary_per_monitor_settings_prefix(descriptor);
    std::uint32_t applied_count {};
    for (DWORD index = 0;; ++index) {
      wchar_t settings_key_name[256] {};
      DWORD settings_key_name_size = static_cast<DWORD>(std::size(settings_key_name));
      const auto enum_status = RegEnumKeyExW(
        per_monitor_settings_key.get(),
        index,
        settings_key_name,
        &settings_key_name_size,
        nullptr,
        nullptr,
        nullptr,
        nullptr
      );
      if (enum_status == ERROR_NO_MORE_ITEMS) {
        break;
      }
      if (enum_status != ERROR_SUCCESS || wcsncmp(settings_key_name, settings_prefix.c_str(), settings_prefix.size()) != 0) {
        continue;
      }

      NativeRegistryKey settings_key;
      if (RegOpenKeyExW(per_monitor_settings_key.get(), settings_key_name, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, settings_key.put()) != ERROR_SUCCESS) {
        continue;
      }

      // Never clobber a slot that already holds a different user-customized
      // value. That slot is either a sibling source we have no business
      // rewriting, or the slot the user is actively adjusting while this
      // 20x/5s retain loop runs - overwriting it would fight the user's live
      // change. Slots that are absent, default (DpiValue == 0), or already
      // equal are safe to (re)seed with the retained value.
      DWORD existing_value {};
      if (read_registry_dword(settings_key.get(), L"DpiValue", existing_value) &&
          existing_value != 0 && existing_value != dpi_value) {
        continue;
      }

      if (RegSetValueExW(
            settings_key.get(),
            L"DpiValue",
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE *>(&dpi_value),
            sizeof(dpi_value)
          ) == ERROR_SUCCESS) {
        ++applied_count;
      }
    }

    if (applied_count == 0) {
      return false;
    }

    TraceLoggingWrite(
      g_trace_provider,
      "TemporaryDpiSettingsRetained",
      TraceLoggingUInt64(descriptor.display_id, "DisplayId"),
      TraceLoggingUInt32(dpi_value, "DpiValue"),
      TraceLoggingUInt32(applied_count, "AppliedCount")
    );
    return true;
  }

  bool retain_temporary_display_dpi_settings(
    const vdd::DisplayDescriptor &descriptor,
    const std::optional<DWORD> retained_dpi_value = std::nullopt
  ) {
    if (retained_dpi_value) {
      return apply_retained_temporary_dpi_value(descriptor, *retained_dpi_value);
    }

    const auto dpi_value = read_retained_temporary_dpi_value(descriptor);
    return dpi_value && apply_retained_temporary_dpi_value(descriptor, *dpi_value);
  }

  std::wstring guid_registry_string(const vdd::Guid &guid) {
    wchar_t text[39] {};
    std::swprintf(
      text,
      std::size(text),
      L"{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
      static_cast<unsigned int>(guid.data1),
      static_cast<unsigned int>(guid.data2),
      static_cast<unsigned int>(guid.data3),
      static_cast<unsigned int>(guid.data4[0]),
      static_cast<unsigned int>(guid.data4[1]),
      static_cast<unsigned int>(guid.data4[2]),
      static_cast<unsigned int>(guid.data4[3]),
      static_cast<unsigned int>(guid.data4[4]),
      static_cast<unsigned int>(guid.data4[5]),
      static_cast<unsigned int>(guid.data4[6]),
      static_cast<unsigned int>(guid.data4[7])
    );
    return text;
  }

  bool equals_ci(const std::wstring &left, const std::wstring &right) {
    return left.size() == right.size() && _wcsicmp(left.c_str(), right.c_str()) == 0;
  }

  std::optional<std::wstring> profile_association_path_from_driver_value(const std::wstring &driver_value) {
    const auto slash = driver_value.rfind(L'\\');
    if (slash == std::wstring::npos || slash + 1 >= driver_value.size()) {
      return std::nullopt;
    }

    return L"Software\\Microsoft\\Windows NT\\CurrentVersion\\ICM\\ProfileAssociations\\Display\\" + driver_value;
  }

  std::optional<std::wstring> monitor_driver_value_for_identity(
    const vdd::DisplayDescriptor &descriptor,
    const std::uint32_t target_id
  ) {
    const auto hardware_id = temporary_monitor_hardware_id(descriptor);
    NativeRegistryKey hardware_key;
    const auto hardware_path = L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\" + hardware_id;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, hardware_path.c_str(), 0, KEY_ENUMERATE_SUB_KEYS, hardware_key.put()) != ERROR_SUCCESS) {
      return std::nullopt;
    }

    const auto expected_container_id = guid_registry_string(descriptor.container_id);
    std::optional<std::wstring> fallback_driver;
    for (DWORD index = 0;; ++index) {
      wchar_t instance_name[256] {};
      DWORD instance_name_size = static_cast<DWORD>(std::size(instance_name));
      const auto enum_status = RegEnumKeyExW(hardware_key.get(), index, instance_name, &instance_name_size, nullptr, nullptr, nullptr, nullptr);
      if (enum_status == ERROR_NO_MORE_ITEMS) {
        break;
      }
      if (enum_status != ERROR_SUCCESS) {
        continue;
      }

      NativeRegistryKey instance_key;
      if (RegOpenKeyExW(hardware_key.get(), instance_name, 0, KEY_QUERY_VALUE, instance_key.put()) != ERROR_SUCCESS) {
        continue;
      }

      std::wstring container_id;
      if (!read_registry_string(instance_key.get(), L"ContainerID", container_id) ||
          !equals_ci(container_id, expected_container_id)) {
        continue;
      }

      std::wstring driver_value;
      if (!read_registry_string(instance_key.get(), L"Driver", driver_value)) {
        continue;
      }

      DWORD address {};
      if (read_registry_dword(instance_key.get(), L"Address", address) && address == target_id) {
        return driver_value;
      }

      fallback_driver = driver_value;
    }

    return fallback_driver;
  }

  bool monitor_driver_value_matches_identity(
    const std::wstring &driver_value,
    const std::wstring &hardware_id,
    const std::wstring &container_id
  ) {
    NativeRegistryKey hardware_key;
    const auto hardware_path = L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\" + hardware_id;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, hardware_path.c_str(), 0, KEY_ENUMERATE_SUB_KEYS, hardware_key.put()) != ERROR_SUCCESS) {
      return false;
    }

    for (DWORD index = 0;; ++index) {
      wchar_t instance_name[256] {};
      DWORD instance_name_size = static_cast<DWORD>(std::size(instance_name));
      const auto enum_status = RegEnumKeyExW(hardware_key.get(), index, instance_name, &instance_name_size, nullptr, nullptr, nullptr, nullptr);
      if (enum_status == ERROR_NO_MORE_ITEMS) {
        break;
      }
      if (enum_status != ERROR_SUCCESS) {
        continue;
      }

      NativeRegistryKey instance_key;
      if (RegOpenKeyExW(hardware_key.get(), instance_name, 0, KEY_QUERY_VALUE, instance_key.put()) != ERROR_SUCCESS) {
        continue;
      }

      std::wstring candidate_driver;
      std::wstring candidate_container;
      if (read_registry_string(instance_key.get(), L"Driver", candidate_driver) &&
          read_registry_string(instance_key.get(), L"ContainerID", candidate_container) &&
          equals_ci(candidate_driver, driver_value) &&
          equals_ci(candidate_container, container_id)) {
        return true;
      }
    }

    return false;
  }

  void migrate_hdr_profile_association_for_monitor(
    const vdd::DisplayDescriptor descriptor,
    const std::uint32_t target_id
  ) {
    if (!descriptor.retain_identity) {
      return;
    }

    const auto sid = active_console_user_sid_string();
    if (!sid) {
      return;
    }

    const auto current_driver = monitor_driver_value_for_identity(descriptor, target_id);
    if (!current_driver) {
      return;
    }

    const auto current_profile_path = profile_association_path_from_driver_value(*current_driver);
    if (!current_profile_path) {
      return;
    }

    NativeRegistryKey current_profile_key;
    const auto current_user_path = *sid + L"\\" + *current_profile_path;
    if (RegCreateKeyExW(
          HKEY_USERS,
          current_user_path.c_str(),
          0,
          nullptr,
          REG_OPTION_NON_VOLATILE,
          KEY_QUERY_VALUE | KEY_SET_VALUE,
          nullptr,
          current_profile_key.put(),
          nullptr
        ) != ERROR_SUCCESS) {
      return;
    }

    DWORD current_profile_type {};
    if (!read_registry_profile_value(current_profile_key.get(), L"ICMProfileAC", current_profile_type).empty()) {
      return;
    }

    const auto current_slash = current_driver->rfind(L'\\');
    if (current_slash == std::wstring::npos) {
      return;
    }

    const auto profile_class_path =
      *sid +
      L"\\Software\\Microsoft\\Windows NT\\CurrentVersion\\ICM\\ProfileAssociations\\Display\\" +
      current_driver->substr(0, current_slash);

    NativeRegistryKey profile_class_key;
    if (RegOpenKeyExW(HKEY_USERS, profile_class_path.c_str(), 0, KEY_ENUMERATE_SUB_KEYS, profile_class_key.put()) != ERROR_SUCCESS) {
      return;
    }

    const auto hardware_id = temporary_monitor_hardware_id(descriptor);
    const auto container_id = guid_registry_string(descriptor.container_id);
    for (DWORD index = 0;; ++index) {
      wchar_t profile_key_name[64] {};
      DWORD profile_key_name_size = static_cast<DWORD>(std::size(profile_key_name));
      const auto enum_status = RegEnumKeyExW(profile_class_key.get(), index, profile_key_name, &profile_key_name_size, nullptr, nullptr, nullptr, nullptr);
      if (enum_status == ERROR_NO_MORE_ITEMS) {
        break;
      }
      if (enum_status != ERROR_SUCCESS) {
        continue;
      }

      const auto candidate_driver = current_driver->substr(0, current_slash + 1) + profile_key_name;
      if (equals_ci(candidate_driver, *current_driver) ||
          !monitor_driver_value_matches_identity(candidate_driver, hardware_id, container_id)) {
        continue;
      }

      NativeRegistryKey source_profile_key;
      if (RegOpenKeyExW(profile_class_key.get(), profile_key_name, 0, KEY_QUERY_VALUE, source_profile_key.put()) != ERROR_SUCCESS) {
        continue;
      }

      DWORD source_profile_type {};
      const auto source_profile = read_registry_profile_value(source_profile_key.get(), L"ICMProfileAC", source_profile_type);
      if (source_profile.empty()) {
        continue;
      }

      DWORD use_per_user = 1;
      (void) read_registry_dword(source_profile_key.get(), L"UsePerUserProfiles", use_per_user);
      (void) RegSetValueExW(
        current_profile_key.get(),
        L"UsePerUserProfiles",
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE *>(&use_per_user),
        sizeof(use_per_user)
      );

      if (RegSetValueExW(
            current_profile_key.get(),
            L"ICMProfileAC",
            0,
            source_profile_type,
            reinterpret_cast<const BYTE *>(source_profile.data()),
            static_cast<DWORD>(source_profile.size())
          ) == ERROR_SUCCESS) {
        TraceLoggingWrite(
          g_trace_provider,
          "HdrProfileAssociationRetained",
          TraceLoggingUInt64(descriptor.display_id, "DisplayId"),
          TraceLoggingUInt32(target_id, "TargetId")
        );
      }
      return;
    }
  }

  EVT_WDF_WORKITEM hdr_profile_retention_work_item;

  void hdr_profile_retention_work_item(WDFWORKITEM work_item) {
    const auto *context = GetHdrProfileRetentionWorkItemContext(work_item);
    if (context) {
      const auto retained_dpi_value = context->has_retained_dpi_value ?
        std::optional<DWORD> {context->retained_dpi_value} :
        std::nullopt;
      for (std::uint32_t attempt = 0; attempt < 20; ++attempt) {
        (void) retain_temporary_display_dpi_settings(context->descriptor, retained_dpi_value);
        migrate_hdr_profile_association_for_monitor(context->descriptor, context->target_id);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }
    }
    WdfObjectDelete(work_item);
  }

  void schedule_hdr_profile_association_retention(
    WDFDEVICE device,
    const vdd::DisplayDescriptor &descriptor,
    const std::uint32_t target_id,
    const std::optional<DWORD> retained_dpi_value
  ) {
    if (!descriptor.retain_identity) {
      return;
    }

    WDF_WORKITEM_CONFIG config;
    WDF_WORKITEM_CONFIG_INIT(&config, hdr_profile_retention_work_item);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, HdrProfileRetentionWorkItemContext);
    attributes.ParentObject = device;

    WDFWORKITEM work_item {};
    if (!NT_SUCCESS(WdfWorkItemCreate(&config, &attributes, &work_item))) {
      return;
    }

    auto *context = GetHdrProfileRetentionWorkItemContext(work_item);
    context->descriptor = descriptor;
    context->target_id = target_id;
    if (retained_dpi_value) {
      context->retained_dpi_value = *retained_dpi_value;
      context->has_retained_dpi_value = true;
    }
    WdfWorkItemEnqueue(work_item);
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
    shape.refresh_rate_millihz = vdd::clamp_windows_driver_u32(derived_refresh_millihz);

    return shape;
  }

  ModeShape mode_shape_from_descriptor(const vdd::DisplayDescriptor &descriptor) {
    if (descriptor.width == 0 || descriptor.height == 0 || descriptor.refresh_rate_millihz == 0) {
      return {};
    }

    return vdd::active_windows_driver_mode_shape(descriptor.width, descriptor.height, descriptor.refresh_rate_millihz);
  }

  std::mutex g_monitor_description_modes_mutex;
  vdd::WindowsDriverRegisteredModeStore g_monitor_description_modes;

  std::optional<std::array<std::byte, vdd::kEdidSize>> monitor_description_key(
    const IDDCX_MONITOR_DESCRIPTION &description
  ) {
    if (description.Type != IDDCX_MONITOR_DESCRIPTION_TYPE_EDID ||
        !description.pData ||
        description.DataSize < vdd::kEdidSize) {
      return std::nullopt;
    }

    std::array<std::byte, vdd::kEdidSize> key {};
    const auto *data = static_cast<const std::byte *>(description.pData);
    std::copy_n(data, key.size(), key.begin());
    return key;
  }

  void register_monitor_description_mode(const vdd::DisplayDescriptor &descriptor) {
    const auto mode = mode_shape_from_descriptor(descriptor);
    if (mode.width == 0 || mode.height == 0 || mode.refresh_rate_millihz == 0) {
      return;
    }

    std::lock_guard lock {g_monitor_description_modes_mutex};
    (void) g_monitor_description_modes.register_mode(descriptor.edid, mode);
  }

  void unregister_monitor_description_mode(const vdd::DisplayDescriptor &descriptor) {
    std::lock_guard lock {g_monitor_description_modes_mutex};
    g_monitor_description_modes.unregister_mode(descriptor.edid);
  }

  std::optional<ModeShape> registered_mode_shape_from_description(
    const IDDCX_MONITOR_DESCRIPTION &description
  ) {
    const auto key = monitor_description_key(description);
    if (!key) {
      return std::nullopt;
    }

    std::lock_guard lock {g_monitor_description_modes_mutex};
    return g_monitor_description_modes.registered_mode(*key);
  }

  std::optional<ModeShape> preferred_mode_shape_from_description(const IDDCX_MONITOR_DESCRIPTION &description) {
    if (const auto registered = registered_mode_shape_from_description(description)) {
      return registered;
    }

    return mode_shape_from_description(description);
  }

  IDDCX_BITS_PER_COMPONENT preferred_rgb_bits_per_component() {
    const auto capabilities = vdd::hdr_output_capabilities();
    if (capabilities.output_bits.rgb_10bpc) {
      return IDDCX_BITS_PER_COMPONENT_10;
    }
    if (capabilities.output_bits.rgb_8bpc) {
      return IDDCX_BITS_PER_COMPONENT_8;
    }
    return IDDCX_BITS_PER_COMPONENT_NONE;
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
    const auto frequencies = vdd::windows_driver_signal_frequencies(shape);
    signal.vSyncFreq = {frequencies.vertical.numerator, frequencies.vertical.denominator};
    signal.hSyncFreq = {frequencies.horizontal.numerator, frequencies.horizontal.denominator};
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
    populate_rgb_wire_bits(mode.BitsPerComponent, preferred_rgb_bits_per_component());
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
    populate_rgb_wire_bits(mode.BitsPerComponent, preferred_rgb_bits_per_component());
    return mode;
  }

  NTSTATUS fill_monitor_modes(
    const IDARG_IN_PARSEMONITORDESCRIPTION *input,
    IDARG_OUT_PARSEMONITORDESCRIPTION *output
  ) {
    if (!input || !output) {
      return STATUS_INVALID_PARAMETER;
    }

    const auto [modes, preferred_index] =
      vdd::build_windows_driver_mode_shapes(preferred_mode_shape_from_description(input->MonitorDescription));
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

    const auto [modes, preferred_index] =
      vdd::build_windows_driver_mode_shapes(preferred_mode_shape_from_description(input->MonitorDescription));
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

    const auto [modes, preferred_index] = vdd::build_windows_driver_mode_shapes(std::nullopt);
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

    const auto [modes, preferred_index] = vdd::build_windows_driver_target_mode_shapes(
      mode_shape_from_description(input->MonitorDescription),
      requested_shape
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

    const auto [modes, preferred_index] = vdd::build_windows_driver_target_mode_shapes(
      mode_shape_from_description(input->MonitorDescription),
      requested_shape
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

  bool is_access_lost_hresult(const HRESULT hr) {
    return hr == DXGI_ERROR_ACCESS_LOST;
  }

  class UniqueHandle {
  public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle):
        handle_ {handle} {
    }
    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    UniqueHandle(UniqueHandle &&other) noexcept:
        handle_ {std::exchange(other.handle_, nullptr)} {
    }
    UniqueHandle &operator=(UniqueHandle &&other) noexcept {
      if (this != &other) {
        reset(std::exchange(other.handle_, nullptr));
      }
      return *this;
    }
    ~UniqueHandle() {
      reset();
    }

    HANDLE get() const {
      return handle_;
    }

    explicit operator bool() const {
      return handle_ && handle_ != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE handle = nullptr) {
      if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
      }
      handle_ = handle;
    }

  private:
    HANDLE handle_ {};
  };

  class WdfObjectReferenceGuard {
  public:
    WdfObjectReferenceGuard() = default;
    explicit WdfObjectReferenceGuard(WDFOBJECT object):
        object_ {object} {
      if (object_) {
        WdfObjectReference(object_);
      }
    }
    static WdfObjectReferenceGuard adopt(WDFOBJECT object) {
      WdfObjectReferenceGuard guard;
      guard.object_ = object;
      return guard;
    }
    WdfObjectReferenceGuard(const WdfObjectReferenceGuard &) = delete;
    WdfObjectReferenceGuard &operator=(const WdfObjectReferenceGuard &) = delete;
    WdfObjectReferenceGuard(WdfObjectReferenceGuard &&other) noexcept:
        object_ {std::exchange(other.object_, nullptr)} {
    }
    WdfObjectReferenceGuard &operator=(WdfObjectReferenceGuard &&other) noexcept {
      if (this != &other) {
        reset();
        object_ = std::exchange(other.object_, nullptr);
      }
      return *this;
    }
    ~WdfObjectReferenceGuard() {
      reset();
    }

    void reset() {
      if (object_) {
        WdfObjectDereference(object_);
        object_ = nullptr;
      }
    }

  private:
    WDFOBJECT object_ {};
  };

  class CursorProcessor {
  public:
    explicit CursorProcessor(IDDCX_MONITOR monitor):
        monitor_ {monitor},
        shape_buffer_(kHardwareCursorShapeBufferBytes) {
    }

    ~CursorProcessor() {
      stop();
    }

    CursorProcessor(const CursorProcessor &) = delete;
    CursorProcessor &operator=(const CursorProcessor &) = delete;

    NTSTATUS start() {
      if (!monitor_) {
        return STATUS_INVALID_PARAMETER;
      }

      cursor_event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
      if (!cursor_event_) {
        return STATUS_INSUFFICIENT_RESOURCES;
      }

      IDARG_IN_SETUP_HWCURSOR setup {};
      setup.CursorInfo.Size = sizeof(setup.CursorInfo);
      setup.CursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_EMULATION;
      setup.CursorInfo.AlphaCursorSupport = TRUE;
      setup.CursorInfo.MaxX = kHardwareCursorMaxWidth;
      setup.CursorInfo.MaxY = kHardwareCursorMaxHeight;
      setup.hNewCursorDataAvailable = cursor_event_.get();

      const NTSTATUS status = IddCxMonitorSetupHardwareCursor(monitor_, &setup);
      if (!NT_SUCCESS(status)) {
        TraceLoggingWrite(
          g_trace_provider,
          "HardwareCursorSetupFailed",
          TraceLoggingInt32(status, "Status")
        );
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_CURSOR, "HardwareCursorSetupFailed");
        return status;
      }

      try {
        worker_ = std::thread([this]() {
          cursor_loop();
        });
      } catch (...) {
        TraceLoggingWrite(g_trace_provider, "HardwareCursorWorkerStartFailed");
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_CURSOR, "HardwareCursorWorkerStartFailed");
        return STATUS_INSUFFICIENT_RESOURCES;
      }

      TraceLoggingWrite(
        g_trace_provider,
        "HardwareCursorEnabled",
        TraceLoggingUInt32(kHardwareCursorMaxWidth, "MaxWidth"),
        TraceLoggingUInt32(kHardwareCursorMaxHeight, "MaxHeight")
      );
      TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CURSOR, "HardwareCursorEnabled");
      return STATUS_SUCCESS;
    }

    void stop() {
      stop_requested_.store(true, std::memory_order_release);
      if (cursor_event_) {
        SetEvent(cursor_event_.get());
      }
      if (worker_.joinable()) {
        worker_.join();
      }
      cursor_event_.reset();
    }

    bool stop_for_teardown(const std::chrono::milliseconds timeout) {
      stop_requested_.store(true, std::memory_order_release);
      if (cursor_event_) {
        SetEvent(cursor_event_.get());
      }
      if (!worker_.joinable()) {
        cursor_event_.reset();
        return true;
      }

      const auto wait_ms = static_cast<DWORD>((std::min<std::int64_t>)(
        (std::max<std::int64_t>)(timeout.count(), 0),
        MAXDWORD
      ));
      const DWORD wait_result = WaitForSingleObject(worker_.native_handle(), wait_ms);
      if (wait_result == WAIT_OBJECT_0) {
        worker_.join();
        cursor_event_.reset();
        return true;
      }

      TraceLoggingWrite(
        g_trace_provider,
        "HardwareCursorWorkerTeardownDeferred",
        TraceLoggingUInt32(wait_result, "WaitResult"),
        TraceLoggingUInt32(GetLastError(), "LastError")
      );
      TraceEvents(TRACE_LEVEL_WARNING, TRACE_CURSOR, "HardwareCursorWorkerTeardownDeferred");
      return false;
    }

    void detach_worker_for_leak() {
      if (worker_.joinable()) {
        worker_.detach();
      }
    }

    bool stop_for_monitor_departure() {
      if (stop_for_teardown(kCursorProcessorTeardownTimeout)) {
        return true;
      }

      TraceLoggingWrite(g_trace_provider, "HardwareCursorWorkerDepartureBlocked");
      TraceEvents(TRACE_LEVEL_WARNING, TRACE_CURSOR, "HardwareCursorWorkerDepartureBlocked");
      return false;
    }

  private:
    void cursor_loop() {
      (void) query_cursor();
      while (!stop_requested_.load(std::memory_order_acquire)) {
        const DWORD wait_result = WaitForSingleObject(cursor_event_.get(), 1000);
        if (stop_requested_.load(std::memory_order_acquire)) {
          break;
        }
        if (wait_result == WAIT_TIMEOUT) {
          continue;
        }
        if (wait_result != WAIT_OBJECT_0) {
          TraceLoggingWrite(
            g_trace_provider,
            "HardwareCursorWaitFailed",
            TraceLoggingUInt32(wait_result, "WaitResult"),
            TraceLoggingUInt32(GetLastError(), "LastError")
          );
          TraceEvents(TRACE_LEVEL_WARNING, TRACE_CURSOR, "HardwareCursorWaitFailed");
          return;
        }

        (void) query_cursor();
      }
    }

    NTSTATUS query_cursor() {
      IDARG_IN_QUERY_HWCURSOR input {};
      input.LastShapeId = last_shape_id_;
      input.ShapeBufferSizeInBytes = static_cast<UINT>(shape_buffer_.size());
      input.pShapeBuffer = shape_buffer_.data();

      if (IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorQueryHardwareCursor3)) {
        IDARG_OUT_QUERY_HWCURSOR3 output {};
        const HRESULT hr = IddCxMonitorQueryHardwareCursor3(monitor_, &input, &output);
        if (SUCCEEDED(hr)) {
          update_cursor_state(output.IsCursorVisible, output.X, output.Y, output.IsCursorShapeUpdated, output.CursorShapeInfo);
        }
        return cursor_query_status(hr);
      }

      if (IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorQueryHardwareCursor2)) {
        IDARG_OUT_QUERY_HWCURSOR2 output {};
        const NTSTATUS status = IddCxMonitorQueryHardwareCursor2(monitor_, &input, &output);
        if (NT_SUCCESS(status)) {
          update_cursor_state(output.IsCursorVisible, output.X, output.Y, output.IsCursorShapeUpdated, output.CursorShapeInfo);
        }
        return cursor_query_status(status);
      }

      IDARG_OUT_QUERY_HWCURSOR output {};
      const NTSTATUS status = IddCxMonitorQueryHardwareCursor(monitor_, &input, &output);
      if (NT_SUCCESS(status)) {
        update_cursor_state(output.IsCursorVisible, output.X, output.Y, output.IsCursorShapeUpdated, output.CursorShapeInfo);
      }
      return cursor_query_status(status);
    }

    NTSTATUS cursor_query_status(const HRESULT status) {
      const auto ntstatus = static_cast<NTSTATUS>(status);
      if (NT_SUCCESS(ntstatus) || ntstatus == STATUS_GRAPHICS_PATH_NOT_IN_TOPOLOGY) {
        return ntstatus;
      }

      TraceLoggingWrite(
        g_trace_provider,
        "HardwareCursorQueryFailed",
        TraceLoggingInt32(ntstatus, "Status")
      );
      TraceEvents(TRACE_LEVEL_WARNING, TRACE_CURSOR, "HardwareCursorQueryFailed");
      return ntstatus;
    }

    void update_cursor_state(
      const BOOL visible,
      const INT x,
      const INT y,
      const BOOL shape_updated,
      const IDDCX_CURSOR_SHAPE_INFO &shape_info
    ) {
      cursor_visible_ = visible != FALSE;
      cursor_x_ = x;
      cursor_y_ = y;
      if (shape_updated) {
        last_shape_id_ = shape_info.ShapeId;
        cursor_shape_ = shape_info;
      }
    }

    IDDCX_MONITOR monitor_ {};
    UniqueHandle cursor_event_ {};
    std::thread worker_ {};
    std::atomic<bool> stop_requested_ {false};
    std::vector<std::uint8_t> shape_buffer_ {};
    DWORD last_shape_id_ {};
    IDDCX_CURSOR_SHAPE_INFO cursor_shape_ {};
    bool cursor_visible_ {};
    INT cursor_x_ {};
    INT cursor_y_ {};
  };

  void stop_cursor_processor(std::unique_ptr<CursorProcessor> &processor) {
    if (!processor) {
      return;
    }

    if (processor->stop_for_teardown(kCursorProcessorTeardownTimeout)) {
      processor.reset();
      return;
    }

    TraceLoggingWrite(g_trace_provider, "HardwareCursorWorkerTeardownAbandoned");
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_CURSOR, "HardwareCursorWorkerTeardownAbandoned");
    processor->detach_worker_for_leak();
    (void) processor.release();
  }

  bool try_stop_cursor_processor_for_monitor_departure(std::unique_ptr<CursorProcessor> &processor) {
    if (!processor) {
      return true;
    }

    if (processor->stop_for_monitor_departure()) {
      processor.reset();
      return true;
    }

    return false;
  }

  class SwapChainProcessor {
  public:
    SwapChainProcessor(IDDCX_SWAPCHAIN swapchain, HANDLE next_surface_available):
        swapchain_ {swapchain},
        next_surface_available_ {next_surface_available},
        referenced_swapchain_ {reinterpret_cast<WDFOBJECT>(swapchain)} {
      if (referenced_swapchain_) {
        WdfObjectReference(referenced_swapchain_);
      }
      // Private manual-reset stop event. The worker waits on BOTH this and the
      // OS-supplied frame event; teardown signals this one. Never signal the OS
      // frame event (next_surface_available_) to request stop -- it is owned by
      // IddCx and manufacturing signals on it is outside the driver contract.
      stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~SwapChainProcessor() {
      stop();
      delete_swapchain();
      if (stop_event_) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
      }
    }

    SwapChainProcessor(const SwapChainProcessor &) = delete;
    SwapChainProcessor &operator=(const SwapChainProcessor &) = delete;

    HRESULT start(const LUID &render_adapter_luid) {
      try {
        worker_ = std::thread([this, render_adapter_luid]() {
          try {
            process_frames(render_adapter_luid);
          } catch (...) {
            delete_swapchain();
          }
        });
      } catch (...) {
        return E_OUTOFMEMORY;
      }

      // Do NOT block here waiting for the worker to create its D3D render device.
      // start() runs inside the IddCx EvtIddCxMonitorAssignSwapChain callback. Creating
      // a D3D11 device on the render adapter (create_dxgi_device_for_luid ->
      // D3D11CreateDevice) while that callback is in flight serializes against the OS
      // display stack and deadlocks with concurrent D3D11CreateDevice calls coming from a
      // capture client on the same adapter (e.g. Sunshine's capture reinit when the
      // virtual display topology changes). That stalls D3D11CreateDevice in
      // ZwWaitForAlertByThreadId, wedges capture reinit, and ultimately trips the host's
      // session-teardown watchdog. The WDK IddCx sample likewise starts the worker and
      // returns immediately; device creation and teardown stay entirely on the worker
      // thread, and a creation failure simply stops frame processing for this swapchain.
      return S_OK;
    }

    void stop() {
      request_stop();
      if (worker_.joinable()) {
        worker_.join();
      }
      dxgi_device_.Reset();
      device_.Reset();
    }

    bool stop_for_teardown(const std::chrono::milliseconds timeout) {
      request_stop();
      if (!worker_.joinable()) {
        dxgi_device_.Reset();
        device_.Reset();
        return true;
      }

      const auto wait_ms = static_cast<DWORD>((std::min<std::int64_t>)(
        (std::max<std::int64_t>)(timeout.count(), 0),
        MAXDWORD
      ));
      const DWORD wait_result = WaitForSingleObject(worker_.native_handle(), wait_ms);
      if (wait_result == WAIT_OBJECT_0) {
        worker_.join();
        dxgi_device_.Reset();
        device_.Reset();
        return true;
      }

      TraceLoggingWrite(
        g_trace_provider,
        "SwapChainWorkerTeardownDeferred",
        TraceLoggingUInt32(wait_result, "WaitResult"),
        TraceLoggingUInt32(GetLastError(), "LastError")
      );
      TraceEvents(TRACE_LEVEL_WARNING, TRACE_SWAPCHAIN, "SwapChainWorkerTeardownDeferred");
      return false;
    }

    bool has_stopped() {
      if (!worker_.joinable()) {
        return true;
      }

      return WaitForSingleObject(worker_.native_handle(), 0) == WAIT_OBJECT_0;
    }

    void detach_worker_for_leak() {
      if (worker_.joinable()) {
        worker_.detach();
      }
    }

    void request_stop() {
      stop_requested_.store(true, std::memory_order_release);
      if (stop_event_) {
        SetEvent(stop_event_);
      }
      assignment_changed_.notify_all();
    }

    void mark_assign_succeeded() {
      complete_assign(true);
    }

    void mark_assign_abandoned() {
      complete_assign(false);
    }

    void abandon_swapchain() {
      {
        std::lock_guard lock {assignment_mutex_};
        assign_completed_ = true;
        driver_owns_swapchain_ = false;
        swapchain_ = nullptr;
      }
      assignment_changed_.notify_all();
      release_swapchain_reference();
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

    void complete_assign(const bool driver_owns_swapchain) {
      {
        std::lock_guard lock {assignment_mutex_};
        assign_completed_ = true;
        driver_owns_swapchain_ = driver_owns_swapchain;
      }
      assignment_changed_.notify_all();
    }

    IDDCX_SWAPCHAIN claim_owned_swapchain_for_delete(bool &assignment_pending) {
      assignment_pending = false;
      std::unique_lock lock {assignment_mutex_};
      if (!assignment_changed_.wait_for(lock, kSwapchainProcessorTeardownTimeout, [this]() {
        return assign_completed_;
      })) {
        assignment_pending = true;
        TraceLoggingWrite(g_trace_provider, "SwapChainDeleteDeferredForAssignCompletion");
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_SWAPCHAIN, "SwapChainDeleteDeferredForAssignCompletion");
        return nullptr;
      }

      if (!driver_owns_swapchain_) {
        swapchain_ = nullptr;
        return nullptr;
      }

      driver_owns_swapchain_ = false;
      return std::exchange(swapchain_, nullptr);
    }

    void release_swapchain_reference() {
      WDFOBJECT referenced_swapchain {};
      {
        std::lock_guard lock {assignment_mutex_};
        referenced_swapchain = std::exchange(referenced_swapchain_, nullptr);
      }

      if (referenced_swapchain) {
        WdfObjectDereference(referenced_swapchain);
      }
    }

    HRESULT wait_for_assignment_commit() {
      std::unique_lock lock {assignment_mutex_};
      assignment_changed_.wait(lock, [this]() {
        return assign_completed_ || stop_requested_.load(std::memory_order_acquire);
      });

      if (!assign_completed_ || !driver_owns_swapchain_ || !swapchain_) {
        TraceLoggingWrite(g_trace_provider, "SwapChainSetDeviceSkippedBeforeAssignCommit");
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_SWAPCHAIN, "SwapChainSetDeviceSkippedBeforeAssignCommit");
        return HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
      }

      return S_OK;
    }

    void delete_swapchain() {
      bool assignment_pending = false;
      if (const auto swapchain = claim_owned_swapchain_for_delete(assignment_pending)) {
        // A successful AssignSwapChain transfers hSwapChain ownership to the
        // driver. Closing the WDF object is what releases any frame still owned
        // after the worker exits; abandon_swapchain() is only for callbacks we
        // return to IddCx as ABANDON_SWAPCHAIN.
        WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapchain));
      }
      if (assignment_pending) {
        // The worker has stopped using the raw swapchain handle, but ownership
        // was not resolved within the bounded cleanup window. Do not delete a
        // swapchain that IddCx may still own; only drop our lifetime pin.
        release_swapchain_reference();
        return;
      }
      release_swapchain_reference();
    }

    void log_render_device_lost(const char *operation, const HRESULT hr) const {
      const HRESULT removed_reason = device_ ? device_->GetDeviceRemovedReason() : E_FAIL;
      TraceLoggingWrite(
        g_trace_provider,
        "RenderDeviceLost",
        TraceLoggingString(operation, "Operation"),
        TraceLoggingUInt32(static_cast<std::uint32_t>(hr), "HResult"),
        TraceLoggingUInt32(static_cast<std::uint32_t>(removed_reason), "DeviceRemovedReason")
      );
      TraceEvents(TRACE_LEVEL_WARNING, TRACE_SWAPCHAIN, "RenderDeviceLost");
    }

    HRESULT assign_swapchain_device() {
      HRESULT hr = wait_for_assignment_commit();
      if (FAILED(hr)) {
        return hr;
      }

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
      if (FAILED(hr)) {
        if (stop_requested_.load(std::memory_order_acquire)) {
          return;
        }
        delete_swapchain();
        return;
      }
      if (stop_requested_.load(std::memory_order_acquire)) {
        return;
      }

      while (!stop_requested_.load(std::memory_order_acquire)) {
        HANDLE wait_handles[2] = {next_surface_available_, stop_event_};
        const DWORD handle_count = stop_event_ ? 2u : 1u;
        const DWORD wait_result = WaitForMultipleObjects(handle_count, wait_handles, FALSE, 1000);
        if (stop_requested_.load(std::memory_order_acquire)) {
          break;
        }
        if (wait_result == WAIT_TIMEOUT) {
          continue;
        }
        if (handle_count == 2 && wait_result == WAIT_OBJECT_0 + 1) {
          // stop_event_ signaled by teardown; stop_requested_ is set before the
          // event so the check above normally already broke -- exit defensively.
          break;
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
            if (stop_requested_.load(std::memory_order_acquire)) {
              TraceLoggingWrite(
                g_trace_provider,
                "SwapChainAcquireStoppedDuringTeardown",
                TraceLoggingUInt32(static_cast<std::uint32_t>(acquire_result), "HResult")
              );
              TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_SWAPCHAIN, "SwapChainAcquireStoppedDuringTeardown");
              return;
            }
            if (is_access_lost_hresult(acquire_result)) {
              // A virtual-display mode change (alt-tab / topology churn) raises
              // ACCESS_LOST. Do NOT destroy+recreate the render device here: a
              // D3D11 device create/destroy on the render adapter while the OS is
              // mid mode-commit serializes against the display stack and wedges the
              // host's concurrent D3DKMTDestroyHwQueue (the alt-tab deadlock). Match
              // the canonical IddCx drivers (SudoVDA / WDK sample): exit the worker
              // and let the OS re-assign a fresh swapchain -- a fresh device is
              // created at assign time, after the mode has settled.
              log_render_device_lost("ReleaseAndAcquireBuffer", acquire_result);
              delete_swapchain();
              return;
            }
            if (is_device_lost_hresult(acquire_result)) {
              log_render_device_lost("ReleaseAndAcquireBuffer", acquire_result);
              delete_swapchain();
              return;
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
          HRESULT finished_result = IddCxSwapChainFinishedProcessingFrame(swapchain_);
          if (FAILED(finished_result)) {
            if (stop_requested_.load(std::memory_order_acquire)) {
              TraceLoggingWrite(
                g_trace_provider,
                "SwapChainFinishedStoppedDuringTeardown",
                TraceLoggingUInt32(static_cast<std::uint32_t>(finished_result), "HResult")
              );
              TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_SWAPCHAIN, "SwapChainFinishedStoppedDuringTeardown");
              return;
            }
            if (is_access_lost_hresult(finished_result)) {
              // Same as the acquire path: never destroy+recreate the device during
              // an in-flight mode change. Exit and let the OS re-assign.
              log_render_device_lost("FinishedProcessingFrame", finished_result);
              delete_swapchain();
              return;
            }
            if (is_device_lost_hresult(finished_result)) {
              log_render_device_lost("FinishedProcessingFrame", finished_result);
              delete_swapchain();
              return;
            }
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

    IDDCX_SWAPCHAIN swapchain_ {};
    HANDLE next_surface_available_ {};
    HANDLE stop_event_ {};
    std::atomic<bool> stop_requested_ {false};
    std::thread worker_ {};
    std::mutex assignment_mutex_ {};
    std::condition_variable assignment_changed_ {};
    bool assign_completed_ {};
    bool driver_owns_swapchain_ {};
    WDFOBJECT referenced_swapchain_ {};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device_;
  };

  enum class DeferredSwapChainCleanup {
    CloseOwned,
    AbandonOwned
  };

  void defer_stop_swapchain_processor(
    std::unique_ptr<SwapChainProcessor> processor,
    const DeferredSwapChainCleanup cleanup = DeferredSwapChainCleanup::CloseOwned
  ) {
    if (!processor) {
      return;
    }

    if (cleanup == DeferredSwapChainCleanup::AbandonOwned) {
      processor->mark_assign_abandoned();
    }

    try {
      std::thread {[processor = std::move(processor), cleanup]() mutable {
        TraceLoggingWrite(g_trace_provider, "SwapChainWorkerDeferredCleanupStarted");
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_SWAPCHAIN, "SwapChainWorkerDeferredCleanupStarted");
        if (!processor->stop_for_teardown(kSwapchainProcessorTeardownTimeout)) {
          TraceLoggingWrite(g_trace_provider, "SwapChainWorkerDeferredCleanupBlocked");
          TraceEvents(TRACE_LEVEL_WARNING, TRACE_SWAPCHAIN, "SwapChainWorkerDeferredCleanupBlocked");
          processor->detach_worker_for_leak();
          (void) processor.release();
          return;
        }
        if (cleanup == DeferredSwapChainCleanup::AbandonOwned) {
          processor->abandon_swapchain();
        }
        processor.reset();
        TraceLoggingWrite(g_trace_provider, "SwapChainWorkerDeferredCleanupComplete");
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_SWAPCHAIN, "SwapChainWorkerDeferredCleanupComplete");
      }}.detach();
    } catch (...) {
      // If cleanup ownership cannot be transferred, preserve the old safety
      // fallback: leak the backing object so a still-running worker never
      // dereferences freed C++ state.
      processor->detach_worker_for_leak();
      (void) processor.release();
      TraceLoggingWrite(g_trace_provider, "SwapChainWorkerDeferredCleanupFailed");
      TraceEvents(TRACE_LEVEL_WARNING, TRACE_SWAPCHAIN, "SwapChainWorkerDeferredCleanupFailed");
    }
  }

  void stop_swapchain_processor(
    std::unique_ptr<SwapChainProcessor> &processor,
    const DeferredSwapChainCleanup deferred_cleanup = DeferredSwapChainCleanup::CloseOwned
  ) {
    if (!processor) {
      return;
    }

    if (deferred_cleanup == DeferredSwapChainCleanup::AbandonOwned) {
      processor->mark_assign_abandoned();
    }

    if (processor->stop_for_teardown(kSwapchainProcessorTeardownTimeout)) {
      if (deferred_cleanup == DeferredSwapChainCleanup::AbandonOwned) {
        processor->abandon_swapchain();
      }
      processor.reset();
      return;
    }

    // Keep teardown non-blocking for IddCx. Callers that can cross monitor
    // departure clear ownership before deferred cleanup starts so neither the
    // worker nor the destructor deletes a stale IddCx/WDF swapchain handle.
    defer_stop_swapchain_processor(std::move(processor), deferred_cleanup);
  }

  void request_stop_swapchain_processor(std::unique_ptr<SwapChainProcessor> &processor) {
    if (processor) {
      processor->request_stop();
    }
  }

  void stop_swapchain_processors(
    std::vector<std::unique_ptr<SwapChainProcessor>> &processors,
    const DeferredSwapChainCleanup deferred_cleanup = DeferredSwapChainCleanup::CloseOwned
  ) {
    for (auto &processor: processors) {
      stop_swapchain_processor(processor, deferred_cleanup);
    }
    processors.clear();
  }

  void abandon_swapchain_processor_for_shutdown(std::unique_ptr<SwapChainProcessor> &processor) {
    if (!processor) {
      return;
    }

    processor->request_stop();
    processor->mark_assign_abandoned();
    processor->detach_worker_for_leak();
    (void) processor.release();
    TraceLoggingWrite(g_trace_provider, "SwapChainWorkerAbandonedDuringShutdown");
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_SWAPCHAIN, "SwapChainWorkerAbandonedDuringShutdown");
  }

  void abandon_swapchain_processors_for_shutdown(std::vector<std::unique_ptr<SwapChainProcessor>> &processors) {
    for (auto &processor: processors) {
      abandon_swapchain_processor_for_shutdown(processor);
    }
    processors.clear();
  }

  void request_stop_swapchain_processors(std::vector<std::unique_ptr<SwapChainProcessor>> &processors) {
    for (auto &processor: processors) {
      request_stop_swapchain_processor(processor);
    }
  }

  std::chrono::milliseconds remaining_teardown_timeout(
    const std::chrono::steady_clock::time_point deadline
  ) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return std::chrono::milliseconds {0};
    }

    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  }

  bool try_stop_swapchain_processor_for_monitor_departure(
    std::unique_ptr<SwapChainProcessor> &processor,
    const std::chrono::steady_clock::time_point deadline
  ) {
    if (!processor) {
      return true;
    }

    processor->request_stop();
    if (processor->stop_for_teardown(remaining_teardown_timeout(deadline))) {
      processor.reset();
      return true;
    }

    TraceLoggingWrite(g_trace_provider, "SwapChainWorkerDepartureBlocked");
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_SWAPCHAIN, "SwapChainWorkerDepartureBlocked");
    return false;
  }

  bool try_stop_swapchain_processors_for_monitor_departure(
    std::vector<std::unique_ptr<SwapChainProcessor>> &processors,
    const std::chrono::steady_clock::time_point deadline
  ) {
    bool all_stopped = true;
    for (auto &processor: processors) {
      if (!try_stop_swapchain_processor_for_monitor_departure(processor, deadline)) {
        all_stopped = false;
      }
    }
    processors.erase(
      std::remove_if(
        processors.begin(),
        processors.end(),
        [](const auto &processor) {
          return !processor;
        }
      ),
      processors.end()
    );
    return all_stopped;
  }

  bool try_stop_swapchain_processor_for_unassign(
    std::unique_ptr<SwapChainProcessor> &processor,
    const std::chrono::steady_clock::time_point deadline
  ) {
    if (!processor) {
      return true;
    }

    processor->request_stop();
    if (processor->stop_for_teardown(remaining_teardown_timeout(deadline))) {
      processor.reset();
      return true;
    }

    TraceLoggingWrite(g_trace_provider, "SwapChainWorkerUnassignBlocked");
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_SWAPCHAIN, "SwapChainWorkerUnassignBlocked");
    return false;
  }

  bool try_stop_swapchain_processors_for_unassign(
    std::vector<std::unique_ptr<SwapChainProcessor>> &processors,
    const std::chrono::steady_clock::time_point deadline
  ) {
    bool all_stopped = true;
    for (auto &processor: processors) {
      if (!try_stop_swapchain_processor_for_unassign(processor, deadline)) {
        all_stopped = false;
      }
    }
    processors.erase(
      std::remove_if(
        processors.begin(),
        processors.end(),
        [](const auto &processor) {
          return !processor;
        }
      ),
      processors.end()
    );
    return all_stopped;
  }

  std::uint32_t collect_finished_retired_swapchain_processors(
    std::vector<std::unique_ptr<SwapChainProcessor>> &processors,
    std::vector<std::unique_ptr<SwapChainProcessor>> &finished_processors
  ) {
    std::uint32_t cleaned = 0;
    for (auto &processor: processors) {
      if (processor && processor->has_stopped()) {
        finished_processors.push_back(std::move(processor));
        ++cleaned;
        TraceLoggingWrite(g_trace_provider, "RetiredSwapChainProcessorCollected");
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_SWAPCHAIN, "RetiredSwapChainProcessorCollected");
      }
    }
    processors.erase(
      std::remove_if(
        processors.begin(),
        processors.end(),
        [](const auto &processor) {
          return !processor;
        }
      ),
      processors.end()
    );
    return cleaned;
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
      // The virtual sink does not render an endpoint image or apply the OS
      // 3x4 color-space/gamma transform to a downstream panel. Keep the
      // SetGammaRamp callback registered for FP16/HDR diagnostics, but do not
      // advertise software gamma support until the frame path applies it.
      caps.EndPointDiagnostics.GammaSupport = hdr_capabilities.endpoint_gamma_transform ?
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
      const auto dpi_value = read_retained_temporary_dpi_value(descriptor);
      // A nullopt dpi_value is ambiguous: it can mean "the user is on
      // recommended scaling / has no retained value" or "the interactive
      // console hive was momentarily unreachable" (no console user logged in
      // yet, WTSQueryUserToken not ready). Only treat it as a real absence
      // when we can actually reach the console user's hive; on a transient
      // token failure, keep whatever value we already retained so the 20x
      // retry loop can still re-apply it once the user finishes logging in.
      const auto console_user_available = active_console_user_sid_string().has_value();
      const auto result = save_temporary_display_profile(driver_, device_, descriptor);
      {
        std::lock_guard lock {mutex_};
        if (result == vdd::BackendError::None && dpi_value) {
          retained_dpi_values_by_display_id_[descriptor.display_id] = *dpi_value;
        } else if (result != vdd::BackendError::None || console_user_available) {
          retained_dpi_values_by_display_id_.erase(descriptor.display_id);
        }
        // else: transient console-hive unavailability - preserve the existing
        // entry rather than dropping a value we simply could not read.
      }
      return result;
    }

    vdd::BackendError unreserve_temporary_display_identity(const std::uint64_t display_id) override {
      const auto result = remove_temporary_display_profile(driver_, device_, display_id);
      {
        std::lock_guard lock {mutex_};
        retained_dpi_values_by_display_id_.erase(display_id);
      }
      if (result != vdd::BackendError::None) {
        TraceLoggingWrite(g_trace_provider, "TemporaryIdentityUnreserveFailed", TraceLoggingUInt64(display_id, "DisplayId"));
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "TemporaryIdentityUnreserveFailed");
      }
      return result;
    }

    vdd::BackendError depart_temporary_display(const std::uint64_t display_id) override {
      const auto result = depart_display(display_id);
      if (result == vdd::BackendError::None) {
        std::lock_guard lock {mutex_};
        retained_dpi_values_by_display_id_.erase(display_id);
      }
      return result;
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
      WDFOBJECT referenced_adapter {};
      {
        std::lock_guard lock {mutex_};
        preferred_render_adapter_luid_ = preferred_render_adapter;
        adapter = adapter_;
        if (adapter) {
          referenced_adapter = reinterpret_cast<WDFOBJECT>(adapter);
          WdfObjectReference(referenced_adapter);
        }
      }

      if (!adapter) {
        return vdd::BackendError::None;
      }

      struct RenderAdapterState {
        vdd::BackendError result {vdd::BackendError::Failed};
      };
      auto state = std::make_shared<RenderAdapterState>();
      {
        std::lock_guard lock {mutex_};
        ++render_adapter_calls_in_flight_;
      }

      std::thread render_adapter_thread;
      try {
        render_adapter_thread = std::thread([this, adapter, referenced_adapter, preferred_render_adapter, state]() {
          const auto adapter_reference = WdfObjectReferenceGuard::adopt(referenced_adapter);
          IDARG_IN_ADAPTERSETRENDERADAPTER args {};
          args.PreferredRenderAdapter = preferred_render_adapter;
          IddCxAdapterSetRenderAdapter(adapter, &args);
          state->result = vdd::BackendError::None;
          finish_render_adapter_call();
        });
      } catch (...) {
        WdfObjectDereference(referenced_adapter);
        finish_render_adapter_call();
        return vdd::BackendError::Failed;
      }

      const DWORD wait_result = WaitForSingleObject(
        render_adapter_thread.native_handle(),
        static_cast<DWORD>(kRenderAdapterTimeout.count())
      );
      if (wait_result == WAIT_OBJECT_0) {
        render_adapter_thread.join();
        if (state->result != vdd::BackendError::None) {
          return state->result;
        }
      } else if (WaitForSingleObject(render_adapter_thread.native_handle(), 0) == WAIT_OBJECT_0) {
        render_adapter_thread.join();
        if (state->result != vdd::BackendError::None) {
          return state->result;
        }
      } else {
        TraceLoggingWrite(
          g_trace_provider,
          "RenderAdapterPreferenceTimedOut",
          TraceLoggingUInt32(wait_result, "WaitResult"),
          TraceLoggingUInt32(GetLastError(), "LastError")
        );
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "RenderAdapterPreferenceTimedOut");
        {
          std::lock_guard lock {mutex_};
          // Same poison guard as monitor departure: only set unhealthy while the
          // detached worker is still outstanding.
          if (render_adapter_calls_in_flight_ > 0) {
            lifecycle_unhealthy_ = true;
          }
        }
        render_adapter_thread.detach();
        return vdd::BackendError::Failed;
      }

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
      TraceLoggingWrite(
        g_trace_provider,
        "AdapterInitSucceeded",
        TraceLoggingInt32(args->AdapterInitStatus, "Status")
      );
      if (adapter && (preferred_render_adapter.HighPart != 0 || preferred_render_adapter.LowPart != 0)) {
        auto *adapter_object = reinterpret_cast<WDFOBJECT>(adapter);
        WdfObjectReference(adapter_object);
        struct RenderAdapterState {
          vdd::BackendError result {vdd::BackendError::Failed};
        };
        auto state = std::make_shared<RenderAdapterState>();
        {
          std::lock_guard lock {mutex_};
          ++render_adapter_calls_in_flight_;
        }

        std::thread render_adapter_thread;
        try {
          render_adapter_thread = std::thread([this, adapter, adapter_object, preferred_render_adapter, state]() {
            const auto adapter_reference = WdfObjectReferenceGuard::adopt(adapter_object);
            IDARG_IN_ADAPTERSETRENDERADAPTER set_render_adapter {};
            set_render_adapter.PreferredRenderAdapter = preferred_render_adapter;
            IddCxAdapterSetRenderAdapter(adapter, &set_render_adapter);
            state->result = vdd::BackendError::None;
            finish_render_adapter_call();
          });
        } catch (...) {
          WdfObjectDereference(adapter_object);
          finish_render_adapter_call();
          return STATUS_SUCCESS;
        }

        const DWORD wait_result = WaitForSingleObject(
          render_adapter_thread.native_handle(),
          static_cast<DWORD>(kRenderAdapterTimeout.count())
        );
        if (wait_result == WAIT_OBJECT_0 ||
            WaitForSingleObject(render_adapter_thread.native_handle(), 0) == WAIT_OBJECT_0) {
          render_adapter_thread.join();
        } else {
          TraceLoggingWrite(
            g_trace_provider,
            "AdapterInitRenderAdapterPreferenceTimedOut",
            TraceLoggingUInt32(wait_result, "WaitResult"),
            TraceLoggingUInt32(GetLastError(), "LastError")
          );
          TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "AdapterInitRenderAdapterPreferenceTimedOut");
          {
            std::lock_guard lock {mutex_};
            if (render_adapter_calls_in_flight_ > 0) {
              lifecycle_unhealthy_ = true;
            }
          }
          render_adapter_thread.detach();
        }
      }
      return STATUS_SUCCESS;
    }

    NTSTATUS commit_modes(const IDARG_IN_COMMITMODES *args) {
      if (!args || (args->PathCount > 0 && !args->pPaths)) {
        return STATUS_INVALID_PARAMETER;
      }

      for (UINT index = 0; index < args->PathCount; ++index) {
        const auto &path = args->pPaths[index];
        if (!path.MonitorObject) {
          continue;
        }

        if ((static_cast<UINT>(path.Flags) & static_cast<UINT>(IDDCX_PATH_FLAGS_ACTIVE)) == 0) {
          stop_hardware_cursor(path.MonitorObject);
        } else {
          (void) setup_hardware_cursor(path.MonitorObject);
        }
      }

      return STATUS_SUCCESS;
    }

    NTSTATUS commit_modes2(const IDARG_IN_COMMITMODES2 *args) {
      if (!args || (args->PathCount > 0 && !args->pPaths)) {
        return STATUS_INVALID_PARAMETER;
      }

      for (UINT index = 0; index < args->PathCount; ++index) {
        const auto &path = args->pPaths[index];
        if (!path.MonitorObject) {
          continue;
        }

        if ((static_cast<UINT>(path.Flags) & static_cast<UINT>(IDDCX_PATH_FLAGS_ACTIVE)) == 0) {
          stop_hardware_cursor(path.MonitorObject);
        } else {
          (void) setup_hardware_cursor(path.MonitorObject);
        }
      }

      return STATUS_SUCCESS;
    }

    bool shutdown() {
      {
        std::unique_lock lock {mutex_};
        shutting_down_ = true;
        if (monitor_arrivals_in_flight_ > 0 ||
            monitor_departures_in_flight_ > 0 ||
            render_adapter_calls_in_flight_ > 0) {
          departure_cv_.wait_for(lock, kMonitorDepartureTimeout, [&]() {
            return monitor_arrivals_in_flight_ == 0 &&
                   monitor_departures_in_flight_ == 0 &&
                   render_adapter_calls_in_flight_ == 0;
          });
        }

        if (monitor_arrivals_in_flight_ > 0 ||
            monitor_departures_in_flight_ > 0 ||
            render_adapter_calls_in_flight_ > 0) {
          TraceLoggingWrite(
            g_trace_provider,
            "BackendShutdownDeferredForInFlightLifecycle",
            TraceLoggingUInt32(monitor_arrivals_in_flight_, "ArrivalsInFlight"),
            TraceLoggingUInt32(monitor_departures_in_flight_, "DeparturesInFlight"),
            TraceLoggingUInt32(render_adapter_calls_in_flight_, "RenderAdapterCallsInFlight")
          );
          TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER, "BackendShutdownDeferredForInFlightLifecycle");
          return false;
        }
      }

      std::vector<std::uint64_t> display_ids;
      {
        std::lock_guard lock {mutex_};
        display_ids.reserve(monitors_.size());
        for (const auto &[display_id, record]: monitors_) {
          if (!record.departing) {
            display_ids.push_back(display_id);
          }
        }
      }

      std::vector<std::uint64_t> failed_departures;
      for (const auto display_id: display_ids) {
        if (depart_display(display_id) != vdd::BackendError::None) {
          failed_departures.push_back(display_id);
        }
      }

      if (!failed_departures.empty()) {
        std::lock_guard lock {mutex_};
        for (const auto display_id: failed_departures) {
          const auto monitor = monitors_.find(display_id);
          if (monitor == monitors_.end()) {
            continue;
          }

          if (auto *context = GetMonitorContext(monitor->second.monitor); context && context->backend == this) {
            context->backend = nullptr;
          }
          TraceLoggingWrite(
            g_trace_provider,
            "MonitorShutdownDeferredAfterFailedDeparture",
            TraceLoggingUInt64(display_id, "DisplayId")
          );
          TraceEvents(TRACE_LEVEL_WARNING, TRACE_SWAPCHAIN, "MonitorShutdownDeferredAfterFailedDeparture");
        }
      }

      std::unique_lock lock {mutex_};
      if (monitor_departures_in_flight_ > 0) {
        departure_cv_.wait_for(lock, kMonitorDepartureTimeout, [&]() {
          return monitor_departures_in_flight_ == 0;
        });
      }

      if (monitor_departures_in_flight_ > 0 || !monitors_.empty()) {
        TraceLoggingWrite(
          g_trace_provider,
          "BackendShutdownDeferredForMonitors",
          TraceLoggingUInt32(monitor_departures_in_flight_, "DeparturesInFlight"),
          TraceLoggingUInt32(static_cast<std::uint32_t>(monitors_.size()), "RemainingMonitors")
        );
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER, "BackendShutdownDeferredForMonitors");
        return false;
      }

      return true;
    }

    void cleanup_finished_retired_swapchains() {
      std::vector<std::unique_ptr<SwapChainProcessor>> finished_processors;
      std::vector<std::uint64_t> orphaned_late_arrivals;
      {
        std::lock_guard lock {mutex_};
        for (auto &[display_id, record]: monitors_) {
          if (record.orphaned_late_arrival && !record.departing) {
            orphaned_late_arrivals.push_back(display_id);
          }
          const auto before = record.retired_swapchain_processors.size();
          const auto cleaned = collect_finished_retired_swapchain_processors(
            record.retired_swapchain_processors,
            finished_processors
          );
          const auto after = record.retired_swapchain_processors.size();
          if (cleaned == 0) {
            continue;
          }

          TraceLoggingWrite(
            g_trace_provider,
            "RetiredSwapChainProcessorsCleaned",
            TraceLoggingUInt64(display_id, "DisplayId"),
            TraceLoggingUInt32(cleaned, "Cleaned"),
            TraceLoggingUInt32(static_cast<std::uint32_t>(after), "Remaining")
          );
          TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_SWAPCHAIN, "RetiredSwapChainProcessorsCleaned");
        }
      }
      finished_processors.clear();
      for (const auto display_id: orphaned_late_arrivals) {
        if (depart_display(display_id) != vdd::BackendError::None) {
          TraceLoggingWrite(
            g_trace_provider,
            "MonitorArrivalLateCleanupRetryDeferred",
            TraceLoggingUInt64(display_id, "DisplayId")
          );
          TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "MonitorArrivalLateCleanupRetryDeferred");
        }
      }
    }

  private:
    NTSTATUS setup_hardware_cursor(IDDCX_MONITOR monitor) {
      if (!monitor) {
        return STATUS_INVALID_PARAMETER;
      }

      auto *context = GetMonitorContext(monitor);
      if (!context || !context->backend) {
        return STATUS_DEVICE_NOT_READY;
      }

      {
        std::lock_guard lock {mutex_};
        const auto record = find_current_monitor_locked(context->display_id, monitor);
        if (record == monitors_.end() || record->second.departing || shutting_down_) {
          return STATUS_GRAPHICS_PATH_NOT_IN_TOPOLOGY;
        }
      }

      // Hardware cursor polling uses IddCxMonitorQueryHardwareCursor* on a worker tied to the
      // raw IDDCX_MONITOR. Those calls cannot be cancelled safely during mode/topology churn, and
      // a worker that outlives IddCxMonitorDeparture can use the departed monitor. Do not arm this
      // optional path; Sunshine renders/captures the cursor independently.
      TraceLoggingWrite(g_trace_provider, "HardwareCursorDisabledForTopologySafety");
      TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_CURSOR, "HardwareCursorDisabledForTopologySafety");
      return STATUS_SUCCESS;
    }

    void stop_hardware_cursor(IDDCX_MONITOR monitor) {
      if (!monitor) {
        return;
      }

      auto *context = GetMonitorContext(monitor);
      if (!context || !context->backend) {
        return;
      }

      std::unique_ptr<CursorProcessor> processor;
      {
        std::lock_guard lock {mutex_};
        const auto record = find_current_monitor_locked(context->display_id, monitor);
        if (record == monitors_.end()) {
          return;
        }

        processor = std::move(record->second.cursor_processor);
      }

      if (processor) {
        stop_cursor_processor(processor);
      }
    }

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

    void finish_unassign_callback(const std::uint64_t display_id, const IDDCX_MONITOR monitor) {
      {
        std::lock_guard lock {mutex_};
        const auto record = find_current_monitor_locked(display_id, monitor);
        if (record != monitors_.end() && record->second.unassign_callbacks_in_flight > 0) {
          --record->second.unassign_callbacks_in_flight;
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

    class UnassignCallbackScope {
    public:
      UnassignCallbackScope(IddCxBackend &backend, const std::uint64_t display_id, const IDDCX_MONITOR monitor):
          backend_ {backend},
          display_id_ {display_id},
          monitor_ {monitor} {
      }

      ~UnassignCallbackScope() {
        backend_.finish_unassign_callback(display_id_, monitor_);
      }

      UnassignCallbackScope(const UnassignCallbackScope &) = delete;
      UnassignCallbackScope &operator=(const UnassignCallbackScope &) = delete;

    private:
      IddCxBackend &backend_;
      std::uint64_t display_id_ {};
      IDDCX_MONITOR monitor_ {};
    };

    void clear_lifecycle_unhealthy_if_settled_locked() {
      if (monitor_arrivals_in_flight_ == 0 &&
          monitor_departures_in_flight_ == 0 &&
          render_adapter_calls_in_flight_ == 0) {
        lifecycle_unhealthy_ = false;
      }
    }

    void finish_monitor_arrival() {
      {
        std::lock_guard lock {mutex_};
        if (monitor_arrivals_in_flight_ > 0) {
          --monitor_arrivals_in_flight_;
        }
        clear_lifecycle_unhealthy_if_settled_locked();
      }
      departure_cv_.notify_all();
    }

    void finish_render_adapter_call() {
      {
        std::lock_guard lock {mutex_};
        if (render_adapter_calls_in_flight_ > 0) {
          --render_adapter_calls_in_flight_;
        }
        clear_lifecycle_unhealthy_if_settled_locked();
      }
      departure_cv_.notify_all();
    }

    vdd::BackendDisplayResult arrive_display(const vdd::DisplayDescriptor &requested_descriptor, const bool permanent) {
      struct ArrivalState {
        // All fields guarded by IddCxBackend::mutex_. The worker-completes vs
        // caller-times-out decision is made under that lock so there is exactly one
        // winner: if the caller wins (caller_timed_out), the worker departs a
        // late-but-successful arrival (no orphan); if the worker wins (worker_done),
        // the caller consumes result. This also avoids the lifecycle_unhealthy_
        // poison: the flag is only set while the worker is genuinely outstanding.
        vdd::BackendDisplayResult result {vdd::BackendError::Failed, {}, 0};
        bool worker_done {false};
        bool caller_timed_out {false};
      };

      auto state = std::make_shared<ArrivalState>();
      {
        std::lock_guard lock {mutex_};
        ++monitor_arrivals_in_flight_;
      }

      std::thread arrival_thread;
      try {
        arrival_thread = std::thread([this, requested_descriptor, permanent, state]() {
          auto result = arrive_display_synchronously(requested_descriptor, permanent);
          bool caller_abandoned = false;
          {
            std::lock_guard lock {mutex_};
            if (state->caller_timed_out) {
              caller_abandoned = true;
            } else {
              state->result = result;
              state->worker_done = true;
            }
          }
          if (caller_abandoned && result.error == vdd::BackendError::None) {
            // The caller already timed out and reported failure (the controller
            // rolled back its store record). A successful late arrival would strand
            // an orphan backend monitor with no control-plane owner, so depart it.
            TraceLoggingWrite(
              g_trace_provider,
              "MonitorArrivalCompletedAfterTimeout",
              TraceLoggingUInt64(requested_descriptor.display_id, "DisplayId")
            );
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "MonitorArrivalCompletedAfterTimeout");
            {
              std::lock_guard lock {mutex_};
              if (auto monitor = monitors_.find(requested_descriptor.display_id);
                  monitor != monitors_.end()) {
                monitor->second.orphaned_late_arrival = true;
              }
            }
            if (depart_display(requested_descriptor.display_id) != vdd::BackendError::None) {
              TraceLoggingWrite(
                g_trace_provider,
                "MonitorArrivalLateCleanupFailed",
                TraceLoggingUInt64(requested_descriptor.display_id, "DisplayId")
              );
              TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "MonitorArrivalLateCleanupFailed");
            }
          }
          finish_monitor_arrival();
        });
      } catch (...) {
        finish_monitor_arrival();
        return {vdd::BackendError::Failed, {}, 0};
      }

      const DWORD wait_result = WaitForSingleObject(
        arrival_thread.native_handle(),
        static_cast<DWORD>(kMonitorArrivalTimeout.count())
      );

      bool worker_finished = false;
      {
        std::lock_guard lock {mutex_};
        if (state->worker_done) {
          worker_finished = true;
        } else {
          // Give up on this arrival. The worker (still outstanding) will observe
          // caller_timed_out and depart a late success itself, and will clear the
          // unhealthy flag via finish_monitor_arrival once it settles.
          state->caller_timed_out = true;
          lifecycle_unhealthy_ = true;
        }
      }

      if (worker_finished) {
        arrival_thread.join();
        return state->result;
      }

      TraceLoggingWrite(
        g_trace_provider,
        "MonitorArrivalTimedOut",
        TraceLoggingUInt64(requested_descriptor.display_id, "DisplayId"),
        TraceLoggingUInt32(wait_result, "WaitResult"),
        TraceLoggingUInt32(GetLastError(), "LastError")
      );
      TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "MonitorArrivalTimedOut");
      arrival_thread.detach();
      return {vdd::BackendError::Failed, {}, 0};
    }

    vdd::BackendDisplayResult arrive_display_synchronously(
      const vdd::DisplayDescriptor &requested_descriptor,
      const bool permanent
    ) {
      const auto descriptor = descriptor_with_runtime_hdr_policy(requested_descriptor);
      IDDCX_ADAPTER adapter {};
      WDFOBJECT referenced_adapter {};
      {
        std::lock_guard lock {mutex_};
        if (shutting_down_ || !adapter_ready_) {
          TraceLoggingWrite(
            g_trace_provider,
            "MonitorArrivalBlockedAdapterNotReady",
            TraceLoggingBool(shutting_down_, "ShuttingDown"),
            TraceLoggingBool(adapter_ready_, "AdapterReady"),
            TraceLoggingInt32(adapter_init_status_, "AdapterInitStatus")
          );
          return {vdd::BackendError::Failed, {}, 0};
        }
        if (!adapter_ || monitors_.contains(descriptor.display_id)) {
          TraceLoggingWrite(
            g_trace_provider,
            "MonitorArrivalBlockedInvalidState",
            TraceLoggingBool(adapter_ != nullptr, "HasAdapter"),
            TraceLoggingBool(monitors_.contains(descriptor.display_id), "DuplicateDisplayId"),
            TraceLoggingUInt64(descriptor.display_id, "DisplayId")
          );
          return {vdd::BackendError::Failed, {}, 0};
        }
        adapter = adapter_;
        referenced_adapter = reinterpret_cast<WDFOBJECT>(adapter);
        WdfObjectReference(referenced_adapter);
      }
      const auto adapter_reference = WdfObjectReferenceGuard::adopt(referenced_adapter);

      MonitorRecord record {};
      record.descriptor = descriptor;
      record.permanent = permanent;
      record.arriving = true;

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

      try {
        register_monitor_description_mode(descriptor);
      } catch (...) {
        TraceLoggingWrite(
          g_trace_provider,
          "MonitorArrivalRegisterModeFailed",
          TraceLoggingUInt64(descriptor.display_id, "DisplayId")
        );
        return {vdd::BackendError::Failed, {}, 0};
      }

      IDARG_OUT_MONITORCREATE create_out {};
      auto status = IddCxMonitorCreate(adapter, &create_args, &create_out);
      if (!NT_SUCCESS(status)) {
        TraceLoggingWrite(
          g_trace_provider,
          "MonitorCreateFailed",
          TraceLoggingUInt64(descriptor.display_id, "DisplayId"),
          TraceLoggingInt32(status, "Status")
        );
        unregister_monitor_description_mode(descriptor);
        return {vdd::BackendError::Failed, {}, 0};
      }
      WdfObjectReference(reinterpret_cast<WDFOBJECT>(create_out.MonitorObject));
      auto monitor_reference = WdfObjectReferenceGuard::adopt(reinterpret_cast<WDFOBJECT>(create_out.MonitorObject));

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
          unregister_monitor_description_mode(descriptor);
          TraceLoggingWrite(
            g_trace_provider,
            "MonitorArrivalBookkeepingException",
            TraceLoggingUInt64(descriptor.display_id, "DisplayId")
          );
          return {vdd::BackendError::Failed, {}, 0};
        }
      }
      if (!NT_SUCCESS(status)) {
        TraceLoggingWrite(
          g_trace_provider,
          "MonitorArrivalBookkeepingFailed",
          TraceLoggingUInt64(descriptor.display_id, "DisplayId"),
          TraceLoggingInt32(status, "Status")
        );
        WdfObjectDelete(create_out.MonitorObject);
        unregister_monitor_description_mode(descriptor);
        return {vdd::BackendError::Failed, {}, 0};
      }

      IDARG_OUT_MONITORARRIVAL arrival_out {};
      status = IddCxMonitorArrival(create_out.MonitorObject, &arrival_out);
      if (!NT_SUCCESS(status)) {
        TraceLoggingWrite(
          g_trace_provider,
          "MonitorArrivalFailed",
          TraceLoggingUInt64(descriptor.display_id, "DisplayId"),
          TraceLoggingInt32(status, "Status")
        );
        {
          std::lock_guard lock {mutex_};
          monitors_.erase(descriptor.display_id);
        }
        WdfObjectDelete(create_out.MonitorObject);
        unregister_monitor_description_mode(descriptor);
        return {vdd::BackendError::Failed, {}, 0};
      }

      {
        std::lock_guard lock {mutex_};
        const auto monitor = monitors_.find(descriptor.display_id);
        if (monitor == monitors_.end() || monitor->second.monitor != create_out.MonitorObject) {
          TraceLoggingWrite(
            g_trace_provider,
            "MonitorArrivalRecordMissingAfterArrival",
            TraceLoggingUInt64(descriptor.display_id, "DisplayId")
          );
          TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "MonitorArrivalRecordMissingAfterArrival");
          WdfObjectDelete(create_out.MonitorObject);
          unregister_monitor_description_mode(descriptor);
          return {vdd::BackendError::Failed, {}, 0};
        }
        monitor->second.arriving = false;
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
      if (!permanent) {
        std::optional<DWORD> retained_dpi_value;
        {
          std::lock_guard lock {mutex_};
          if (const auto dpi = retained_dpi_values_by_display_id_.find(descriptor.display_id);
              dpi != retained_dpi_values_by_display_id_.end()) {
            retained_dpi_value = dpi->second;
          }
        }
        schedule_hdr_profile_association_retention(device_, descriptor, arrival_out.OsTargetId, retained_dpi_value);
      }
      return {
        vdd::BackendError::None,
        vdd::from_windows_luid(arrival_out.OsAdapterLuid),
        arrival_out.OsTargetId
      };
    }

    void finish_monitor_departure(
      const std::uint64_t display_id,
      const IDDCX_MONITOR monitor_handle,
      const NTSTATUS status
    ) {
      std::optional<vdd::DisplayDescriptor> descriptor_to_unregister;
      {
        std::lock_guard lock {mutex_};
        if (monitor_departures_in_flight_ > 0) {
          --monitor_departures_in_flight_;
        }

        const auto monitor = monitors_.find(display_id);
        if (monitor != monitors_.end() && monitor->second.monitor == monitor_handle) {
          if (NT_SUCCESS(status)) {
            descriptor_to_unregister = monitor->second.descriptor;
            monitors_.erase(monitor);
          } else {
            monitor->second.departing = false;
          }
        }
        clear_lifecycle_unhealthy_if_settled_locked();
      }

      if (descriptor_to_unregister) {
        unregister_monitor_description_mode(*descriptor_to_unregister);
      }

      if (NT_SUCCESS(status)) {
        TraceLoggingWrite(g_trace_provider, "MonitorDeparted", TraceLoggingUInt64(display_id, "DisplayId"));
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "MonitorDeparted");
      } else {
        TraceLoggingWrite(
          g_trace_provider,
          "MonitorDepartureFailed",
          TraceLoggingUInt64(display_id, "DisplayId"),
          TraceLoggingInt32(status, "Status")
        );
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "MonitorDepartureFailed");
      }
      departure_cv_.notify_all();
    }

    NTSTATUS depart_monitor_with_timeout(
      const std::uint64_t display_id,
      const IDDCX_MONITOR monitor_handle
    ) {
      struct DepartureState {
        std::atomic<NTSTATUS> status {STATUS_PENDING};
      };

      auto state = std::make_shared<DepartureState>();
      WdfObjectReference(reinterpret_cast<WDFOBJECT>(monitor_handle));
      auto monitor_reference = std::make_shared<WdfObjectReferenceGuard>(
        WdfObjectReferenceGuard::adopt(reinterpret_cast<WDFOBJECT>(monitor_handle))
      );
      {
        std::lock_guard lock {mutex_};
        ++monitor_departures_in_flight_;
      }

      std::thread departure_thread;
      try {
        departure_thread = std::thread([this, display_id, monitor_handle, state, monitor_reference]() {
          const auto status = IddCxMonitorDeparture(monitor_handle);
          state->status.store(status, std::memory_order_release);
          finish_monitor_departure(display_id, monitor_handle, status);
        });
      } catch (...) {
        monitor_reference.reset();
        finish_monitor_departure(display_id, monitor_handle, STATUS_INSUFFICIENT_RESOURCES);
        return STATUS_INSUFFICIENT_RESOURCES;
      }

      const auto wait_ms = static_cast<DWORD>(kMonitorDepartureTimeout.count());
      const DWORD wait_result = WaitForSingleObject(departure_thread.native_handle(), wait_ms);
      if (wait_result == WAIT_OBJECT_0) {
        departure_thread.join();
        return state->status.load(std::memory_order_acquire);
      }

      TraceLoggingWrite(
        g_trace_provider,
        "MonitorDepartureTimedOut",
        TraceLoggingUInt64(display_id, "DisplayId"),
        TraceLoggingUInt32(wait_result, "WaitResult"),
        TraceLoggingUInt32(GetLastError(), "LastError")
      );
      TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "MonitorDepartureTimedOut");
      {
        std::lock_guard lock {mutex_};
        // Only mark unhealthy while the detached departure worker is genuinely
        // still outstanding. If it already finished (and cleared the flag) under
        // this same lock, do not re-poison a flag nobody will clear.
        if (monitor_departures_in_flight_ > 0) {
          lifecycle_unhealthy_ = true;
        }
      }
      departure_thread.detach();
      return STATUS_TIMEOUT;
    }

    vdd::BackendError depart_display(const std::uint64_t display_id) {
      IDDCX_MONITOR monitor_handle {};
      std::unique_ptr<CursorProcessor> cursor_processor_to_stop;
      std::unique_ptr<SwapChainProcessor> processor_to_stop;
      std::vector<std::unique_ptr<SwapChainProcessor>> retired_processors_to_stop;
      {
        std::unique_lock lock {mutex_};
        auto monitor = monitors_.find(display_id);
        if (monitor == monitors_.end()) {
          return vdd::BackendError::None;
        }
        if (monitor->second.arriving) {
          TraceLoggingWrite(
            g_trace_provider,
            "MonitorDepartureDeferredForArrival",
            TraceLoggingUInt64(display_id, "DisplayId")
          );
          TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "MonitorDepartureDeferredForArrival");
          return vdd::BackendError::Failed;
        }
        if (monitor->second.departing) {
          return vdd::BackendError::Failed;
        }

        monitor->second.departing = true;
        monitor_handle = monitor->second.monitor;
        if (monitor->second.assign_callbacks_in_flight > 0 ||
            monitor->second.unassign_callbacks_in_flight > 0) {
          // DisplayConfig can remove a just-activated path while IddCx is still
          // unwinding swapchain callbacks. Mark departure first, then wait only
          // for known in-flight callbacks to restore or settle processor
          // bookkeeping before crossing the monitor lifetime boundary.
          departure_cv_.wait_for(lock, std::chrono::milliseconds(250), [&]() {
            const auto current = monitors_.find(display_id);
            return current == monitors_.end() ||
                   current->second.monitor != monitor_handle ||
                   (current->second.assign_callbacks_in_flight == 0 &&
                    current->second.unassign_callbacks_in_flight == 0);
          });
        }

        monitor = monitors_.find(display_id);
        if (monitor == monitors_.end() || monitor->second.monitor != monitor_handle) {
          return vdd::BackendError::None;
        }
        if (monitor->second.assign_callbacks_in_flight > 0 ||
            monitor->second.unassign_callbacks_in_flight > 0) {
          monitor->second.departing = false;
          TraceLoggingWrite(
            g_trace_provider,
            "MonitorDepartureDeferredForSwapChainCallback",
            TraceLoggingUInt64(display_id, "DisplayId"),
            TraceLoggingUInt32(monitor->second.assign_callbacks_in_flight, "AssignCallbacksInFlight"),
            TraceLoggingUInt32(monitor->second.unassign_callbacks_in_flight, "UnassignCallbacksInFlight")
          );
          TraceEvents(TRACE_LEVEL_WARNING, TRACE_SWAPCHAIN, "MonitorDepartureDeferredForSwapChainCallback");
          return vdd::BackendError::Failed;
        }
        cursor_processor_to_stop = std::move(monitor->second.cursor_processor);
        processor_to_stop = std::move(monitor->second.swapchain_processor);
        retired_processors_to_stop = std::move(monitor->second.retired_swapchain_processors);
      }

      const bool cursor_processor_stopped = try_stop_cursor_processor_for_monitor_departure(cursor_processor_to_stop);
      if (!cursor_processor_stopped) {
        std::lock_guard lock {mutex_};
        if (auto monitor = monitors_.find(display_id);
            monitor != monitors_.end() &&
            monitor->second.monitor == monitor_handle) {
          monitor->second.departing = false;
          monitor->second.cursor_processor = std::move(cursor_processor_to_stop);
          if (processor_to_stop) {
            monitor->second.swapchain_processor = std::move(processor_to_stop);
          }
          for (auto &processor: retired_processors_to_stop) {
            if (processor) {
              monitor->second.retired_swapchain_processors.push_back(std::move(processor));
            }
          }
        } else {
          stop_cursor_processor(cursor_processor_to_stop);
          stop_swapchain_processor(processor_to_stop, DeferredSwapChainCleanup::AbandonOwned);
          stop_swapchain_processors(retired_processors_to_stop, DeferredSwapChainCleanup::AbandonOwned);
        }
        TraceLoggingWrite(
          g_trace_provider,
          "MonitorDepartureDeferredForHardwareCursorWorker",
          TraceLoggingUInt64(display_id, "DisplayId")
        );
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_CURSOR, "MonitorDepartureDeferredForHardwareCursorWorker");
        return vdd::BackendError::Failed;
      }

      // Wake all frame workers first so processors that stop within the
      // teardown window can close their owned swapchains before monitor
      // departure. If any worker is still in an IddCx/D3D call after the
      // bounded wait, do NOT cross the monitor-departure lifetime boundary;
      // keep the monitor alive and let the control plane retry the departure.
      request_stop_swapchain_processor(processor_to_stop);
      request_stop_swapchain_processors(retired_processors_to_stop);
      const auto swapchain_teardown_deadline =
        std::chrono::steady_clock::now() + kSwapchainProcessorTeardownTimeout;
      const bool active_processor_stopped = try_stop_swapchain_processor_for_monitor_departure(
        processor_to_stop,
        swapchain_teardown_deadline
      );
      const bool retired_processors_stopped = try_stop_swapchain_processors_for_monitor_departure(
        retired_processors_to_stop,
        swapchain_teardown_deadline
      );
      if (!active_processor_stopped || !retired_processors_stopped) {
        std::lock_guard lock {mutex_};
        if (auto monitor = monitors_.find(display_id);
            monitor != monitors_.end() &&
            monitor->second.monitor == monitor_handle) {
          monitor->second.departing = false;
          if (processor_to_stop) {
            monitor->second.swapchain_processor = std::move(processor_to_stop);
          }
          for (auto &processor: retired_processors_to_stop) {
            if (processor) {
              monitor->second.retired_swapchain_processors.push_back(std::move(processor));
            }
          }
        } else {
          stop_swapchain_processor(processor_to_stop, DeferredSwapChainCleanup::AbandonOwned);
          stop_swapchain_processors(retired_processors_to_stop, DeferredSwapChainCleanup::AbandonOwned);
        }
        TraceLoggingWrite(
          g_trace_provider,
          "MonitorDepartureDeferredForSwapChainWorker",
          TraceLoggingUInt64(display_id, "DisplayId")
        );
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_SWAPCHAIN, "MonitorDepartureDeferredForSwapChainWorker");
        return vdd::BackendError::Failed;
      }

      // IddCx can synchronously or asynchronously issue swapchain callbacks
      // during departure. Run the call behind a bounded worker so one wedged OS
      // display-stack call cannot pin the control path forever.
      const auto status = depart_monitor_with_timeout(display_id, monitor_handle);
      if (!NT_SUCCESS(status)) {
        return vdd::BackendError::Failed;
      }

      return vdd::BackendError::None;
    }

  public:
    NTSTATUS assign_swapchain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN *args) {
      // IddCx bugchecks the driver if EvtIddCxMonitorAssignSwapChain returns any
      // status other than STATUS_SUCCESS or STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN.
      // Every failure exit on this path must therefore decline via ABANDON, never a
      // generic NTSTATUS.
      if (!monitor || !args || !args->hSwapChain || !args->hNextSurfaceAvailable) {
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
      }

      auto *context = GetMonitorContext(monitor);
      if (!context || !context->backend) {
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
      }

      {
        std::lock_guard lock {mutex_};
        const auto record = find_current_monitor_locked(context->display_id, monitor);
        if (record == monitors_.end() || record->second.departing || shutting_down_) {
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
      // NOTE: do NOT publish assignment success yet. The worker is already running
      // and is allowed to WdfObjectDelete the swapchain once ownership is committed.
      // Publishing success before the departing recheck below let a fast worker
      // failure delete a swapchain that this callback might then hand back to IddCx
      // as ABANDON_SWAPCHAIN (which keeps OS ownership) -> double free / verifier
      // bugcheck. Commit ownership exactly once, on the success path, under mutex_.
      std::unique_ptr<SwapChainProcessor> previous_processor;
      std::vector<std::unique_ptr<SwapChainProcessor>> finished_retired_processors;
      bool abandon_new_processor = false;
      {
        std::lock_guard lock {mutex_};
        const auto record = find_current_monitor_locked(context->display_id, monitor);
        if (record == monitors_.end() || record->second.departing) {
          abandon_new_processor = true;
        } else {
          collect_finished_retired_swapchain_processors(
            record->second.retired_swapchain_processors,
            finished_retired_processors
          );
          previous_processor = std::move(record->second.swapchain_processor);
          // Irrevocable success: from here the callback returns STATUS_SUCCESS and
          // the decision never transitions back to abandoned.
          processor->mark_assign_succeeded();
          record->second.swapchain_processor = std::move(processor);
        }
      }
      finished_retired_processors.clear();

      if (abandon_new_processor) {
        processor->mark_assign_abandoned();
        stop_swapchain_processor(processor);
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
      }

      if (previous_processor) {
        // IddCx rotates swapchains inside HandleNewSwapChain. Request the old
        // worker to stop, but keep the old WDF object alive until monitor
        // teardown so this callback never waits on a worker blocked in IddCx.
        previous_processor->request_stop();
        bool retired = false;
        auto deferred_cleanup = DeferredSwapChainCleanup::CloseOwned;
        try {
          std::lock_guard lock {mutex_};
          const auto record = find_current_monitor_locked(context->display_id, monitor);
          if (record != monitors_.end() && !record->second.departing) {
            record->second.retired_swapchain_processors.push_back(std::move(previous_processor));
            retired = true;
          } else {
            deferred_cleanup = DeferredSwapChainCleanup::AbandonOwned;
          }
        } catch (...) {
        }
        if (!retired && previous_processor) {
          defer_stop_swapchain_processor(std::move(previous_processor), deferred_cleanup);
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
      std::vector<std::unique_ptr<SwapChainProcessor>> finished_retired_processors;
      std::optional<UnassignCallbackScope> unassign_scope;
      {
        std::lock_guard lock {mutex_};
        const auto record = find_current_monitor_locked(context->display_id, monitor);
        if (record == monitors_.end()) {
          // Monitor departure may remove our bookkeeping before IddCx delivers a
          // final unassign callback. The swapchain is already gone in that case.
          return STATUS_SUCCESS;
        }
        if (record->second.departing || shutting_down_) {
          // Departure/shutdown owns the monitor lifetime boundary. Leave the
          // processors attached to bookkeeping so that path can defer removal
          // if any worker is still live.
          TraceLoggingWrite(
            g_trace_provider,
            "SwapChainUnassignIgnoredDuringDeparture",
            TraceLoggingUInt64(context->display_id, "DisplayId"),
            TraceLoggingBool(record->second.departing, "Departing"),
            TraceLoggingBool(shutting_down_, "ShuttingDown")
          );
          TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_SWAPCHAIN, "SwapChainUnassignIgnoredDuringDeparture");
          return STATUS_SUCCESS;
        }

        ++record->second.unassign_callbacks_in_flight;
        unassign_scope.emplace(*this, context->display_id, monitor);
        collect_finished_retired_swapchain_processors(
          record->second.retired_swapchain_processors,
          finished_retired_processors
        );
        processor_to_stop = std::move(record->second.swapchain_processor);
        retired_processors_to_stop = std::move(record->second.retired_swapchain_processors);
      }
      finished_retired_processors.clear();

      // IddCx unassign asks the driver to stop processing and close its owned
      // swapchain object. If a worker is stuck in IddCx/D3D, keep the object in
      // monitor bookkeeping as retired work so later monitor departure still
      // sees the live processor and can defer the lifetime boundary.
      request_stop_swapchain_processor(processor_to_stop);
      request_stop_swapchain_processors(retired_processors_to_stop);
      const auto swapchain_teardown_deadline =
        std::chrono::steady_clock::now() + kSwapchainProcessorTeardownTimeout;
      const bool active_processor_stopped = try_stop_swapchain_processor_for_unassign(
        processor_to_stop,
        swapchain_teardown_deadline
      );
      const bool retired_processors_stopped = try_stop_swapchain_processors_for_unassign(
        retired_processors_to_stop,
        swapchain_teardown_deadline
      );
      if (!active_processor_stopped || !retired_processors_stopped) {
        std::lock_guard lock {mutex_};
        const auto record = find_current_monitor_locked(context->display_id, monitor);
        if (record != monitors_.end()) {
          if (processor_to_stop) {
            record->second.retired_swapchain_processors.push_back(std::move(processor_to_stop));
          }
          for (auto &processor: retired_processors_to_stop) {
            if (processor) {
              record->second.retired_swapchain_processors.push_back(std::move(processor));
            }
          }
        } else {
          stop_swapchain_processor(processor_to_stop, DeferredSwapChainCleanup::AbandonOwned);
          stop_swapchain_processors(retired_processors_to_stop, DeferredSwapChainCleanup::AbandonOwned);
        }
        TraceLoggingWrite(
          g_trace_provider,
          "SwapChainUnassignDeferredForWorker",
          TraceLoggingUInt64(context->display_id, "DisplayId")
        );
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_SWAPCHAIN, "SwapChainUnassignDeferredForWorker");
      }
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

      // FP16-capable IddCx adapters can still receive 3x4 color-space
      // transforms even when endpoint gamma support is not advertised. Record
      // the callback for diagnostics; the current virtual-display sink has no
      // endpoint pixel-processing stage where this transform can be applied.
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

    bool lifecycle_unhealthy() {
      std::lock_guard lock {mutex_};
      return lifecycle_unhealthy_;
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
    bool shutting_down_ {};
    std::uint32_t monitor_arrivals_in_flight_ {};
    std::uint32_t monitor_departures_in_flight_ {};
    std::uint32_t render_adapter_calls_in_flight_ {};
    bool lifecycle_unhealthy_ {};
    NTSTATUS adapter_init_status_ {STATUS_DEVICE_NOT_READY};
    std::map<std::uint64_t, MonitorRecord> monitors_ {};
    std::map<std::uint64_t, DWORD> retained_dpi_values_by_display_id_ {};
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
      (void) shutdown_for_cleanup();
    }

    bool shutdown_for_cleanup() {
      if (shutdown_completed_) {
        return true;
      }

      if (!stop_reaper()) {
        return false;
      }

      if (!backend.shutdown()) {
        return false;
      }
      shutdown_completed_ = true;
      return true;
    }

    vdd::IoctlDispatchResult dispatch(
      const ULONG io_control_code,
      void *input,
      const std::size_t input_buffer_length,
      void *output,
      const std::size_t output_buffer_length,
      const std::chrono::steady_clock::time_point now
    ) {
      if (backend.lifecycle_unhealthy() && !can_dispatch_while_backend_unhealthy(io_control_code)) {
        TraceLoggingWrite(
          g_trace_provider,
          "DeviceIoControlRejectedForUnhealthyBackend",
          TraceLoggingUInt32(io_control_code, "IoControlCode")
        );
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER, "DeviceIoControlRejectedForUnhealthyBackend");
        return {vdd::IoctlStatus::BackendFailed, 0, {}};
      }

      std::unique_lock lock {controller_mutex, std::defer_lock};
      if (!lock.try_lock_for(kControllerLockTimeout)) {
        TraceLoggingWrite(
          g_trace_provider,
          "DeviceIoControlControllerBusy",
          TraceLoggingUInt32(io_control_code, "IoControlCode")
        );
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER, "DeviceIoControlControllerBusy");
        return {vdd::IoctlStatus::BackendFailed, 0, {}};
      }
      return dispatcher.dispatch(
        io_control_code,
        input,
        input_buffer_length,
        output,
        output_buffer_length,
        now,
        &lock
      );
    }

    IddCxBackend backend;
    vdd::DriverController controller;
    vdd::IoctlDispatcher dispatcher;

  private:
    static constexpr auto kReaperInterval = std::chrono::seconds(1);
    static constexpr auto kReaperTeardownTimeout = std::chrono::milliseconds(500);
    static constexpr auto kControllerLockTimeout = std::chrono::seconds(30);

    static bool can_dispatch_while_backend_unhealthy(const ULONG io_control_code) {
      switch (io_control_code) {
        case vdd::kIoctlGetProtocolVersion:
        case vdd::kIoctlRemoveTemporaryDisplay:
        case vdd::kIoctlFeedLease:
        case vdd::kIoctlReleaseLease:
        case vdd::kIoctlQueryLease:
        case vdd::kIoctlQueryPermanentDisplayCount:
        case vdd::kIoctlQueryDisplayState:
        case vdd::kIoctlQueryDisplayManifest:
          return true;
        default:
          return false;
      }
    }

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

    bool stop_reaper() {
      reaper_stop_requested.store(true, std::memory_order_release);
      reaper_cv.notify_all();
      if (!reaper_thread.joinable()) {
        return true;
      }

      const DWORD wait_result = WaitForSingleObject(
        reaper_thread.native_handle(),
        static_cast<DWORD>(kReaperTeardownTimeout.count())
      );
      if (wait_result == WAIT_OBJECT_0) {
        reaper_thread.join();
        return true;
      }

      TraceLoggingWrite(
        g_trace_provider,
        "LeaseReaperShutdownDeferred",
        TraceLoggingUInt32(wait_result, "WaitResult"),
        TraceLoggingUInt32(GetLastError(), "LastError")
      );
      TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER, "LeaseReaperShutdownDeferred");
      reaper_thread.detach();
      return false;
    }

    void reaper_loop() {
      std::unique_lock wait_lock {reaper_wait_mutex};
      while (!reaper_stop_requested.load(std::memory_order_acquire)) {
        if (reaper_cv.wait_for(wait_lock, kReaperInterval, [this]() {
              return reaper_stop_requested.load(std::memory_order_acquire);
            })) {
          break;
        }

        std::unique_lock controller_lock {controller_mutex, std::defer_lock};
        if (!controller_lock.try_lock_for(std::chrono::milliseconds(10))) {
          TraceLoggingWrite(g_trace_provider, "LeaseReaperSkippedBusyController");
          TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER, "LeaseReaperSkippedBusyController");
          continue;
        }
        (void) controller.reap_expired(std::chrono::steady_clock::now(), &controller_lock);
        controller_lock.unlock();
        backend.cleanup_finished_retired_swapchains();
      }
    }

    std::timed_mutex controller_mutex {};
    std::atomic<bool> reaper_stop_requested {false};
    bool shutdown_completed_ {};
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
    auto *state = std::exchange(context->state, nullptr);
    if (!state) {
      return;
    }

    if (state->shutdown_for_cleanup()) {
      delete state;
      return;
    }

    // The lease reaper or a bounded monitor-departure worker can still be
    // inside backend/IddCx code while WDF is cleaning up the device. Keep
    // DeviceState alive instead of deleting storage those threads may touch.
    TraceLoggingWrite(g_trace_provider, "DeviceStateLeakedForDeferredShutdown");
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER, "DeviceStateLeakedForDeferredShutdown");
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
    return context->backend.load()->query_target_modes(monitor, input, output);
  }

  return fill_target_modes(input, output);
}

NTSTATUS SunshineEvtCommitModes(IDDCX_ADAPTER adapter, const IDARG_IN_COMMITMODES *args) {
  auto *context = GetAdapterContext(adapter);
  if (!context || !context->backend) {
    return STATUS_DEVICE_NOT_READY;
  }

  return context->backend->commit_modes(args);
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

NTSTATUS SunshineEvtCommitModes2(IDDCX_ADAPTER adapter, const IDARG_IN_COMMITMODES2 *args) {
  auto *context = GetAdapterContext(adapter);
  if (!context || !context->backend) {
    return STATUS_DEVICE_NOT_READY;
  }

  return context->backend->commit_modes2(args);
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

  return context->backend.load()->set_default_hdr_metadata(monitor, args);
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

  return context->backend.load()->set_gamma_ramp(monitor, args);
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
    return context->backend.load()->query_target_modes2(monitor, input, output);
  }

  return fill_target_modes2(input, output);
}

NTSTATUS SunshineEvtAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN *args) {
  // IddCx bugchecks on any assign return other than SUCCESS / ABANDON_SWAPCHAIN.
  // The null-backend case is reachable on a late callback after shutdown nulls
  // MonitorContext::backend, so it must decline via ABANDON rather than a generic
  // failure status.
  if (!monitor) {
    return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
  }

  auto *context = GetMonitorContext(monitor);
  if (!context || !context->backend) {
    return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
  }

  return context->backend.load()->assign_swapchain(monitor, args);
}

NTSTATUS SunshineEvtUnassignSwapChain(IDDCX_MONITOR monitor) {
  if (!monitor) {
    return STATUS_INVALID_PARAMETER;
  }

  auto *context = GetMonitorContext(monitor);
  if (!context || !context->backend) {
    return STATUS_DEVICE_NOT_READY;
  }

  return context->backend.load()->unassign_swapchain(monitor);
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
