#include "virtual_display/driver/control_client.h"
#include "virtual_display/driver/probe_commands.h"
#include "virtual_display/driver/device_identity.h"
#include "virtual_display/driver/windows_control_client.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <d3d11.h>
  #include <d3dkmthk.h>
  #include <dxgi1_2.h>
  #include <Icm.h>
  #include <WtsApi32.h>
  #include <TlHelp32.h>
  #include <wrl/client.h>
  #include <windows.h>
  #include <winternl.h>
  #include <Aclapi.h>
  #ifdef _MSC_VER
    #include <Windows.Devices.Display.Core.Interop.h>
    #include <winrt/Windows.Devices.Display.Core.h>
    #include <winrt/Windows.Foundation.Collections.h>
    #include <winrt/Windows.Graphics.DirectX.h>
    #include <winrt/base.h>
  #endif
#endif

namespace vdd = virtual_display::driver;

namespace {
#ifdef _WIN32
  constexpr const wchar_t *kHelperEventSource = vdd::kBrokerServiceNameW.data();
  constexpr DWORD kEventHelperTopologyApplied = 0x3000;
  constexpr DWORD kEventHelperTopologyFailed = 0x3001;
  constexpr DWORD kEventHelperColorQueryCompleted = 0x3100;
  constexpr DWORD kEventHelperColorQueryFailed = 0x3101;
  constexpr DWORD kEventHelperColorAssociationCompleted = 0x3102;
  constexpr DWORD kEventHelperColorAssociationFailed = 0x3103;
  constexpr COLORPROFILESUBTYPE kStandardDisplayColorMode =
  #ifdef CPST_STANDARD_DISPLAY_COLOR_MODE
    CPST_STANDARD_DISPLAY_COLOR_MODE;
  #else
    static_cast<COLORPROFILESUBTYPE>(7);
  #endif
  constexpr COLORPROFILESUBTYPE kExtendedDisplayColorMode =
  #ifdef CPST_EXTENDED_DISPLAY_COLOR_MODE
    CPST_EXTENDED_DISPLAY_COLOR_MODE;
  #else
    static_cast<COLORPROFILESUBTYPE>(8);
  #endif
  #ifndef DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR
  constexpr std::uint32_t DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR = 2;
  #endif

  std::wstring widen_ascii(const std::string_view text) {
    std::wstring wide;
    wide.reserve(text.size());
    for (const unsigned char ch: text) {
      wide.push_back(static_cast<wchar_t>(ch));
    }
    return wide;
  }

  void report_helper_event(const WORD type, const DWORD event_id, const std::wstring_view text) {
    HANDLE source = RegisterEventSourceW(nullptr, kHelperEventSource);
    if (!source) {
      return;
    }

    const std::wstring message {text};
    LPCWSTR strings[] {message.c_str()};
    (void) ReportEventW(source, type, 0, event_id, nullptr, 1, 0, strings, nullptr);
    DeregisterEventSource(source);
  }

  void report_helper_event(const WORD type, const DWORD event_id, const std::string_view text) {
    report_helper_event(type, event_id, widen_ascii(text));
  }
#endif

  void print_usage() {
    std::cout
      << "virtualdisplay_probe commands:\n"
      << "  --diagnose\n"
      << "  --apply-extended-topology\n"
      << "  --apply-extended-topology-current-session\n"
      << "  --probe-idd-hdr-functionalize-current-session [output-path]\n"
      << "  --probe-idd-hdr-functionalize-shared-owner-current-session [output-path]\n"
      << "  --probe-idd-hdr-functionalize-nongdi-source-current-session [output-path]\n"
      << "  --probe-wcg-prime-hdr-inherited-token <token_handle> <session_id> <display_id> <output-root>\n"
      << "  --probe-wcg-prime-hdr-native-user <session_id> <display_id> <output-root>\n"
      << "  --remote-current-session-wcg-to-hdr <display_id> <output-path>\n"
      << "  --query-private-functionalize-current-session [output-path]\n"
      << "  --probe-displaymanager-fp16-functionalize-current-session\n"
      << "  --probe-displaymanager-fp16-enforce-functionalize-current-session\n"
      << "  --probe-displaymanager-desktop-hdr-functionalize-current-session\n"
      << "  --probe-displaymanager-desktop-owner2-hdr-functionalize-current-session [output-path]\n"
      << "  --launch-displaymanager-owner2-probe-in-session <session_id> <output-path>\n"
      << "  --probe-displaymanager-acquire-arriving-target [timeout_ms]\n"
      << "  --probe-displaymanager-acquire-new-temp-target [width height refresh_hz]\n"
      << "  --dump-display-config-current-session\n"
      << "  --query-hdr-target <target_luid high:low> <target_id>\n"
      << "  --query-d3dkmt-current-mode <adapter_luid high:low> <source_id>\n"
      << "  --query-d3dkmt-mode-list <adapter_luid high:low> <source_id>\n"
      << "  --query-vidpn-ownership-current-session [output-path]\n"
      << "  --set-hdr-target <target_luid high:low> <target_id> <0|1>\n"
      << "  --probe-idd-hdr-gate <target_luid high:low> <target_id>\n"
      << "  --set-hdr-current-session <0|1> [output-path]\n"
      << "  --apply-manifest-topology\n"
      << "  --query-color-profiles\n"
      << "  --associate-color-profile <source_luid high:low> <source_id> <profile> [--advanced-color] [--default]\n"
      << "  --check\n"
      << "  --query-permanent\n"
      << "  --set-permanent <count>\n"
      << "  --remote-query-permanent <session_id>\n"
      << "  --remote-query-state <session_id>\n"
      << "  --remote-set-permanent <session_id> <count>\n"
      << "  --remote-set-hdr <session_id> <display_id> <0|1> [sdr_white_level_nits]\n"
      << "  --remote-set-mode <session_id> <display_id> <width> <height> <refresh_millihz>\n"
      << "  --self-test-permanent [count]\n"
      << "  --self-test-temp [width height refresh_hz]\n"
      << "  --self-test-4k240 [timeout_ms]\n"
      << "  --self-test-hdr [width height refresh_hz]\n"
      << "  --self-test-initial-remote-hdr [width height refresh_hz]\n"
      << "  --self-test-lease-expiry [width height refresh_hz timeout_ms]\n"
      << "  --qa-multi-temp-lease [count timeout_ms]\n"
      << "  --qa-temp-identity-retention [width height refresh_hz timeout_ms]\n"
      << "  --qa-temp-lease [width height refresh_hz timeout_ms]\n"
      << "  --stress-capture-remove [iterations width height refresh_hz]\n"
      << "  --debug-temp-config [width height refresh_hz timeout_ms]\n";
  }

  std::uint64_t transient_id(const std::uint64_t salt) {
    static std::random_device random;
    static std::mt19937_64 generator {
      (static_cast<std::uint64_t>(random()) << 32u) ^
      static_cast<std::uint64_t>(random()) ^
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
    };
    std::uint64_t value {};
    do {
      value = 0x6000000000000000ull | ((generator() ^ salt) & 0x0fffffffffffffffull);
    } while (value == 0);
    return value;
  }

#ifdef _WIN32
  bool valid_display_mode_dimensions(const std::uint32_t width, const std::uint32_t height) {
    constexpr auto kMaxLong = static_cast<std::uint32_t>((std::numeric_limits<LONG>::max)());
    constexpr std::uint32_t kMaxReasonableDisplayExtent = 16'384;
    return width != 0 &&
           height != 0 &&
           width <= kMaxLong &&
           height <= kMaxLong &&
           width <= kMaxReasonableDisplayExtent &&
           height <= kMaxReasonableDisplayExtent;
  }

  std::uint32_t rational_to_millihz(const DISPLAYCONFIG_RATIONAL &rate) {
    if (rate.Denominator == 0) {
      return 0;
    }

    return vdd::saturating_u32(
      (static_cast<std::uint64_t>(rate.Numerator) * 1000ull) /
      rate.Denominator
    );
  }
#endif

  vdd::CreateTemporaryDisplayRequest make_temporary_request(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_hz
  ) {
    vdd::CreateTemporaryDisplayRequest request {};
    request.lease_id = transient_id(0x0d15ea5e);
    request.display_id = transient_id(0x51dd15c0);
    request.width = width;
    request.height = height;
    request.refresh_rate_millihz = vdd::refresh_millihz_from_hz(refresh_hz);
    request.requested_timeout_ms = 10'000;
    std::strncpy(request.display_name, "Sunshine Probe", sizeof(request.display_name) - 1);
    return request;
  }

  int fail(const std::string &message, const vdd::ControlOperationResult &result) {
    std::cerr << message << ": " << vdd::to_string(result.status);
    if (result.native_error != 0) {
      std::cerr << " native_error=" << result.native_error;
    }
    std::cerr << '\n';
    return 1;
  }

  template<class T>
  int fail(const std::string &message, const vdd::ControlResult<T> &result) {
    return fail(message, {result.status, result.native_error});
  }

  bool read_u32_arg(
    const int argc,
    char **argv,
    const int index,
    const std::uint32_t fallback,
    const char *label,
    std::uint32_t &value
  ) {
    if (argc <= index) {
      value = fallback;
      return true;
    }

    const auto parsed = vdd::parse_probe_u32_token(argv[index]);
    if (!parsed) {
      std::cerr << "invalid " << label << '\n';
      return false;
    }
    value = *parsed;
    return true;
  }

  bool read_u64_arg(
    const int argc,
    char **argv,
    const int index,
    const std::uint64_t fallback,
    const char *label,
    std::uint64_t &value
  ) {
    if (argc <= index) {
      value = fallback;
      return true;
    }

    const auto parsed = vdd::parse_probe_u64_token(argv[index]);
    if (!parsed) {
      std::cerr << "invalid " << label << '\n';
      return false;
    }
    value = *parsed;
    return true;
  }

  bool require_command_arg_count(const std::string_view command, const int argc) {
    if (!vdd::probe_command_arg_count_valid(command, argc)) {
      print_usage();
      return false;
    }
    return true;
  }

#ifdef _WIN32
  struct AdvancedColorInfo {
    bool v2 = false;
    bool supported = false;
    bool active = false;
    bool limited_by_policy = false;
    bool hdr_supported = false;
    bool hdr_enabled = false;
    DISPLAYCONFIG_COLOR_ENCODING color_encoding = DISPLAYCONFIG_COLOR_ENCODING_RGB;
    std::uint32_t bits_per_color_channel = 0;
    std::uint32_t active_color_mode = 0;
  };

  struct DisplayConfigGetAdvancedColorInfo2 {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header {};
    union {
      struct {
        std::uint32_t advanced_color_supported : 1;
        std::uint32_t advanced_color_active : 1;
        std::uint32_t reserved1 : 1;
        std::uint32_t advanced_color_limited_by_policy : 1;
        std::uint32_t high_dynamic_range_supported : 1;
        std::uint32_t high_dynamic_range_user_enabled : 1;
        std::uint32_t wide_color_supported : 1;
        std::uint32_t wide_color_user_enabled : 1;
        std::uint32_t reserved : 24;
      };
      std::uint32_t value;
    };
    DISPLAYCONFIG_COLOR_ENCODING color_encoding = DISPLAYCONFIG_COLOR_ENCODING_RGB;
    std::uint32_t bits_per_color_channel = 0;
    std::uint32_t active_color_mode = 0;
  };

  struct DisplayConfigSetHdrState {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header {};
    union {
      struct {
        std::uint32_t enable_hdr : 1;
        std::uint32_t reserved : 31;
      };
      std::uint32_t value;
    };
  };

  std::optional<AdvancedColorInfo> query_advanced_color(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    LONG *native_error = nullptr
  ) {
    const auto luid = vdd::to_windows_luid(adapter_luid);
    if (native_error) {
      *native_error = ERROR_SUCCESS;
    }

    DisplayConfigGetAdvancedColorInfo2 info {};
    info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
    info.header.size = sizeof(info);
    info.header.adapterId = luid;
    info.header.id = target_id;
    auto result = DisplayConfigGetDeviceInfo(&info.header);
    if (result == ERROR_SUCCESS) {
      return AdvancedColorInfo {
        true,
        info.advanced_color_supported != 0,
        info.advanced_color_active != 0,
        info.advanced_color_limited_by_policy != 0,
        info.high_dynamic_range_supported != 0,
        info.high_dynamic_range_user_enabled != 0,
        info.color_encoding,
        info.bits_per_color_channel,
        info.active_color_mode
      };
    }

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO fallback {};
    fallback.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    fallback.header.size = sizeof(fallback);
    fallback.header.adapterId = luid;
    fallback.header.id = target_id;
    result = DisplayConfigGetDeviceInfo(&fallback.header);
    if (result != ERROR_SUCCESS) {
      if (native_error) {
        *native_error = result;
      }
      return std::nullopt;
    }

    return AdvancedColorInfo {
      false,
      fallback.advancedColorSupported != 0,
      fallback.advancedColorEnabled != 0,
      fallback.advancedColorForceDisabled != 0,
      false,
      false,
      fallback.colorEncoding,
      fallback.bitsPerColorChannel,
      0
    };
  }

  bool set_hdr_state(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const bool enabled,
    LONG *native_error = nullptr
  ) {
    DisplayConfigSetHdrState state {};
    state.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE;
    state.header.size = sizeof(state);
    state.header.adapterId = vdd::to_windows_luid(adapter_luid);
    state.header.id = target_id;
    state.enable_hdr = enabled ? 1u : 0u;
    const auto result = DisplayConfigSetDeviceInfo(&state.header);
    if (native_error) {
      *native_error = result;
    }
    return result == ERROR_SUCCESS;
  }

  bool set_advanced_color(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const bool enabled,
    LONG *native_error = nullptr
  ) {
    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE state {};
    state.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    state.header.size = sizeof(state);
    state.header.adapterId = vdd::to_windows_luid(adapter_luid);
    state.header.id = target_id;
    state.enableAdvancedColor = enabled ? 1u : 0u;
    const auto result = DisplayConfigSetDeviceInfo(&state.header);
    if (native_error) {
      *native_error = result;
    }
    return result == ERROR_SUCCESS;
  }

  struct DisplayConfigData {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    UINT32 flags = 0;
  };

  struct DisplayConfigQueryResult {
    std::optional<DisplayConfigData> data;
    LONG native_error = ERROR_SUCCESS;
  };

  bool same_luid(const LUID &left, const LUID &right) {
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
  }

  bool enable_process_privilege(const wchar_t *name, DWORD *native_error) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
      if (native_error) {
        *native_error = GetLastError();
      }
      return false;
    }

    LUID luid {};
    if (!LookupPrivilegeValueW(nullptr, name, &luid)) {
      if (native_error) {
        *native_error = GetLastError();
      }
      CloseHandle(token);
      return false;
    }

    TOKEN_PRIVILEGES privileges {};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    const bool adjusted = AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) != FALSE;
    const DWORD error = GetLastError();
    CloseHandle(token);
    if (!adjusted || error == ERROR_NOT_ALL_ASSIGNED) {
      if (native_error) {
        *native_error = adjusted ? error : (error == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : error);
      }
      return false;
    }
    if (native_error) {
      *native_error = ERROR_SUCCESS;
    }
    return true;
  }

  class ScopedConsoleUserImpersonation {
  public:
    ScopedConsoleUserImpersonation() = default;
    ScopedConsoleUserImpersonation(const ScopedConsoleUserImpersonation &) = delete;
    ScopedConsoleUserImpersonation &operator=(const ScopedConsoleUserImpersonation &) = delete;

    ~ScopedConsoleUserImpersonation() {
      if (active_) {
        (void) RevertToSelf();
      }
      if (token_) {
        CloseHandle(token_);
      }
    }

    bool begin(const DWORD target_session_id, DWORD *native_error) {
      DWORD error = ERROR_SUCCESS;
      if (!enable_process_privilege(L"SeTcbPrivilege", &error)) {
        if (native_error) {
          *native_error = error;
        }
        return false;
      }

      source_session_id_ = WTSGetActiveConsoleSessionId();
      target_session_id_ = target_session_id;
      if (source_session_id_ == 0xffffffffu) {
        if (native_error) {
          *native_error = ERROR_NO_SUCH_LOGON_SESSION;
        }
        return false;
      }

      HANDLE source_token = nullptr;
      if (!WTSQueryUserToken(source_session_id_, &source_token)) {
        if (native_error) {
          *native_error = GetLastError();
        }
        return false;
      }

      constexpr DWORD token_access = TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY |
                                     TOKEN_IMPERSONATE | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID;
      const bool duplicated = DuplicateTokenEx(
        source_token,
        token_access,
        nullptr,
        SecurityImpersonation,
        TokenPrimary,
        &token_
      ) != FALSE;
      error = duplicated ? ERROR_SUCCESS : GetLastError();
      CloseHandle(source_token);
      if (!duplicated) {
        if (native_error) {
          *native_error = error;
        }
        return false;
      }

      if (!SetTokenInformation(token_, TokenSessionId, &target_session_id_, sizeof(target_session_id_))) {
        if (native_error) {
          *native_error = GetLastError();
        }
        return false;
      }
      if (!ImpersonateLoggedOnUser(token_)) {
        if (native_error) {
          *native_error = GetLastError();
        }
        return false;
      }

      active_ = true;
      if (native_error) {
        *native_error = ERROR_SUCCESS;
      }
      return true;
    }

    bool revert(DWORD *native_error) {
      if (!active_) {
        if (native_error) {
          *native_error = ERROR_SUCCESS;
        }
        return true;
      }
      if (!RevertToSelf()) {
        if (native_error) {
          *native_error = GetLastError();
        }
        return false;
      }
      active_ = false;
      if (native_error) {
        *native_error = ERROR_SUCCESS;
      }
      return true;
    }

    [[nodiscard]] DWORD source_session_id() const noexcept {
      return source_session_id_;
    }

    [[nodiscard]] DWORD target_session_id() const noexcept {
      return target_session_id_;
    }

  private:
    HANDLE token_ = nullptr;
    DWORD source_session_id_ = 0xffffffffu;
    DWORD target_session_id_ = 0xffffffffu;
    bool active_ = false;
  };

  class UniversalDisplayManagerEventHandler {
  public:
    explicit UniversalDisplayManagerEventHandler(const bool mark_handled):
        mark_handled_ {mark_handled} {
    }

    HRESULT query_interface(const IID &iid, void **object) {
      if (!object) {
        return E_POINTER;
      }
      std::cerr << "displaymanager_internal_handler_qi="
                << std::hex << std::setfill('0')
                << std::setw(8) << iid.Data1 << '-'
                << std::setw(4) << iid.Data2 << '-'
                << std::setw(4) << iid.Data3 << '-'
                << std::setw(2) << static_cast<unsigned>(iid.Data4[0])
                << std::setw(2) << static_cast<unsigned>(iid.Data4[1]) << '-';
      for (std::size_t index = 2; index < std::size(iid.Data4); ++index) {
        std::cerr << std::setw(2) << static_cast<unsigned>(iid.Data4[index]);
      }
      std::cerr << std::dec << '\n';
      if (IsEqualIID(iid, IID_IUnknown) ||
          IsEqualIID(iid, IID_IInspectable) ||
          IsEqualIID(iid, IID_IAgileObject)) {
        *object = this;
        add_ref();
        return S_OK;
      }
      if (!typed_event_handler_iid_bound_ && (iid.Data3 & 0xf000u) == 0x5000u) {
        typed_event_handler_iid_ = iid;
        typed_event_handler_iid_bound_ = true;
      }
      if (!typed_event_handler_iid_bound_ || !IsEqualIID(iid, typed_event_handler_iid_)) {
        *object = nullptr;
        return E_NOINTERFACE;
      }
      *object = this;
      add_ref();
      return S_OK;
    }

    ULONG add_ref() {
      return ++references_;
    }

    ULONG release() {
      const ULONG remaining = --references_;
      if (remaining == 0) {
        delete this;
      }
      return remaining;
    }

    HRESULT invoke(void *, void *args) {
      if (!mark_handled_ || !args) {
        return S_OK;
      }
      auto **args_vtable = *reinterpret_cast<void ***>(args);
      using PutHandledFn = HRESULT (WINAPI *)(void *, boolean);
      const auto put_handled = reinterpret_cast<PutHandledFn>(args_vtable[7]);
      return put_handled(args, TRUE);
    }

    struct Vtable {
      HRESULT (WINAPI *query_interface)(UniversalDisplayManagerEventHandler *, const IID &, void **);
      ULONG (WINAPI *add_ref)(UniversalDisplayManagerEventHandler *);
      ULONG (WINAPI *release)(UniversalDisplayManagerEventHandler *);
      HRESULT (WINAPI *get_iids)(UniversalDisplayManagerEventHandler *, ULONG *, IID **);
      HRESULT (WINAPI *get_runtime_class_name)(UniversalDisplayManagerEventHandler *, HSTRING *);
      HRESULT (WINAPI *get_trust_level)(UniversalDisplayManagerEventHandler *, TrustLevel *);
      HRESULT (WINAPI *invoke)(UniversalDisplayManagerEventHandler *, void *, void *);
    };

    Vtable *vtable = &s_vtable;

  private:
    static HRESULT WINAPI query_interface_thunk(
      UniversalDisplayManagerEventHandler *self,
      const IID &iid,
      void **object
    ) {
      return self->query_interface(iid, object);
    }

    static ULONG WINAPI add_ref_thunk(UniversalDisplayManagerEventHandler *self) {
      return self->add_ref();
    }

    static ULONG WINAPI release_thunk(UniversalDisplayManagerEventHandler *self) {
      return self->release();
    }

    static HRESULT WINAPI get_iids_thunk(
      UniversalDisplayManagerEventHandler *self,
      ULONG *count,
      IID **iids
    ) {
      if (!count || !iids) {
        return E_POINTER;
      }
      if (!self->typed_event_handler_iid_bound_) {
        *count = 0;
        *iids = nullptr;
        return S_OK;
      }
      auto *result = static_cast<IID *>(CoTaskMemAlloc(sizeof(IID)));
      if (!result) {
        *count = 0;
        *iids = nullptr;
        return E_OUTOFMEMORY;
      }
      *result = self->typed_event_handler_iid_;
      *count = 1;
      *iids = result;
      return S_OK;
    }

    static HRESULT WINAPI get_runtime_class_name_thunk(
      UniversalDisplayManagerEventHandler *,
      HSTRING *name
    ) {
      if (!name) {
        return E_POINTER;
      }
      *name = nullptr;
      return S_OK;
    }

    static HRESULT WINAPI get_trust_level_thunk(
      UniversalDisplayManagerEventHandler *,
      TrustLevel *trust_level
    ) {
      if (!trust_level) {
        return E_POINTER;
      }
      *trust_level = BaseTrust;
      return S_OK;
    }

    static HRESULT WINAPI invoke_thunk(
      UniversalDisplayManagerEventHandler *self,
      void *sender,
      void *args
    ) {
      return self->invoke(sender, args);
    }

    inline static Vtable s_vtable {
      query_interface_thunk,
      add_ref_thunk,
      release_thunk,
      get_iids_thunk,
      get_runtime_class_name_thunk,
      get_trust_level_thunk,
      invoke_thunk
    };

    std::atomic<ULONG> references_ {1};
    bool mark_handled_ = false;
    IID typed_event_handler_iid_ {};
    bool typed_event_handler_iid_bound_ = false;
  };

  int require_active_console_session(const std::string_view command) {
    const DWORD active_session_id = WTSGetActiveConsoleSessionId();
    if (active_session_id == 0xffffffffu) {
      std::cerr << command << " requires an active console session for DisplayConfig and color APIs\n";
      return 1;
    }

    DWORD current_session_id = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &current_session_id)) {
      std::cerr << command << " could not determine the current process session"
                << " native_error=" << GetLastError() << '\n';
      return 1;
    }

    if (current_session_id != active_session_id) {
      std::cerr << command << " must run in the active console session for DisplayConfig and color APIs"
                << " current_session=" << current_session_id
                << " active_session=" << active_session_id << '\n';
      return 1;
    }

    return 0;
  }

  DisplayConfigQueryResult query_display_config_result(const UINT32 flags) {
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    auto result = GetDisplayConfigBufferSizes(flags, &path_count, &mode_count);
    if (result != ERROR_SUCCESS) {
      return {std::nullopt, result};
    }

    if (!vdd::display_config_counts_are_reasonable(path_count, mode_count)) {
      return {std::nullopt, ERROR_INVALID_DATA};
    }

    try {
      std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
      for (int attempt = 0; attempt < 4; ++attempt) {
        UINT32 query_path_count = static_cast<UINT32>(paths.size());
        UINT32 query_mode_count = static_cast<UINT32>(modes.size());
        result = QueryDisplayConfig(
          flags,
          &query_path_count,
          query_path_count == 0 ? nullptr : paths.data(),
          &query_mode_count,
          query_mode_count == 0 ? nullptr : modes.data(),
          nullptr
        );

        if (result == ERROR_SUCCESS) {
          if (!vdd::display_config_counts_are_reasonable(query_path_count, query_mode_count)) {
            return {std::nullopt, ERROR_INVALID_DATA};
          }
          paths.resize(query_path_count);
          modes.resize(query_mode_count);
          return {DisplayConfigData {std::move(paths), std::move(modes), flags}, ERROR_SUCCESS};
        }

        if (result != ERROR_INSUFFICIENT_BUFFER) {
          return {std::nullopt, result};
        }

        const auto next_path_count = (std::max)(query_path_count, static_cast<UINT32>(paths.size() + 1));
        const auto next_mode_count = (std::max)(query_mode_count, static_cast<UINT32>(modes.size() + 1));
        if (!vdd::display_config_counts_are_reasonable(next_path_count, next_mode_count)) {
          return {std::nullopt, ERROR_INVALID_DATA};
        }
        paths.resize(next_path_count);
        modes.resize(next_mode_count);
      }
    } catch (const std::bad_alloc &) {
      return {std::nullopt, ERROR_NOT_ENOUGH_MEMORY};
    }

    return {std::nullopt, result};
  }

  std::optional<DisplayConfigData> query_display_config(const UINT32 flags) {
    return query_display_config_result(flags).data;
  }

  void clear_virtual_mode_indexes(DISPLAYCONFIG_PATH_INFO &path) {
    // With SDC_VIRTUAL_MODE_AWARE these union fields are group/index halves, not
    // raw modeInfoIdx values. Supplying stale query indices makes SetDisplayConfig
    // reject the topology or create a path that cannot be queried afterward.
    path.sourceInfo.sourceModeInfoIdx = DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID;
    path.targetInfo.targetModeInfoIdx = DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;
    path.targetInfo.desktopModeInfoIdx = DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
  }

  void prepare_virtual_topology_path(DISPLAYCONFIG_PATH_INFO &path, const UINT32 clone_group_id, const bool active) {
    clear_virtual_mode_indexes(path);
    path.sourceInfo.cloneGroupId = clone_group_id;
    if (active) {
      path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
    } else {
      path.flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
      path.sourceInfo.cloneGroupId = DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID;
    }
  }

  LONG apply_extended_topology_result() {
    const LONG result = SetDisplayConfig(
      0,
      nullptr,
      0,
      nullptr,
      SDC_APPLY | SDC_TOPOLOGY_EXTEND
    );
    report_helper_event(
      result == ERROR_SUCCESS ? EVENTLOG_INFORMATION_TYPE : EVENTLOG_ERROR_TYPE,
      result == ERROR_SUCCESS ? kEventHelperTopologyApplied : kEventHelperTopologyFailed,
      "Extended topology apply result=" + std::to_string(result)
    );
    return result;
  }

  bool apply_extended_topology() {
    return apply_extended_topology_result() == ERROR_SUCCESS;
  }

  const vdd::DisplayManifestProfile *profile_for_target(
    const vdd::DisplayManifest &manifest,
    const DISPLAYCONFIG_PATH_INFO &path
  ) {
    if (path.targetInfo.outputTechnology != DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL) {
      return nullptr;
    }

    const auto bounded_profile_count =
      (std::min)(manifest.profile_count, vdd::kMaxPermanentDisplayProfiles);
    for (std::uint32_t index = 0; index < bounded_profile_count; ++index) {
      const auto &profile = manifest.profiles[index];
      if (profile.connector_index == path.targetInfo.id && profile.layout_policy != vdd::kDisplayManifestLayoutPolicyNone) {
        return &profile;
      }
    }
    return nullptr;
  }

  std::optional<UINT32> source_mode_index(const DISPLAYCONFIG_PATH_INFO &path, const bool virtual_mode_aware) {
    if (virtual_mode_aware) {
      if (path.sourceInfo.sourceModeInfoIdx == DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID) {
        return std::nullopt;
      }
      return path.sourceInfo.sourceModeInfoIdx;
    }

    if (path.sourceInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
      return std::nullopt;
    }
    return path.sourceInfo.modeInfoIdx;
  }

  struct IddHdrGateEscapeEnvelope {
    D3DKMT_HANDLE adapter_handle;
    std::uint32_t escape_code;
    std::uint32_t reserved;
    std::uint32_t input_size;
    void *input;
    std::uint32_t output_size;
    std::uint32_t output_reserved;
    void *output;
    std::uint32_t returned_output_bytes;
    std::uint32_t result_reserved;
  };

  struct IddHdrGateSupportResult {
    std::uint32_t missing_support_mask;
    std::uint32_t path_index;
  };

  struct IddHdrCurrentModeResult {
    NTSTATUS status;
    D3DKMT_CURRENTDISPLAYMODE mode;
  };

#pragma pack(push, 1)
  struct IddHdrGatePathWire {
    std::uint32_t flags;
    LUID target_luid;
    std::uint32_t target_id;
    LONG position_x;
    LONG position_y;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t rotation;
    std::uint32_t refresh_numerator;
    std::uint32_t refresh_denominator;
    std::uint32_t vsync_divider;
    std::uint32_t monitor_color_mode;
    std::uint32_t monitor_scale_factor;
    std::uint32_t reserved_38;
    std::uint32_t reserved_3c;
    std::uint32_t red_x;
    std::uint32_t red_y;
    std::uint32_t green_x;
    std::uint32_t green_y;
    std::uint32_t blue_x;
    std::uint32_t blue_y;
    std::uint32_t white_x;
    std::uint32_t white_y;
    std::uint32_t min_luminance;
    std::uint32_t max_luminance;
    std::uint32_t max_full_frame_luminance;
    std::uint32_t packed_rgb_bits_per_component;
    std::uint32_t colorimetry_flags;
    std::uint32_t sdr_white_level_nits;
    std::uint64_t reserved_78;
    std::uint32_t reserved_80;
  };
#pragma pack(pop)

  static_assert(sizeof(IddHdrGateEscapeEnvelope) == 0x30);
  static_assert(offsetof(IddHdrGateEscapeEnvelope, adapter_handle) == 0x00);
  static_assert(offsetof(IddHdrGateEscapeEnvelope, escape_code) == 0x04);
  static_assert(offsetof(IddHdrGateEscapeEnvelope, input_size) == 0x0c);
  static_assert(offsetof(IddHdrGateEscapeEnvelope, input) == 0x10);
  static_assert(offsetof(IddHdrGateEscapeEnvelope, output_size) == 0x18);
  static_assert(offsetof(IddHdrGateEscapeEnvelope, output) == 0x20);
  static_assert(offsetof(IddHdrGateEscapeEnvelope, returned_output_bytes) == 0x28);
  static_assert(sizeof(IddHdrGateSupportResult) == 0x08);
  static_assert(sizeof(D3DKMT_CURRENTDISPLAYMODE) == 0x30);
  static_assert(sizeof(IddHdrGatePathWire) == 0x84);
  static_assert(offsetof(IddHdrGatePathWire, flags) == 0x00);
  static_assert(offsetof(IddHdrGatePathWire, target_luid) == 0x04);
  static_assert(offsetof(IddHdrGatePathWire, target_id) == 0x0c);
  static_assert(offsetof(IddHdrGatePathWire, position_x) == 0x10);
  static_assert(offsetof(IddHdrGatePathWire, position_y) == 0x14);
  static_assert(offsetof(IddHdrGatePathWire, width) == 0x18);
  static_assert(offsetof(IddHdrGatePathWire, height) == 0x1c);
  static_assert(offsetof(IddHdrGatePathWire, rotation) == 0x20);
  static_assert(offsetof(IddHdrGatePathWire, refresh_numerator) == 0x24);
  static_assert(offsetof(IddHdrGatePathWire, refresh_denominator) == 0x28);
  static_assert(offsetof(IddHdrGatePathWire, vsync_divider) == 0x2c);
  static_assert(offsetof(IddHdrGatePathWire, monitor_color_mode) == 0x30);
  static_assert(offsetof(IddHdrGatePathWire, monitor_scale_factor) == 0x34);
  static_assert(offsetof(IddHdrGatePathWire, reserved_38) == 0x38);
  static_assert(offsetof(IddHdrGatePathWire, reserved_3c) == 0x3c);
  static_assert(offsetof(IddHdrGatePathWire, red_x) == 0x40);
  static_assert(offsetof(IddHdrGatePathWire, red_y) == 0x44);
  static_assert(offsetof(IddHdrGatePathWire, green_x) == 0x48);
  static_assert(offsetof(IddHdrGatePathWire, green_y) == 0x4c);
  static_assert(offsetof(IddHdrGatePathWire, blue_x) == 0x50);
  static_assert(offsetof(IddHdrGatePathWire, blue_y) == 0x54);
  static_assert(offsetof(IddHdrGatePathWire, white_x) == 0x58);
  static_assert(offsetof(IddHdrGatePathWire, white_y) == 0x5c);
  static_assert(offsetof(IddHdrGatePathWire, min_luminance) == 0x60);
  static_assert(offsetof(IddHdrGatePathWire, max_luminance) == 0x64);
  static_assert(offsetof(IddHdrGatePathWire, max_full_frame_luminance) == 0x68);
  static_assert(offsetof(IddHdrGatePathWire, packed_rgb_bits_per_component) == 0x6c);
  static_assert(offsetof(IddHdrGatePathWire, colorimetry_flags) == 0x70);
  static_assert(offsetof(IddHdrGatePathWire, sdr_white_level_nits) == 0x74);
  static_assert(offsetof(IddHdrGatePathWire, reserved_78) == 0x78);
  static_assert(offsetof(IddHdrGatePathWire, reserved_80) == 0x80);

  IddHdrCurrentModeResult query_idd_current_mode(
    const D3DKMT_HANDLE adapter,
    const D3DDDI_VIDEO_PRESENT_SOURCE_ID source_id
  ) {
    IddHdrCurrentModeResult result {};
    result.mode.VidPnSourceId = source_id;
    D3DKMT_QUERYADAPTERINFO query {};
    query.hAdapter = adapter;
    query.Type = KMTQAITYPE_CURRENTDISPLAYMODE;
    query.pPrivateDriverData = &result.mode;
    query.PrivateDriverDataSize = sizeof(result.mode);
    result.status = D3DKMTQueryAdapterInfo(&query);
    return result;
  }

  LONG apply_nongdi_source_topology_result() {
    constexpr UINT32 query_flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    auto config = query_display_config(query_flags);
    if (!config) {
      std::cout << "nongdi_source_query_failed=1\n";
      return ERROR_INVALID_DATA;
    }

    std::uint32_t changed_modes = 0;
    for (const auto &path: config->paths) {
      if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
        continue;
      }
      const auto mode_index = source_mode_index(path, true);
      if (!mode_index || *mode_index >= config->modes.size()) {
        continue;
      }
      auto &mode = config->modes[*mode_index];
      if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE ||
          !same_luid(mode.adapterId, path.sourceInfo.adapterId) ||
          mode.id != path.sourceInfo.id) {
        continue;
      }
      std::cout << "nongdi_source_mode_index=" << *mode_index
                << " adapter_luid=" << mode.adapterId.HighPart << ':' << mode.adapterId.LowPart
                << " source_id=" << mode.id
                << " previous_pixel_format=" << static_cast<unsigned int>(mode.sourceMode.pixelFormat)
                << '\n';
      mode.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_NONGDI;
      ++changed_modes;
    }
    if (changed_modes == 0) {
      std::cout << "nongdi_source_changed_modes=0\n";
      return ERROR_NOT_FOUND;
    }

    const LONG result = SetDisplayConfig(
      static_cast<UINT32>(config->paths.size()),
      config->paths.data(),
      static_cast<UINT32>(config->modes.size()),
      config->modes.data(),
      SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_VIRTUAL_MODE_AWARE
    );
    std::cout << "nongdi_source_changed_modes=" << changed_modes
              << " apply_result=" << result << '\n';
    return result;
  }

  struct DisplayConfigStateTokenWire {
    std::uint32_t gdi_display_uniqueness;
    std::uint32_t monitor_uniqueness;
    std::uint32_t adapter_population_uniqueness;
    GUID activity_id;
  };

  struct alignas(8) DisplayConfigPathInfoInternalWire {
    std::array<std::byte, 0xd8> bytes;
  };

  static_assert(sizeof(DisplayConfigStateTokenWire) == 0x1c);
  static_assert(sizeof(DisplayConfigPathInfoInternalWire) == 0xd8);

  std::uint32_t internal_path_u32(
    const DisplayConfigPathInfoInternalWire &path,
    const std::size_t offset
  ) {
    std::uint32_t value {};
    std::memcpy(&value, path.bytes.data() + offset, sizeof(value));
    return value;
  }

  std::uint64_t internal_path_u64(
    const DisplayConfigPathInfoInternalWire &path,
    const std::size_t offset
  ) {
    std::uint64_t value {};
    std::memcpy(&value, path.bytes.data() + offset, sizeof(value));
    return value;
  }

  void set_internal_path_u32(
    DisplayConfigPathInfoInternalWire &path,
    const std::size_t offset,
    const std::uint32_t value
  ) {
    std::memcpy(path.bytes.data() + offset, &value, sizeof(value));
  }

  void set_internal_path_u64(
    DisplayConfigPathInfoInternalWire &path,
    const std::size_t offset,
    const std::uint64_t value
  ) {
    std::memcpy(path.bytes.data() + offset, &value, sizeof(value));
  }

  void print_internal_path_summary(
    const std::string_view prefix,
    const std::vector<DisplayConfigPathInfoInternalWire> &paths,
    const UINT32 path_count
  ) {
    const auto bounded_count = (std::min)(static_cast<std::size_t>(path_count), paths.size());
    for (std::size_t index = 0; index < bounded_count; ++index) {
      const auto &path = paths[index];
      std::cout << prefix << "_path[" << index << "]"
                << " flags=0x" << std::hex << std::setw(16) << std::setfill('0')
                << internal_path_u64(path, 0)
                << " modality_mask=0x" << std::setw(16)
                << internal_path_u64(path, 0x08)
                << " source_pixel_format=0x" << std::setw(8)
                << internal_path_u32(path, 0x60) << std::dec << '\n';
      std::cout << prefix << "_path[" << index << "]_bytes=";
      for (const auto byte: path.bytes) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned int>(std::to_integer<unsigned char>(byte));
      }
      std::cout << std::dec << '\n';
    }
  }

  int query_private_functionalize_current_session() {
    constexpr WORD query_display_config2_ordinal = 2619;
    constexpr WORD functionalize_display_config_ordinal = 2620;
    constexpr std::uint32_t source_pixel_format_offset = 0x60;
    constexpr std::uint32_t fp16_ddi_format = 0x71;

    using QueryDisplayConfig2Fn = LONG (WINAPI *)(
      UINT32,
      UINT32 *,
      DisplayConfigPathInfoInternalWire *,
      DISPLAYCONFIG_TOPOLOGY_ID *,
      DisplayConfigStateTokenWire *
    );
    using FunctionalizeDisplayConfigFn = LONG (WINAPI *)(
      UINT32,
      UINT32 *,
      DisplayConfigPathInfoInternalWire *,
      DisplayConfigStateTokenWire *,
      void *,
      DISPLAYCONFIG_TOPOLOGY_ID *
    );
    using NtUserFunctionalizeDisplayConfigFn = LONG (WINAPI *)(
      UINT32,
      UINT32 *,
      DisplayConfigPathInfoInternalWire *,
      DisplayConfigStateTokenWire *,
      void *,
      DISPLAYCONFIG_TOPOLOGY_ID *
    );

    const HMODULE private_api = LoadLibraryW(L"api-ms-win-rtcore-ntuser-private-l1-1-5.dll");
    if (!private_api) {
      std::cerr << "private_functionalize_load_error=" << GetLastError() << '\n';
      return 1;
    }
    const auto query_display_config2 = reinterpret_cast<QueryDisplayConfig2Fn>(
      GetProcAddress(private_api, MAKEINTRESOURCEA(query_display_config2_ordinal))
    );
    const auto functionalize_display_config = reinterpret_cast<FunctionalizeDisplayConfigFn>(
      GetProcAddress(private_api, MAKEINTRESOURCEA(functionalize_display_config_ordinal))
    );
    const HMODULE win32u = LoadLibraryW(L"win32u.dll");
    const auto nt_functionalize_display_config = win32u ?
      reinterpret_cast<NtUserFunctionalizeDisplayConfigFn>(
        GetProcAddress(win32u, "NtUserFunctionalizeDisplayConfig")
      ) : nullptr;
    std::cout << "private_query_resolved=" << static_cast<int>(query_display_config2 != nullptr)
              << " private_functionalize_resolved="
              << static_cast<int>(functionalize_display_config != nullptr)
              << " nt_functionalize_resolved="
              << static_cast<int>(nt_functionalize_display_config != nullptr) << '\n';
    if (!query_display_config2 || !functionalize_display_config || !nt_functionalize_display_config) {
      if (win32u) {
        FreeLibrary(win32u);
      }
      FreeLibrary(private_api);
      return 1;
    }

    UINT32 query_flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    LONG size_result = GetDisplayConfigBufferSizes(query_flags, &path_count, &mode_count);
    if (size_result != ERROR_SUCCESS) {
      query_flags = QDC_ONLY_ACTIVE_PATHS;
      size_result = GetDisplayConfigBufferSizes(query_flags, &path_count, &mode_count);
    }
    std::cout << "private_query_flags=" << query_flags
              << " size_result=" << size_result
              << " path_capacity=" << path_count
              << " public_mode_count=" << mode_count << '\n';
    if (size_result != ERROR_SUCCESS || path_count == 0 || path_count > 64) {
      FreeLibrary(private_api);
      return 1;
    }

    std::vector<DisplayConfigPathInfoInternalWire> paths(path_count);
    DisplayConfigStateTokenWire state_token {};
    DISPLAYCONFIG_TOPOLOGY_ID topology_id = static_cast<DISPLAYCONFIG_TOPOLOGY_ID>(0);
    UINT32 queried_path_count = path_count;
    LONG query_result = query_display_config2(
      query_flags,
      &queried_path_count,
      paths.data(),
      nullptr,
      &state_token
    );
    if (query_result != ERROR_SUCCESS && query_flags != QDC_ONLY_ACTIVE_PATHS) {
      query_flags = QDC_ONLY_ACTIVE_PATHS;
      path_count = 0;
      mode_count = 0;
      size_result = GetDisplayConfigBufferSizes(query_flags, &path_count, &mode_count);
      if (size_result == ERROR_SUCCESS && path_count > 0 && path_count <= 64) {
        paths.assign(path_count, {});
        state_token = {};
        queried_path_count = path_count;
        query_result = query_display_config2(
          query_flags,
          &queried_path_count,
          paths.data(),
          nullptr,
          &state_token
        );
        std::cout << "private_query_retry_flags=" << query_flags
                  << " size_result=" << size_result
                  << " path_capacity=" << path_count
                  << " public_mode_count=" << mode_count << '\n';
      }
    }
    std::cout << "private_query_result=" << query_result
              << " path_count=" << queried_path_count
              << " topology_id=" << static_cast<unsigned int>(topology_id)
              << " state=" << state_token.gdi_display_uniqueness << ':'
              << state_token.monitor_uniqueness << ':'
              << state_token.adapter_population_uniqueness << '\n';
    if (query_result != ERROR_SUCCESS || queried_path_count == 0 || queried_path_count > paths.size()) {
      FreeLibrary(private_api);
      return 1;
    }
    print_internal_path_summary("private_query", paths, queried_path_count);

    auto unchanged_paths = paths;
    auto unchanged_state = state_token;
    auto unchanged_topology = topology_id;
    UINT32 unchanged_count = queried_path_count;
    const LONG unchanged_status = nt_functionalize_display_config(
      0,
      &unchanged_count,
      unchanged_paths.data(),
      &unchanged_state,
      nullptr,
      &unchanged_topology
    );
    std::cout << "private_functionalize_unchanged_status=0x"
              << std::hex << std::setw(8) << std::setfill('0')
              << static_cast<std::uint32_t>(unchanged_status) << std::dec
              << " path_count=" << unchanged_count
              << " topology_id=" << static_cast<unsigned int>(unchanged_topology) << '\n';
    print_internal_path_summary("private_functionalize_unchanged", unchanged_paths, unchanged_count);

    auto fp16_paths = paths;
    for (UINT32 index = 0; index < queried_path_count; ++index) {
      set_internal_path_u32(fp16_paths[index], source_pixel_format_offset, fp16_ddi_format);
    }
    auto fp16_state = state_token;
    auto fp16_topology = topology_id;
    UINT32 fp16_count = queried_path_count;
    const LONG fp16_status = nt_functionalize_display_config(
      0,
      &fp16_count,
      fp16_paths.data(),
      &fp16_state,
      nullptr,
      &fp16_topology
    );
    std::cout << "private_functionalize_fp16_status=0x"
              << std::hex << std::setw(8) << std::setfill('0')
              << static_cast<std::uint32_t>(fp16_status) << std::dec
              << " path_count=" << fp16_count
              << " topology_id=" << static_cast<unsigned int>(fp16_topology) << '\n';
    print_internal_path_summary("private_functionalize_fp16", fp16_paths, fp16_count);

    auto no_state_paths = paths;
    auto no_state_topology = topology_id;
    UINT32 no_state_count = queried_path_count;
    const LONG no_state_status = nt_functionalize_display_config(
      0,
      &no_state_count,
      no_state_paths.data(),
      nullptr,
      nullptr,
      &no_state_topology
    );
    std::cout << "private_functionalize_no_state_status=0x"
              << std::hex << std::setw(8) << std::setfill('0')
              << static_cast<std::uint32_t>(no_state_status) << std::dec
              << " path_count=" << no_state_count
              << " topology_id=" << static_cast<unsigned int>(no_state_topology) << '\n';
    print_internal_path_summary("private_functionalize_no_state", no_state_paths, no_state_count);

    auto fp16_no_state_paths = paths;
    for (UINT32 index = 0; index < queried_path_count; ++index) {
      set_internal_path_u32(fp16_no_state_paths[index], source_pixel_format_offset, fp16_ddi_format);
    }
    auto fp16_no_state_topology = topology_id;
    UINT32 fp16_no_state_count = queried_path_count;
    const LONG fp16_no_state_status = nt_functionalize_display_config(
      0,
      &fp16_no_state_count,
      fp16_no_state_paths.data(),
      nullptr,
      nullptr,
      &fp16_no_state_topology
    );
    std::cout << "private_functionalize_fp16_no_state_status=0x"
              << std::hex << std::setw(8) << std::setfill('0')
              << static_cast<std::uint32_t>(fp16_no_state_status) << std::dec
              << " path_count=" << fp16_no_state_count
              << " topology_id=" << static_cast<unsigned int>(fp16_no_state_topology) << '\n';
    print_internal_path_summary(
      "private_functionalize_fp16_no_state",
      fp16_no_state_paths,
      fp16_no_state_count
    );
    FreeLibrary(win32u);
    FreeLibrary(private_api);
    return no_state_status >= 0 && fp16_no_state_status >= 0 ? 0 : 1;
  }

  int probe_displaymanager_fp16_functionalize_current_session(
    const bool enforce_source_ownership,
    const bool start_for_desktop,
    const bool claim_display_manager_owner = false
  ) {
#ifdef _MSC_VER
    using namespace winrt::Windows::Devices::Display::Core;
    using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
    ScopedConsoleUserImpersonation impersonation;
    DWORD impersonation_error = ERROR_SUCCESS;
    DWORD session_id = 0xffffffffu;
    const bool have_session_id = ProcessIdToSessionId(GetCurrentProcessId(), &session_id) != FALSE;
    const bool impersonating = !enforce_source_ownership ||
      (have_session_id && impersonation.begin(session_id, &impersonation_error));
    char impersonated_user_name[256] {};
    DWORD impersonated_user_name_size = static_cast<DWORD>(std::size(impersonated_user_name));
    const bool have_impersonated_user_name = enforce_source_ownership && impersonating &&
      GetUserNameA(impersonated_user_name, &impersonated_user_name_size) != FALSE;
    std::cout << "displaymanager_console_user_impersonation="
              << static_cast<int>(enforce_source_ownership && impersonating)
              << " source_session=" << impersonation.source_session_id()
              << " target_session=" << impersonation.target_session_id()
              << " identity=" << (have_impersonated_user_name ? impersonated_user_name : "unchanged")
              << " native_error=" << impersonation_error << '\n';
    if (!impersonating) {
      return 1;
    }
    struct VidPnOwnerGuard {
      D3DKMT_HANDLE adapter {};
      D3DKMT_HANDLE device {};
      HANDLE source_presentation_handle {};
      std::vector<HANDLE> broker_manager_handles;
      bool claimed {};

      ~VidPnOwnerGuard() {
        if (claimed) {
          (void) D3DKMTReleaseProcessVidPnSourceOwners(GetCurrentProcess());
        }
        if (source_presentation_handle) {
          CloseHandle(source_presentation_handle);
        }
        for (const auto handle: broker_manager_handles) {
          if (handle) {
            CloseHandle(handle);
          }
        }
        if (device != 0) {
          D3DKMT_DESTROYDEVICE destroy {device};
          (void) D3DKMTDestroyDevice(&destroy);
        }
        if (adapter != 0) {
          D3DKMT_CLOSEADAPTER close {adapter};
          (void) D3DKMTCloseAdapter(&close);
        }
      }
    } owner_guard;
    void *display_manager_nt_handle = nullptr;
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      const auto manager = DisplayManager::Create(
        enforce_source_ownership ? DisplayManagerOptions::EnforceSourceOwnership : DisplayManagerOptions::None
      );
      winrt::com_ptr<::IUnknown> desktop_context;
      if (start_for_desktop) {
        // This proof is deliberately pinned to the signed Windows 11 26100.8972
        // DispBroker.dll implementation inspected alongside this experiment.
        // The cloaked IDisplayManagerForDesktop interface lives at object
        // offset 0x20. The production desktop endpoint already belongs to DWM;
        // preserve the returned desktop context and continue only when startup
        // reports that exact name collision. Refuse to call it unless every
        // inspected vtable entry and prologue match this build.
        constexpr std::uintptr_t start_for_desktop_rva = 0x1a200;
        constexpr std::uintptr_t get_disp_mgr_handle_rva = 0x20568;
        constexpr std::size_t start_for_desktop_slot = 24;
        constexpr std::ptrdiff_t desktop_interface_offset = 0x20;
        constexpr std::array<unsigned char, 16> expected_prologue {
          0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
          0xc2, 0xc6, 0x81, 0xb8, 0x01, 0x00, 0x00, 0x01
        };
        constexpr std::array<unsigned char, 16> expected_get_handle_prologue {
          0x48, 0x83, 0x22, 0x00, 0x48, 0x83, 0x62, 0x08,
          0x00, 0x48, 0x8b, 0x81, 0x48, 0x02, 0x00, 0x00
        };
        constexpr std::array<std::size_t, 6> add_handler_slots {6, 8, 10, 12, 14, 16};
        constexpr std::array<std::uintptr_t, 6> add_handler_rvas {
          0x20be0,
          0x20a40,
          0x203f0,
          0x1f2d0,
          0x348d0,
          0x1ecd0
        };
        const auto dispbroker = GetModuleHandleW(L"DispBroker.dll");
        if (!dispbroker) {
          std::cerr << "displaymanager_desktop_start_module_error=" << GetLastError() << '\n';
          return 1;
        }
        const auto *start_bytes = reinterpret_cast<const unsigned char *>(dispbroker) +
                                  start_for_desktop_rva;
        const bool signature_matches = std::memcmp(
          start_bytes,
          expected_prologue.data(),
          expected_prologue.size()
        ) == 0;
        const auto *get_handle_bytes = reinterpret_cast<const unsigned char *>(dispbroker) +
                                       get_disp_mgr_handle_rva;
        const bool get_handle_signature_matches = std::memcmp(
          get_handle_bytes,
          expected_get_handle_prologue.data(),
          expected_get_handle_prologue.size()
        ) == 0;
        std::cout << "displaymanager_desktop_start_signature_match="
                  << static_cast<int>(signature_matches) << '\n';
        std::cout << "displaymanager_get_handle_signature_match="
                  << static_cast<int>(get_handle_signature_matches) << '\n';
        if (!signature_matches || !get_handle_signature_matches) {
          return 1;
        }

        const auto mark_handled = [](const auto &, const auto &args) {
          args.Handled(true);
        };
        const auto enabled_token = manager.Enabled(mark_handled);
        const auto disabled_token = manager.Disabled(mark_handled);
        const auto changed_token = manager.Changed(mark_handled);
        const auto paths_failed_token = manager.PathsFailedOrInvalidated(
          mark_handled
        );
        (void) enabled_token;
        (void) disabled_token;
        (void) changed_token;
        (void) paths_failed_token;

        auto *desktop_interface = reinterpret_cast<std::byte *>(winrt::get_abi(manager)) +
                                  desktop_interface_offset;
        auto **desktop_vtable = *reinterpret_cast<void ***>(desktop_interface);
        bool handler_vtable_matches = true;
        for (std::size_t index = 0; index < add_handler_slots.size(); ++index) {
          const auto expected = reinterpret_cast<std::byte *>(dispbroker) + add_handler_rvas[index];
          handler_vtable_matches = handler_vtable_matches &&
                                   desktop_vtable[add_handler_slots[index]] == expected;
        }
        handler_vtable_matches = handler_vtable_matches &&
                                 desktop_vtable[start_for_desktop_slot] ==
                                   reinterpret_cast<std::byte *>(dispbroker) +
                                     start_for_desktop_rva;
        std::cout << "displaymanager_desktop_vtable_match="
                  << static_cast<int>(handler_vtable_matches) << '\n';
        if (!handler_vtable_matches) {
          return 1;
        }

        std::array<winrt::com_ptr<::IUnknown>, add_handler_slots.size()> internal_handlers;
        for (std::size_t index = 0; index < internal_handlers.size(); ++index) {
          internal_handlers[index].attach(reinterpret_cast<::IUnknown *>(
            new UniversalDisplayManagerEventHandler {index != 3 && index != 4}
          ));
        }
        using AddHandlerFn = HRESULT (WINAPI *)(void *, void *, winrt::event_token *);
        std::array<winrt::event_token, add_handler_slots.size()> internal_tokens {};
        for (std::size_t index = 0; index < add_handler_slots.size(); ++index) {
          const auto add_handler = reinterpret_cast<AddHandlerFn>(
            desktop_vtable[add_handler_slots[index]]
          );
          const HRESULT add_result = add_handler(
            desktop_interface,
            static_cast<void *>(internal_handlers[index].get()),
            &internal_tokens[index]
          );
          std::cout << "displaymanager_desktop_handler[" << index << "]_result=0x"
                    << std::hex << std::setw(8) << std::setfill('0')
                    << static_cast<std::uint32_t>(add_result) << std::dec << '\n';
          if (FAILED(add_result)) {
            return 1;
          }
        }

        using StartForDesktopFn = HRESULT (WINAPI *)(void *, ::IUnknown **);
        const auto start = reinterpret_cast<StartForDesktopFn>(
          desktop_vtable[start_for_desktop_slot]
        );
        ::IUnknown *context = nullptr;
        const HRESULT start_result = start(desktop_interface, &context);
        desktop_context.attach(context);
        std::cout << "displaymanager_desktop_start_result=0x"
                  << std::hex << std::setw(8) << std::setfill('0')
                  << static_cast<std::uint32_t>(start_result) << std::dec
                  << " context_present=" << static_cast<int>(desktop_context != nullptr) << '\n';
        // The inspected GetDispMgrHandle implementation reads the raw kernel
        // handle from object offset 0x240 and its shared lifetime storage from
        // 0x248. Read those fields while the manager is alive instead of
        // calling the private helper, which would add a reference that this
        // diagnostic process would then have to release with private code.
        const auto *manager_object = reinterpret_cast<const std::byte *>(winrt::get_abi(manager));
        display_manager_nt_handle =
          *reinterpret_cast<void *const *>(manager_object + 0x240);
        const auto display_manager_handle_storage =
          *reinterpret_cast<void *const *>(manager_object + 0x248);
        std::cout << "displaymanager_nt_handle_present="
                  << static_cast<int>(display_manager_nt_handle != nullptr)
                  << " lifetime_storage_present="
                  << static_cast<int>(display_manager_handle_storage != nullptr)
                  << " nt_handle=0x" << std::hex
                  << reinterpret_cast<std::uintptr_t>(display_manager_nt_handle)
                  << std::dec << '\n';
        const HRESULT already_exists = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        if ((!SUCCEEDED(start_result) && start_result != already_exists) || !desktop_context) {
          return 1;
        }
      }
      const auto targets = manager.GetCurrentTargets();
      std::vector<DisplayTarget> connected_targets;
      connected_targets.reserve(targets.Size());
      std::uint32_t target_index = 0;
      for (const auto &target: targets) {
        std::cout << "displaymanager_target[" << target_index << "]"
                  << " connected=" << static_cast<int>(target.IsConnected())
                  << " stale=" << static_cast<int>(target.IsStale())
                  << " usage_kind=" << static_cast<std::int32_t>(target.UsageKind())
                  << " virtual_mode=" << static_cast<int>(target.IsVirtualModeEnabled())
                  << " virtual_topology=" << static_cast<int>(target.IsVirtualTopologyEnabled())
                  << " adapter_relative_id=" << target.AdapterRelativeId() << '\n';
        if (target.IsConnected() && !target.IsStale()) {
          connected_targets.push_back(target);
        }
        ++target_index;
      }
      std::cout << "displaymanager_target_count=" << targets.Size()
                << " connected_target_count=" << connected_targets.size()
                << " enforce_source_ownership=" << static_cast<int>(enforce_source_ownership) << '\n';
      const auto result = manager.TryAcquireTargetsAndReadCurrentState(connected_targets);
      auto state = result.State();
      std::cout << "displaymanager_acquire_result="
                << static_cast<std::int32_t>(result.ErrorCode())
                << " extended=0x" << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<std::uint32_t>(result.ExtendedErrorCode().value)
                << std::dec << " state_present=" << static_cast<int>(state != nullptr) << '\n';
      if (!state && start_for_desktop) {
        const auto mode_query_result = manager.TryReadCurrentStateForModeQuery();
        state = mode_query_result.State();
        std::cout << "displaymanager_mode_query_result="
                  << static_cast<std::int32_t>(mode_query_result.ErrorCode())
                  << " extended=0x" << std::hex << std::setw(8) << std::setfill('0')
                  << static_cast<std::uint32_t>(mode_query_result.ExtendedErrorCode().value)
                  << std::dec << " state_present=" << static_cast<int>(state != nullptr) << '\n';
      }
      if (!state) {
        manager.Stop();
        return 1;
      }

      std::cout << "displaymanager_state_read_only=" << static_cast<int>(state.IsReadOnly())
                << " stale=" << static_cast<int>(state.IsStale()) << '\n';
      std::uint32_t path_count = 0;
      for (const auto &view: state.Views()) {
        for (const auto &path: view.Paths()) {
          std::cout << "displaymanager_path[" << path_count << "]_source_pixel_before="
                    << static_cast<std::int32_t>(path.SourcePixelFormat()) << '\n';
          const auto wire_before = path.WireFormat();
          if (wire_before) {
            std::cout << "displaymanager_path[" << path_count << "]_wire_before="
                      << "encoding:" << static_cast<std::int32_t>(wire_before.PixelEncoding())
                      << ",bpc:" << wire_before.BitsPerChannel()
                      << ",colorspace:" << static_cast<std::int32_t>(wire_before.ColorSpace())
                      << ",eotf:" << static_cast<std::int32_t>(wire_before.Eotf())
                      << ",metadata:" << static_cast<std::int32_t>(wire_before.HdrMetadata()) << '\n';
          } else {
            std::cout << "displaymanager_path[" << path_count << "]_wire_before=null\n";
          }
          DisplayWireFormat requested_wire {nullptr};
          try {
            path.SourcePixelFormat(DirectXPixelFormat::R16G16B16A16Float);
            if (start_for_desktop) {
              requested_wire = DisplayWireFormat(
                DisplayWireFormatPixelEncoding::Rgb444,
                10,
                DisplayWireFormatColorSpace::BT2020,
                DisplayWireFormatEotf::HdrSmpte2084,
                DisplayWireFormatHdrMetadata::Hdr10
              );
              path.WireFormat(requested_wire);
            }
            std::cout << "displaymanager_path[" << path_count
                      << "]_constraint_result=0x00000000\n";
          } catch (const winrt::hresult_error &error) {
            std::cout << "displaymanager_path[" << path_count << "]_constraint_result=0x"
                      << std::hex << std::setw(8) << std::setfill('0')
                      << static_cast<std::uint32_t>(error.code().value) << std::dec << '\n';
          }
          std::cout << "displaymanager_path[" << path_count << "]_source_pixel_requested="
                    << static_cast<std::int32_t>(path.SourcePixelFormat()) << '\n';
          try {
            const auto modes = path.FindModes(DisplayModeQueryOptions::None);
            std::cout << "displaymanager_path[" << path_count << "]_mode_count="
                      << modes.Size() << '\n';
            std::uint32_t mode_index = 0;
            for (const auto &mode: modes) {
              std::cout << "displaymanager_path[" << path_count << "]_mode["
                        << mode_index << "]_source_pixel="
                        << static_cast<std::int32_t>(mode.SourcePixelFormat()) << '\n';
              try {
                const bool hdr_wire_supported = requested_wire &&
                                                mode.IsWireFormatSupported(requested_wire);
                std::cout << "displaymanager_path[" << path_count << "]_mode["
                          << mode_index << "]_hdr_wire_supported="
                          << static_cast<int>(hdr_wire_supported) << '\n';
              } catch (const winrt::hresult_error &error) {
                std::cout << "displaymanager_path[" << path_count << "]_mode["
                          << mode_index << "]_hdr_wire_result=0x"
                          << std::hex << std::setw(8) << std::setfill('0')
                          << static_cast<std::uint32_t>(error.code().value) << std::dec << '\n';
              }
              try {
                const auto bpc = mode.GetWireFormatSupportedBitsPerChannel(
                  DisplayWireFormatPixelEncoding::Rgb444
                );
                std::cout << "displaymanager_path[" << path_count << "]_mode["
                          << mode_index << "]_rgb_bpc_flags="
                          << static_cast<std::uint32_t>(bpc) << '\n';
              } catch (const winrt::hresult_error &error) {
                std::cout << "displaymanager_path[" << path_count << "]_mode["
                          << mode_index << "]_rgb_bpc_result=0x"
                          << std::hex << std::setw(8) << std::setfill('0')
                          << static_cast<std::uint32_t>(error.code().value) << std::dec << '\n';
              }
              if (++mode_index >= 20) {
                break;
              }
            }
          } catch (const winrt::hresult_error &error) {
            std::cout << "displaymanager_path[" << path_count << "]_find_modes_result=0x"
                      << std::hex << std::setw(8) << std::setfill('0')
                      << static_cast<std::uint32_t>(error.code().value) << std::dec << '\n';
          }
          ++path_count;
        }
      }
      bool owner2_claimed = false;
      if (claim_display_manager_owner) {
        const auto active_config = query_display_config(
          QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE
        );
        DisplayPath owner_path {nullptr};
        std::uint32_t owner_path_count = 0;
        for (const auto &view: state.Views()) {
          for (const auto &path: view.Paths()) {
            if (owner_path_count == 0) {
              owner_path = path;
            }
            ++owner_path_count;
          }
        }
        if (!active_config || active_config->paths.size() != 1 ||
            owner_path_count != 1 || !owner_path) {
          std::cout << "displaymanager_owner2_precondition=0"
                    << " active_paths="
                    << (active_config ? active_config->paths.size() : 0)
                    << " display_paths=" << owner_path_count << '\n';
        } else {
          const auto &active_path = active_config->paths.front();
          winrt::com_ptr<IDisplayPathInterop> path_interop;
          const HRESULT interop_result = winrt::get_unknown(owner_path)->QueryInterface(
            IID_PPV_ARGS(path_interop.put())
          );
          UINT interop_source_id = (std::numeric_limits<UINT>::max)();
          const HRESULT source_id_result = SUCCEEDED(interop_result) ?
            path_interop->GetSourceId(&interop_source_id) : interop_result;
          const bool source_id_matches = SUCCEEDED(source_id_result) &&
                                         interop_source_id == active_path.sourceInfo.id;
          const HRESULT handle_result = source_id_matches ?
            path_interop->CreateSourcePresentationHandle(
              &owner_guard.source_presentation_handle
            ) : E_INVALIDARG;
          std::cout << "displaymanager_owner2_path_interop_result=0x"
                    << std::hex << std::setw(8) << std::setfill('0')
                    << static_cast<std::uint32_t>(interop_result)
                    << " source_id_result=0x" << std::setw(8)
                    << static_cast<std::uint32_t>(source_id_result)
                    << " handle_result=0x" << std::setw(8)
                    << static_cast<std::uint32_t>(handle_result) << std::dec
                    << " interop_source_id=" << interop_source_id
                    << " qdc_source_id=" << active_path.sourceInfo.id
                    << " source_id_matches=" << static_cast<int>(source_id_matches)
                    << " handle_present="
                    << static_cast<int>(owner_guard.source_presentation_handle != nullptr)
                    << '\n';
          if (source_id_matches && !owner_guard.source_presentation_handle) {
            constexpr auto broker_path =
              "C:\\Users\\VibeSeatTest\\AppData\\Local\\Temp\\"
              "displaymanager-owner2-broker-handles.txt";
            DWORD current_session_id = 0xffffffffu;
            (void) ProcessIdToSessionId(GetCurrentProcessId(), &current_session_id);
            bool broker_file_valid = false;
            for (std::uint32_t attempt = 0; attempt < 120 && !broker_file_valid; ++attempt) {
              std::ifstream broker {broker_path};
              DWORD broker_process_id = 0;
              DWORD broker_session_id = 0xffffffffu;
              std::vector<HANDLE> manager_handles;
              std::string line;
              while (std::getline(broker, line)) {
                try {
                  if (line.starts_with("pid=")) {
                    broker_process_id = static_cast<DWORD>(std::stoul(line.substr(4), nullptr, 10));
                  } else if (line.starts_with("session=")) {
                    broker_session_id = static_cast<DWORD>(std::stoul(line.substr(8), nullptr, 10));
                  } else if (line.starts_with("handle=0x")) {
                    manager_handles.push_back(reinterpret_cast<HANDLE>(
                      static_cast<std::uintptr_t>(std::stoull(line.substr(9), nullptr, 16))
                    ));
                  }
                } catch (const std::exception &) {
                  manager_handles.clear();
                  break;
                }
              }
              broker_file_valid = broker_process_id == GetCurrentProcessId() &&
                                  broker_session_id == current_session_id &&
                                  !manager_handles.empty();
              if (broker_file_valid) {
                owner_guard.broker_manager_handles = std::move(manager_handles);
                break;
              }
              if (attempt == 0) {
                std::cout << "displaymanager_owner2_broker_wait_ms=30000"
                          << " broker_path=" << broker_path << '\n' << std::flush;
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            std::cout << "displaymanager_owner2_broker_file_valid="
                      << static_cast<int>(broker_file_valid)
                      << " candidate_count="
                      << owner_guard.broker_manager_handles.size() << '\n';

            struct DisplayManagerSourceOperation {
              std::uint32_t operation;
              std::uint32_t reserved0;
              HANDLE manager_handle;
              LUID adapter_luid;
              std::uint32_t source_id;
              std::uint32_t reserved1;
              OBJECT_ATTRIBUTES *object_attributes;
              ACCESS_MASK desired_access;
              std::uint32_t reserved2;
              HANDLE source_handle;
            };
            static_assert(sizeof(DisplayManagerSourceOperation) == 0x38);
            using SourceOperationFn = NTSTATUS (WINAPI *)(DisplayManagerSourceOperation *);
            const auto gdi32 = GetModuleHandleW(L"gdi32.dll");
            const auto source_operation = gdi32 ? reinterpret_cast<SourceOperationFn>(
              GetProcAddress(gdi32, "D3DKMTDispMgrSourceOperation")
            ) : nullptr;
            std::cout << "displaymanager_owner2_source_operation_present="
                      << static_cast<int>(source_operation != nullptr) << '\n';
            if (broker_file_valid && source_operation) {
              for (std::size_t index = 0;
                   index < owner_guard.broker_manager_handles.size();
                   ++index) {
                OBJECT_ATTRIBUTES attributes;
                InitializeObjectAttributes(&attributes, nullptr, 0, nullptr, nullptr);
                DisplayManagerSourceOperation operation {};
                operation.operation = 4;
                operation.manager_handle = owner_guard.broker_manager_handles[index];
                operation.adapter_luid = active_path.sourceInfo.adapterId;
                operation.source_id = active_path.sourceInfo.id;
                operation.object_attributes = &attributes;
                operation.desired_access = GENERIC_ALL;
                const NTSTATUS status = source_operation(&operation);
                std::cout << "displaymanager_owner2_broker_candidate[" << index
                          << "]_status=0x" << std::hex << std::setw(8)
                          << std::setfill('0') << static_cast<std::uint32_t>(status)
                          << std::dec << " handle_present="
                          << static_cast<int>(operation.source_handle != nullptr) << '\n';
                if (status >= 0 && operation.source_handle) {
                  owner_guard.source_presentation_handle = operation.source_handle;
                  break;
                }
                if (operation.source_handle) {
                  CloseHandle(operation.source_handle);
                }
              }
            }
          }
          const bool use_source_presentation_handle =
            owner_guard.source_presentation_handle != nullptr;
          const std::size_t ownership_handle_count = use_source_presentation_handle ?
            1 : owner_guard.broker_manager_handles.size();
          if (FAILED(interop_result) || FAILED(source_id_result) ||
              ownership_handle_count == 0) {
            std::cout << "displaymanager_owner2_precondition=0"
                      << " ownership_handle_present=0\n";
          } else {
            D3DKMT_OPENADAPTERFROMLUID open {};
            open.AdapterLuid = active_path.sourceInfo.adapterId;
            const auto open_status = D3DKMTOpenAdapterFromLuid(&open);
            std::cout << "displaymanager_owner2_open_status=0x"
                      << std::hex << std::setw(8) << std::setfill('0')
                      << static_cast<std::uint32_t>(open_status) << std::dec << '\n';
            if (open_status >= 0) {
              owner_guard.adapter = open.hAdapter;
              D3DKMT_CREATEDEVICE create {};
              create.hAdapter = open.hAdapter;
              const auto create_status = D3DKMTCreateDevice(&create);
              std::cout << "displaymanager_owner2_create_status=0x"
                        << std::hex << std::setw(8) << std::setfill('0')
                        << static_cast<std::uint32_t>(create_status) << std::dec << '\n';
              if (create_status >= 0) {
                owner_guard.device = create.hDevice;
                const D3DKMT_VIDPNSOURCEOWNER_TYPE owner_type =
                  D3DKMT_VIDPNSOURCEOWNER_EXCLUSIVE;
                const D3DDDI_VIDEO_PRESENT_SOURCE_ID source_id =
                  active_path.sourceInfo.id;
                for (std::size_t index = 0; index < ownership_handle_count; ++index) {
                  const HANDLE candidate = use_source_presentation_handle ?
                    owner_guard.source_presentation_handle :
                    owner_guard.broker_manager_handles[index];
                  const D3DKMT_PTR_TYPE owner_nt_handle =
                    reinterpret_cast<D3DKMT_PTR_TYPE>(candidate);
                  D3DKMT_SETVIDPNSOURCEOWNER2 owner {};
                  owner.Version1.Version0.hDevice = create.hDevice;
                  owner.Version1.Version0.pType = &owner_type;
                  owner.Version1.Version0.pVidPnSourceId = &source_id;
                  owner.Version1.Version0.VidPnSourceCount = 1;
                  owner.Version1.Flags.UseNtHandles = 1;
                  owner.pVidPnSourceNtHandles = &owner_nt_handle;
                  const auto owner_status = D3DKMTSetVidPnSourceOwner2(&owner);
                  owner_guard.claimed = owner_status >= 0;
                  owner2_claimed = owner_guard.claimed;
                  std::cout << "displaymanager_owner2_claim[" << index << "]_status=0x"
                            << std::hex << std::setw(8) << std::setfill('0')
                            << static_cast<std::uint32_t>(owner_status) << std::dec
                            << " handle_kind="
                            << (use_source_presentation_handle ?
                                  "source_presentation" : "brokered_dwm_manager")
                            << " source_adapter=" << active_path.sourceInfo.adapterId.HighPart
                            << ':' << active_path.sourceInfo.adapterId.LowPart
                            << " source_id=" << source_id
                            << " use_nt_handles=1"
                            << " allow_output_duplication=0"
                            << " disable_dwm_virtual_mode=0\n";
                  if (owner_guard.claimed) {
                    break;
                  }
                }
              }
            }
          }
        }
      }
      const auto functionalize = state.TryFunctionalize(DisplayStateFunctionalizeOptions::None);
      std::cout << "displaymanager_functionalize_status="
                << static_cast<std::int32_t>(functionalize.Status())
                << " extended=0x" << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<std::uint32_t>(functionalize.ExtendedErrorCode().value)
                << std::dec << " path_count=" << path_count << '\n';
      path_count = 0;
      for (const auto &view: state.Views()) {
        for (const auto &path: view.Paths()) {
          std::cout << "displaymanager_path[" << path_count << "]_source_pixel_after="
                    << static_cast<std::int32_t>(path.SourcePixelFormat()) << '\n';
          const auto wire_after = path.WireFormat();
          if (wire_after) {
            std::cout << "displaymanager_path[" << path_count << "]_wire_after="
                      << "encoding:" << static_cast<std::int32_t>(wire_after.PixelEncoding())
                      << ",bpc:" << wire_after.BitsPerChannel()
                      << ",colorspace:" << static_cast<std::int32_t>(wire_after.ColorSpace())
                      << ",eotf:" << static_cast<std::int32_t>(wire_after.Eotf())
                      << ",metadata:" << static_cast<std::int32_t>(wire_after.HdrMetadata()) << '\n';
          } else {
            std::cout << "displaymanager_path[" << path_count << "]_wire_after=null\n";
          }
          ++path_count;
        }
      }
      if (owner_guard.claimed) {
        const auto release_status = D3DKMTReleaseProcessVidPnSourceOwners(GetCurrentProcess());
        owner_guard.claimed = false;
        std::cout << "displaymanager_owner2_release_status=0x"
                  << std::hex << std::setw(8) << std::setfill('0')
                  << static_cast<std::uint32_t>(release_status) << std::dec << '\n';
      }
      manager.Stop();
      return functionalize.Status() == DisplayStateOperationStatus::Success &&
                     (!claim_display_manager_owner || owner2_claimed) ?
        0 : 1;
    } catch (const winrt::hresult_error &error) {
      std::cerr << "displaymanager_exception=0x" << std::hex << std::setw(8)
                << std::setfill('0') << static_cast<std::uint32_t>(error.code().value)
                << std::dec << " message=" << winrt::to_string(error.message()) << '\n';
      return 1;
    }
#else
    std::cerr << "DisplayManager functionalize probe requires MSVC C++/WinRT\n";
    return 1;
#endif
  }

  int query_idd_current_mode_command(
    const LUID &adapter_luid,
    const D3DDDI_VIDEO_PRESENT_SOURCE_ID source_id
  ) {
    D3DKMT_OPENADAPTERFROMLUID open {};
    open.AdapterLuid = adapter_luid;
    const auto open_status = D3DKMTOpenAdapterFromLuid(&open);
    if (open_status < 0) {
      std::cerr << "D3DKMTOpenAdapterFromLuid failed status=0x"
                << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<std::uint32_t>(open_status) << std::dec << '\n';
      return 1;
    }

    const auto current = query_idd_current_mode(open.hAdapter, source_id);
    D3DKMT_CLOSEADAPTER close {open.hAdapter};
    (void) D3DKMTCloseAdapter(&close);
    std::cout << "d3dkmt_current_mode_status=0x"
              << std::hex << std::setw(8) << std::setfill('0')
              << static_cast<std::uint32_t>(current.status) << std::dec;
    if (current.status >= 0) {
      std::cout << " adapter_luid=" << adapter_luid.HighPart << ':' << adapter_luid.LowPart
                << " source_id=" << current.mode.VidPnSourceId
                << " mode=" << current.mode.DisplayMode.Width << 'x' << current.mode.DisplayMode.Height
                << " format=0x" << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<std::uint32_t>(current.mode.DisplayMode.Format) << std::dec
                << " refresh=" << current.mode.DisplayMode.RefreshRate.Numerator
                << '/' << current.mode.DisplayMode.RefreshRate.Denominator;
    }
    std::cout << '\n';
    return current.status < 0 ? 1 : 0;
  }

  int query_idd_mode_list_command(
    const LUID &adapter_luid,
    const D3DDDI_VIDEO_PRESENT_SOURCE_ID source_id
  ) {
    D3DKMT_OPENADAPTERFROMLUID open {};
    open.AdapterLuid = adapter_luid;
    const auto open_status = D3DKMTOpenAdapterFromLuid(&open);
    if (open_status < 0) {
      std::cerr << "D3DKMTOpenAdapterFromLuid failed status=0x"
                << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<std::uint32_t>(open_status) << std::dec << '\n';
      return 1;
    }

    D3DKMT_GETDISPLAYMODELIST query {};
    query.hAdapter = open.hAdapter;
    query.VidPnSourceId = source_id;
    const auto count_status = D3DKMTGetDisplayModeList(&query);
    std::vector<D3DKMT_DISPLAYMODE> modes(query.ModeCount);
    NTSTATUS list_status = count_status;
    if (!modes.empty()) {
      query.pModeList = modes.data();
      list_status = D3DKMTGetDisplayModeList(&query);
      modes.resize(std::min<std::size_t>(modes.size(), query.ModeCount));
    }

    D3DKMT_CLOSEADAPTER close {open.hAdapter};
    (void) D3DKMTCloseAdapter(&close);

    std::cout << "d3dkmt_mode_list_count_status=0x"
              << std::hex << std::setw(8) << std::setfill('0')
              << static_cast<std::uint32_t>(count_status)
              << " list_status=0x" << std::setw(8)
              << static_cast<std::uint32_t>(list_status) << std::dec
              << " adapter_luid=" << adapter_luid.HighPart << ':' << adapter_luid.LowPart
              << " source_id=" << source_id
              << " mode_count=" << query.ModeCount << '\n';
    if (list_status < 0) {
      return 1;
    }

    for (std::size_t index = 0; index < modes.size(); ++index) {
      const auto &mode = modes[index];
      std::uint64_t packed_flags {};
      static_assert(sizeof(packed_flags) == sizeof(mode.Flags));
      std::memcpy(&packed_flags, &mode.Flags, sizeof(packed_flags));
      std::cout << "d3dkmt_mode index=" << index
                << " size=" << mode.Width << 'x' << mode.Height
                << " format=0x" << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<std::uint32_t>(mode.Format) << std::dec
                << " integer_refresh=" << mode.IntegerRefreshRate
                << " refresh=" << mode.RefreshRate.Numerator << '/' << mode.RefreshRate.Denominator
                << " flags=0x" << std::hex << std::setw(8)
                << packed_flags << std::dec << '\n';
    }
    return 0;
  }

  int query_vidpn_ownership_current_session() {
    DWORD session_id {};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
      std::cerr << "ProcessIdToSessionId failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    const HWND query_window = GetDesktopWindow();
    RECT query_window_rect {};
    if (!query_window || !GetWindowRect(query_window, &query_window_rect)) {
      std::cerr << "GetDesktopWindow/GetWindowRect failed native_error=" << GetLastError() << '\n';
      return 1;
    }
    std::cout << "vidpn_owner_query_window="
              << reinterpret_cast<std::uintptr_t>(query_window)
              << " rect=" << query_window_rect.left << ',' << query_window_rect.top
              << '-' << query_window_rect.right << ',' << query_window_rect.bottom
              << " caller_session=" << session_id << '\n';

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
      std::cerr << "CreateToolhelp32Snapshot failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    std::size_t queried_count = 0;
    std::size_t matched_count = 0;
    if (Process32FirstW(snapshot, &entry)) {
      do {
        DWORD process_session_id {};
        if (!ProcessIdToSessionId(entry.th32ProcessID, &process_session_id)) {
          continue;
        }

        HANDLE process_handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process_handle) {
          continue;
        }

        D3DKMT_QUERYVIDPNEXCLUSIVEOWNERSHIP query {};
        query.hProcess = process_handle;
        query.hWindow = query_window;
        const auto status = D3DKMTQueryVidPnExclusiveOwnership(&query);
        ++queried_count;
        if (status >= 0 && query.OwnerType != D3DKMT_VIDPNSOURCEOWNER_UNOWNED) {
          ++matched_count;
          char image_name[MAX_PATH * 4] {};
          if (WideCharToMultiByte(
                CP_UTF8,
                0,
                entry.szExeFile,
                -1,
                image_name,
                static_cast<int>(std::size(image_name)),
                nullptr,
                nullptr
              ) <= 0) {
            std::strcpy(image_name, "<utf8-conversion-failed>");
          }
          std::cout << "vidpn_owner_match process=" << entry.th32ProcessID
                    << " process_session=" << process_session_id
                    << " image=" << image_name
                    << " status=0x" << std::hex << std::setw(8) << std::setfill('0')
                    << static_cast<std::uint32_t>(status) << std::dec
                    << " owner_type=" << static_cast<std::uint32_t>(query.OwnerType)
                    << " adapter_luid=" << query.AdapterLuid.HighPart << ':' << query.AdapterLuid.LowPart
                    << " source_id=" << query.VidPnSourceId;
          std::cout << '\n';
        }
        CloseHandle(process_handle);
      } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    std::cout << "vidpn_owner_query_summary queried=" << queried_count
              << " matched=" << matched_count << '\n';
    return queried_count != 0 ? 0 : 1;
  }

  int probe_idd_hdr_gate(const LUID &target_luid, const std::uint32_t target_id) {
    UINT32 query_flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    auto config = query_display_config(query_flags);
    if (!config) {
      query_flags = QDC_ONLY_ACTIVE_PATHS;
      config = query_display_config(query_flags);
    }
    if (!config) {
      std::cerr << "IDD HDR gate probe could not query active display configuration\n";
      return 1;
    }

    const auto path_it = std::find_if(
      config->paths.begin(),
      config->paths.end(),
      [&](const DISPLAYCONFIG_PATH_INFO &path) {
        return same_luid(path.targetInfo.adapterId, target_luid) &&
               path.targetInfo.id == target_id &&
               (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
      }
    );
    if (path_it == config->paths.end()) {
      std::cerr << "IDD HDR gate probe target is not an active path\n";
      return 1;
    }

    const bool virtual_mode_aware = (query_flags & QDC_VIRTUAL_MODE_AWARE) != 0;
    const auto mode_index = source_mode_index(*path_it, virtual_mode_aware);
    if (!mode_index || *mode_index >= config->modes.size()) {
      std::cerr << "IDD HDR gate probe active path has no source mode\n";
      return 1;
    }
    const auto &mode = config->modes[*mode_index];
    if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE ||
        !same_luid(mode.adapterId, path_it->sourceInfo.adapterId) ||
        mode.id != path_it->sourceInfo.id) {
      std::cerr << "IDD HDR gate probe source mode does not match the active path\n";
      return 1;
    }

    D3DKMT_OPENADAPTERFROMLUID open {};
    open.AdapterLuid = target_luid;
    const auto open_status = D3DKMTOpenAdapterFromLuid(&open);
    if (open_status < 0) {
      std::cerr << "D3DKMTOpenAdapterFromLuid failed status=0x"
                << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<std::uint32_t>(open_status) << std::dec << '\n';
      return 1;
    }
    const auto current_mode_before = query_idd_current_mode(open.hAdapter, path_it->sourceInfo.id);

    // IddCx 10.0.26100.4202 serializes one path as a 4-byte count followed by
    // one packed 0x84-byte record before issuing DXGK_IDD_ESCAPE_CODE 2. The
    // layout assertions above make every reverse-engineered wire offset a
    // compile-time contract instead of relying on the SDK's source struct ABI.
    std::array<std::byte, 4 + sizeof(IddHdrGatePathWire)> request {};
    constexpr std::uint32_t path_count = 1;
    constexpr std::uint32_t path_flags = 0x13;  // mode, scale, colorimetry, SDR white
    constexpr std::uint32_t monitor_color_mode_hdr10 = 3;
    constexpr std::uint32_t monitor_scale_factor = 100;
    constexpr std::uint32_t vsync_divider = 1;
    constexpr std::uint32_t colorimetry_flags = 0x06;  // BT.2020 RGB and ST.2084
    constexpr std::uint32_t sdr_white_level_nits = 80;
    constexpr std::uint32_t rgb_bpc_mask = 0x06;  // 8 and 10 bits per component
    constexpr std::uint32_t packed_wire_bpc = (rgb_bpc_mask & 0x3f) << 2;

    auto refresh = path_it->targetInfo.refreshRate;
    if (refresh.Numerator == 0 || refresh.Denominator == 0) {
      refresh = DISPLAYCONFIG_RATIONAL {60'000, 1'000};
    }
    const auto rotation = static_cast<std::uint32_t>(path_it->targetInfo.rotation);
    const auto position_x = mode.sourceMode.position.x;
    const auto position_y = mode.sourceMode.position.y;
    const auto width = mode.sourceMode.width;
    const auto height = mode.sourceMode.height;
    IddHdrGatePathWire wire_path {};
    wire_path.flags = path_flags;
    wire_path.target_luid = target_luid;
    wire_path.target_id = target_id;
    wire_path.position_x = position_x;
    wire_path.position_y = position_y;
    wire_path.width = width;
    wire_path.height = height;
    wire_path.rotation = rotation;
    wire_path.refresh_numerator = refresh.Numerator;
    wire_path.refresh_denominator = refresh.Denominator;
    wire_path.vsync_divider = vsync_divider;
    wire_path.monitor_color_mode = monitor_color_mode_hdr10;
    wire_path.monitor_scale_factor = monitor_scale_factor;
    wire_path.red_x = 725;
    wire_path.red_y = 299;
    wire_path.green_x = 174;
    wire_path.green_y = 816;
    wire_path.blue_x = 134;
    wire_path.blue_y = 47;
    wire_path.white_x = 320;
    wire_path.white_y = 337;
    wire_path.min_luminance = 50;
    wire_path.max_luminance = 10'000'000;
    wire_path.max_full_frame_luminance = 4'000'000;
    wire_path.packed_rgb_bits_per_component = packed_wire_bpc;
    wire_path.colorimetry_flags = colorimetry_flags;
    wire_path.sdr_white_level_nits = sdr_white_level_nits;
    std::memcpy(request.data(), &path_count, sizeof(path_count));
    std::memcpy(request.data() + sizeof(path_count), &wire_path, sizeof(wire_path));

    IddHdrGateSupportResult support {0, (std::numeric_limits<std::uint32_t>::max)()};
    IddHdrGateEscapeEnvelope envelope {
      open.hAdapter,
      2,
      0,
      static_cast<std::uint32_t>(request.size()),
      request.data(),
      static_cast<std::uint32_t>(sizeof(support)),
      0,
      &support,
      0,
      0
    };
    D3DKMT_ESCAPE escape {};
    escape.hAdapter = open.hAdapter;
    escape.Type = D3DKMT_ESCAPE_IDD_REQUEST;
    escape.pPrivateDriverData = &envelope;
    escape.PrivateDriverDataSize = sizeof(envelope);

    const auto transport_status = D3DKMTEscape(&escape);
    const auto current_mode_after = query_idd_current_mode(open.hAdapter, path_it->sourceInfo.id);
    D3DKMT_CLOSEADAPTER close {open.hAdapter};
    (void) D3DKMTCloseAdapter(&close);

    const auto print_status = [](const std::uint32_t value) {
      std::cout << "0x" << std::hex << std::setw(8) << std::setfill('0') << value << std::dec;
    };
    const auto print_current_mode = [&](
      const std::string_view label,
      const IddHdrCurrentModeResult &result
    ) {
      std::cout << "idd_hdr_gate_current_mode_" << label << "_status=";
      print_status(static_cast<std::uint32_t>(result.status));
      if (result.status >= 0) {
        std::cout << " source_id=" << result.mode.VidPnSourceId
                  << " mode=" << result.mode.DisplayMode.Width << 'x' << result.mode.DisplayMode.Height
                  << " format=";
        print_status(static_cast<std::uint32_t>(result.mode.DisplayMode.Format));
        std::cout << " refresh=" << result.mode.DisplayMode.RefreshRate.Numerator
                  << '/' << result.mode.DisplayMode.RefreshRate.Denominator;
      }
      std::cout << '\n';
    };
    std::cout << "idd_hdr_gate_target_luid=" << target_luid.HighPart << ':' << target_luid.LowPart
              << " target_id=" << target_id
              << " source_id=" << path_it->sourceInfo.id
              << " mode=" << width << 'x' << height
              << " refresh=" << refresh.Numerator << '/' << refresh.Denominator << '\n';
    std::cout << "idd_hdr_gate_transport_status=";
    print_status(static_cast<std::uint32_t>(transport_status));
    std::cout << " returned_output_bytes=" << envelope.returned_output_bytes;
    std::cout << " missing_support_mask=";
    print_status(support.missing_support_mask);
    std::cout << " path_index=" << support.path_index << '\n';
    print_current_mode("before", current_mode_before);
    print_current_mode("after", current_mode_after);

    constexpr std::array<std::pair<std::uint32_t, std::string_view>, 8> labels {{
      {0x01, "high_bpp_scanout"},
      {0x02, "target_high_color_space"},
      {0x04, "target_wide_color_space"},
      {0x08, "descriptor_st2084"},
      {0x10, "descriptor_bt2020"},
      {0x20, "descriptor_high_bpp"},
      {0x40, "os_wcg_support"},
      {0x80, "matrix_3x4"},
    }};
    bool first = true;
    std::cout << "idd_hdr_gate_missing=";
    for (const auto &[bit, label]: labels) {
      if ((support.missing_support_mask & bit) == 0) {
        continue;
      }
      if (!first) {
        std::cout << ',';
      }
      std::cout << label;
      first = false;
    }
    if (first) {
      std::cout << "none";
    }
    std::cout << '\n';
    return transport_status < 0 ? 1 : 0;
  }

  int apply_manifest_topology(vdd::ControlClient &client) {
    const auto manifest = client.query_display_manifest();
    if (!manifest.ok()) {
      return fail("query display manifest failed", manifest);
    }

    UINT32 query_flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    auto query = query_display_config_result(query_flags);
    if (!query.data) {
      query_flags = QDC_ONLY_ACTIVE_PATHS;
      query = query_display_config_result(query_flags);
    }
    if (!query.data) {
      std::cerr << "manifest topology query failed native_error=" << query.native_error << '\n';
      report_helper_event(
        EVENTLOG_ERROR_TYPE,
        kEventHelperTopologyFailed,
        "Manifest topology query failed native_error=" + std::to_string(query.native_error)
      );
      return 1;
    }

    auto paths = query.data->paths;
    auto modes = query.data->modes;
    const bool virtual_mode_aware = (query_flags & QDC_VIRTUAL_MODE_AWARE) != 0;
    bool save_to_database = false;
    std::uint32_t applied_profiles = 0;

    for (auto &path: paths) {
      if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
        continue;
      }

      const auto *profile = profile_for_target(manifest.value, path);
      if (!profile) {
        continue;
      }

      const auto mode_index = source_mode_index(path, virtual_mode_aware);
      if (!mode_index || *mode_index >= modes.size() || modes[*mode_index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        continue;
      }

      modes[*mode_index].sourceMode.position = POINTL {profile->position_x, profile->position_y};
      if (profile->layout_policy == vdd::kDisplayManifestLayoutPolicyApplyAndPersist) {
        save_to_database = true;
      }
      ++applied_profiles;
    }

    if (applied_profiles == 0) {
      std::cerr << "manifest topology had no active layout profiles\n";
      report_helper_event(EVENTLOG_ERROR_TYPE, kEventHelperTopologyFailed, L"Manifest topology had no active layout profiles");
      return 1;
    }

    const LONG result = SetDisplayConfig(
      static_cast<UINT32>(paths.size()),
      paths.data(),
      static_cast<UINT32>(modes.size()),
      modes.data(),
      SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_ALLOW_PATH_ORDER_CHANGES |
        (save_to_database ? SDC_SAVE_TO_DATABASE : 0) |
        (virtual_mode_aware ? SDC_VIRTUAL_MODE_AWARE : 0)
    );
    if (result != ERROR_SUCCESS) {
      std::cerr << "apply manifest topology failed native_error=" << result << '\n';
      report_helper_event(
        EVENTLOG_ERROR_TYPE,
        kEventHelperTopologyFailed,
        "Manifest topology apply failed native_error=" + std::to_string(result)
      );
      return 1;
    }

    report_helper_event(
      EVENTLOG_INFORMATION_TYPE,
      kEventHelperTopologyApplied,
      "Manifest topology applied profiles=" + std::to_string(applied_profiles)
    );
    std::cout << "manifest_topology_applied=1\n";
    std::cout << "manifest_topology_profiles=" << applied_profiles << '\n';
    std::cout << "manifest_topology_saved=" << (save_to_database ? 1 : 0) << '\n';
    return 0;
  }

  void prepare_legacy_topology_path(DISPLAYCONFIG_PATH_INFO &path, const bool active) {
    if (active) {
      path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
    } else {
      path.flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
    }
  }

  DISPLAYCONFIG_VIDEO_SIGNAL_INFO make_signal_info(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t refresh_hz
  );

  DISPLAYCONFIG_VIDEO_SIGNAL_INFO make_active_signal_info(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t refresh_hz
  );

  LONG activate_target_path_result(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_hz
  ) {
    if (!valid_display_mode_dimensions(width, height) || refresh_hz == 0) {
      std::cout << "activate_error=invalid_mode"
                << " width=" << width
                << " height=" << height
                << " refresh_hz=" << refresh_hz << '\n';
      return ERROR_INVALID_PARAMETER;
    }

    const auto luid = vdd::to_windows_luid(adapter_luid);

    UINT32 query_flags = QDC_ALL_PATHS | QDC_VIRTUAL_MODE_AWARE;
    auto query = query_display_config_result(query_flags);
    if (!query.data) {
      std::cout << "activate_query_error flags=" << query_flags
                << " native_error=" << query.native_error << '\n';
      query_flags = QDC_ALL_PATHS;
      query = query_display_config_result(query_flags);
    }
    if (!query.data) {
      std::cout << "activate_query_error flags=" << query_flags
                << " native_error=" << query.native_error << '\n';
      return ERROR_INVALID_PARAMETER;
    }
    auto &display_config = *query.data;
    const bool virtual_mode_aware = (query_flags & QDC_VIRTUAL_MODE_AWARE) != 0;

    std::vector<DISPLAYCONFIG_PATH_INFO> topology_paths;
    UINT32 clone_group_id = 0;
    for (auto path: display_config.paths) {
      if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
        continue;
      }
      if (same_luid(path.targetInfo.adapterId, luid) && path.targetInfo.id == target_id) {
        return ERROR_SUCCESS;
      }
      if (virtual_mode_aware) {
        prepare_virtual_topology_path(path, clone_group_id++, true);
      } else {
        prepare_legacy_topology_path(path, true);
      }
      topology_paths.push_back(path);
    }

    std::optional<DISPLAYCONFIG_PATH_INFO> target_path;
    for (auto path: display_config.paths) {
      if (!same_luid(path.targetInfo.adapterId, luid) ||
          path.targetInfo.id != target_id ||
          !path.targetInfo.targetAvailable) {
        continue;
      }

      path.targetInfo.targetAvailable = TRUE;
      if (virtual_mode_aware) {
        prepare_virtual_topology_path(path, clone_group_id, true);
      } else {
        prepare_legacy_topology_path(path, true);
      }
      target_path = path;
      break;
    }

    if (!target_path) {
      return ERROR_NOT_FOUND;
    }
    topology_paths.push_back(*target_path);

    auto requested_paths = topology_paths;
    auto requested_modes = display_config.modes;
    auto &requested_target = requested_paths.back();

    const auto source_mode_index = static_cast<UINT32>(requested_modes.size());
    DISPLAYCONFIG_MODE_INFO source_mode {};
    source_mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    source_mode.id = requested_target.sourceInfo.id;
    source_mode.adapterId = requested_target.sourceInfo.adapterId;
    source_mode.sourceMode.width = width;
    source_mode.sourceMode.height = height;
    source_mode.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
    source_mode.sourceMode.position = POINTL {0, 0};
    requested_modes.push_back(source_mode);

    const auto target_mode_index = static_cast<UINT32>(requested_modes.size());
    DISPLAYCONFIG_MODE_INFO target_mode {};
    target_mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
    target_mode.id = requested_target.targetInfo.id;
    target_mode.adapterId = requested_target.targetInfo.adapterId;
    target_mode.targetMode.targetVideoSignalInfo = make_active_signal_info(width, height, refresh_hz);
    requested_modes.push_back(target_mode);

    if (virtual_mode_aware) {
#if defined(__MINGW32__)
      requested_target.sourceInfo.sourceModeInfoIdx = source_mode_index;
      requested_target.targetInfo.targetModeInfoIdx = target_mode_index;
      requested_target.targetInfo.desktopModeInfoIdx = DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
#else
      const auto desktop_mode_index = static_cast<UINT32>(requested_modes.size());
      DISPLAYCONFIG_MODE_INFO desktop_mode {};
      desktop_mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE;
      desktop_mode.id = requested_target.sourceInfo.id;
      desktop_mode.adapterId = requested_target.sourceInfo.adapterId;
      desktop_mode.desktopImageInfo.PathSourceSize = POINTL {
        static_cast<LONG>(width),
        static_cast<LONG>(height)
      };
      desktop_mode.desktopImageInfo.DesktopImageRegion = RECTL {
        0,
        0,
        static_cast<LONG>(width),
        static_cast<LONG>(height)
      };
      desktop_mode.desktopImageInfo.DesktopImageClip = desktop_mode.desktopImageInfo.DesktopImageRegion;
      requested_modes.push_back(desktop_mode);

      requested_target.sourceInfo.sourceModeInfoIdx = source_mode_index;
      requested_target.targetInfo.targetModeInfoIdx = target_mode_index;
      requested_target.targetInfo.desktopModeInfoIdx = desktop_mode_index;
#endif
    } else {
      requested_target.sourceInfo.modeInfoIdx = source_mode_index;
      requested_target.targetInfo.modeInfoIdx = target_mode_index;
    }

    LONG result = SetDisplayConfig(
      static_cast<UINT32>(requested_paths.size()),
      requested_paths.data(),
      static_cast<UINT32>(requested_modes.size()),
      requested_modes.data(),
      SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_ALLOW_PATH_ORDER_CHANGES |
        (virtual_mode_aware ? SDC_VIRTUAL_MODE_AWARE : 0)
    );
    std::cout << "activate_supplied_result=" << result
              << " virtual_aware=" << (virtual_mode_aware ? 1 : 0)
              << " paths=" << requested_paths.size()
              << " modes=" << requested_modes.size() << '\n';
    if (result == ERROR_SUCCESS) {
      return result;
    }

    if (result == ERROR_GEN_FAILURE || result == ERROR_INVALID_PARAMETER) {
      auto fallback_query = query_display_config_result(query_flags);
      if (!fallback_query.data) {
        std::cout << "activate_topology_fallback_query_error flags=" << query_flags
                  << " native_error=" << fallback_query.native_error << '\n';
        return fallback_query.native_error;
      }

      auto full_paths = std::move(fallback_query.data->paths);
      clone_group_id = 0;
      bool found_target = false;
      for (auto &path: full_paths) {
        const bool already_active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        const bool is_target =
          same_luid(path.targetInfo.adapterId, luid) &&
          path.targetInfo.id == target_id &&
          path.sourceInfo.id == target_path->sourceInfo.id;
        if (virtual_mode_aware) {
          prepare_virtual_topology_path(path, already_active || is_target ? clone_group_id++ : 0, already_active || is_target);
        } else {
          prepare_legacy_topology_path(path, already_active || is_target);
        }
        if (is_target) {
          found_target = true;
          path.targetInfo.targetAvailable = TRUE;
        }
      }
      if (!found_target) {
        std::cout << "activate_topology_fallback_error=target_not_found\n";
        return ERROR_NOT_FOUND;
      }

      result = SetDisplayConfig(
        static_cast<UINT32>(full_paths.size()),
        full_paths.data(),
        0,
        nullptr,
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES |
          (virtual_mode_aware ? SDC_VIRTUAL_MODE_AWARE : 0)
      );
      std::cout << "activate_topology_fallback_result=" << result
                << " virtual_aware=" << (virtual_mode_aware ? 1 : 0)
                << " paths=" << full_paths.size() << '\n';
      if (result == ERROR_SUCCESS) {
        return result;
      }
    }

    return result;
  }

  bool activate_target_path(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_hz
  ) {
    return activate_target_path_result(adapter_luid, target_id, width, height, refresh_hz) == ERROR_SUCCESS;
  }

  void dump_display_config_paths(
    const std::optional<vdd::AdapterLuid> &adapter_luid = std::nullopt,
    const std::optional<std::uint32_t> &target_id = std::nullopt
  ) {
    UINT32 query_flags = QDC_ALL_PATHS | QDC_VIRTUAL_MODE_AWARE;
    auto query = query_display_config_result(query_flags);
    if (!query.data) {
      query_flags = QDC_ALL_PATHS;
      query = query_display_config_result(query_flags);
    }
    if (!query.data) {
      query_flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
      query = query_display_config_result(query_flags);
    }
    if (!query.data) {
      query_flags = QDC_ONLY_ACTIVE_PATHS;
      query = query_display_config_result(query_flags);
    }
    if (!query.data) {
      std::cout << "display_config_query_error=1 native_error=" << query.native_error << '\n';
      return;
    }
    auto &display_config = *query.data;

    const auto filter_luid = adapter_luid ? vdd::to_windows_luid(*adapter_luid) : LUID {};
    std::cout << "display_config_query_flags=" << query_flags
              << " display_config_paths=" << display_config.paths.size()
              << " modes=" << display_config.modes.size() << '\n';
    const bool virtual_mode_aware = (query_flags & QDC_VIRTUAL_MODE_AWARE) != 0;
    for (std::size_t index = 0; index < display_config.paths.size(); ++index) {
      const auto &path = display_config.paths[index];
      const bool matches_filter =
        !adapter_luid ||
        same_luid(path.targetInfo.adapterId, filter_luid) ||
        (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
      const bool matches_target = !target_id || path.targetInfo.id == *target_id;
      if (!matches_filter || (!matches_target && (path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0)) {
        continue;
      }

      std::cout << "path[" << index << "]"
                << " active=" << ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) ? 1 : 0)
                << " source_luid=" << path.sourceInfo.adapterId.HighPart << ':' << path.sourceInfo.adapterId.LowPart
                << " source_id=" << path.sourceInfo.id
                << " source_mode_idx=" << path.sourceInfo.sourceModeInfoIdx
                << " clone_group=" << path.sourceInfo.cloneGroupId
                << " target_luid=" << path.targetInfo.adapterId.HighPart << ':' << path.targetInfo.adapterId.LowPart
                << " target_id=" << path.targetInfo.id
                << " target_mode_idx=" << path.targetInfo.targetModeInfoIdx
                << " desktop_idx=" << path.targetInfo.desktopModeInfoIdx
                << " available=" << (path.targetInfo.targetAvailable ? 1 : 0)
                << " tech=" << static_cast<unsigned int>(path.targetInfo.outputTechnology)
                << " status=" << static_cast<unsigned int>(path.targetInfo.statusFlags)
                << '\n';

      const auto source_index = virtual_mode_aware ?
        path.sourceInfo.sourceModeInfoIdx :
        path.sourceInfo.modeInfoIdx;
      if (source_index != DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID &&
          source_index < display_config.modes.size()) {
        const auto &mode = display_config.modes[source_index];
        if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
          std::cout << "path_source_mode[" << index << "]"
                    << " width=" << mode.sourceMode.width
                    << " height=" << mode.sourceMode.height
                    << " pixel_format=" << static_cast<unsigned int>(mode.sourceMode.pixelFormat)
                    << " position=" << mode.sourceMode.position.x << ',' << mode.sourceMode.position.y
                    << '\n';
        }
      }

      const auto target_index = virtual_mode_aware ?
        path.targetInfo.targetModeInfoIdx :
        path.targetInfo.modeInfoIdx;
      if (target_index != DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID &&
          target_index < display_config.modes.size()) {
        const auto &mode = display_config.modes[target_index];
        if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
          const auto &signal = mode.targetMode.targetVideoSignalInfo;
          std::cout << "path_target_mode[" << index << "]"
                    << " active=" << signal.activeSize.cx << 'x' << signal.activeSize.cy
                    << " total=" << signal.totalSize.cx << 'x' << signal.totalSize.cy
                    << " vsync=" << signal.vSyncFreq.Numerator << '/' << signal.vSyncFreq.Denominator
                    << '\n';
        }
      }
    }
  }

  void dump_active_paths_for_adapter(const vdd::AdapterLuid &adapter_luid) {
    const auto luid = vdd::to_windows_luid(adapter_luid);
    constexpr std::array<UINT32, 2> kQueryFlags {
      QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE,
      QDC_ONLY_ACTIVE_PATHS
    };

    for (const auto flags: kQueryFlags) {
      auto display_config = query_display_config(flags);
      if (!display_config) {
        std::cout << "active_paths_query_error flags=" << flags << '\n';
        continue;
      }

      std::cout << "active_paths flags=" << flags
                << " count=" << display_config->paths.size()
                << " modes=" << display_config->modes.size() << '\n';
      for (std::size_t index = 0; index < display_config->paths.size(); ++index) {
        const auto &path = display_config->paths[index];
        if (!same_luid(path.targetInfo.adapterId, luid)) {
          continue;
        }

        std::cout << "active_path[" << index << "]"
                  << " source_id=" << path.sourceInfo.id
                  << " source_mode_idx=" << (flags & QDC_VIRTUAL_MODE_AWARE ? path.sourceInfo.sourceModeInfoIdx : path.sourceInfo.modeInfoIdx)
                  << " clone_group=" << path.sourceInfo.cloneGroupId
                  << " target_id=" << path.targetInfo.id
                  << " target_mode_idx=" << (flags & QDC_VIRTUAL_MODE_AWARE ? path.targetInfo.targetModeInfoIdx : path.targetInfo.modeInfoIdx)
                  << " refresh=" << path.targetInfo.refreshRate.Numerator
                  << '/' << path.targetInfo.refreshRate.Denominator
                  << " flags=" << path.flags
                  << " status=" << path.targetInfo.statusFlags << '\n';
      }
    }
  }

  std::optional<AdvancedColorInfo> wait_for_advanced_color(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const bool require_hdr_enabled,
    LONG *native_error = nullptr,
    const std::function<void()> &keep_alive = {}
  ) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::optional<AdvancedColorInfo> latest;
    do {
      latest = query_advanced_color(adapter_luid, target_id, native_error);
      if (latest) {
        const bool hdr_ready =
          latest->v2 &&
          latest->supported &&
          latest->active &&
          latest->hdr_supported &&
          latest->hdr_enabled &&
          latest->active_color_mode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR &&
          !latest->limited_by_policy &&
          latest->bits_per_color_channel >= 10;
        if (!require_hdr_enabled || hdr_ready) {
          return latest;
        }
      }
      if (keep_alive) {
        keep_alive();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);

    return latest;
  }

  void print_advanced_color(const AdvancedColorInfo &info) {
    std::cout << "advanced_color_v2=" << (info.v2 ? 1 : 0)
              << " supported=" << (info.supported ? 1 : 0)
              << " active=" << (info.active ? 1 : 0)
              << " limited_by_policy=" << (info.limited_by_policy ? 1 : 0)
              << " hdr_supported=" << (info.hdr_supported ? 1 : 0)
              << " hdr_enabled=" << (info.hdr_enabled ? 1 : 0)
              << " bits_per_color_channel=" << info.bits_per_color_channel
              << " active_color_mode=" << info.active_color_mode
              << " color_encoding=" << static_cast<unsigned int>(info.color_encoding)
              << '\n';
  }

  void query_dmm_diagnostics(const LUID &adapter_luid) {
    D3DKMT_OPENADAPTERFROMLUID open {};
    open.AdapterLuid = adapter_luid;
    const auto open_status = D3DKMTOpenAdapterFromLuid(&open);
    std::cout << "dmm_diagnostics_open_status=0x"
              << std::hex << std::setw(8) << std::setfill('0')
              << static_cast<std::uint32_t>(open_status) << std::dec << '\n';
    if (open_status < 0) {
      return;
    }

    constexpr std::array types {
      D3DKMT_DMMESCAPETYPE_GET_SUMMARY_INFO,
      D3DKMT_DMMESCAPETYPE_GET_VIDEO_PRESENT_SOURCES_INFO,
      D3DKMT_DMMESCAPETYPE_GET_ACTIVEVIDPN_INFO,
      D3DKMT_DMMESCAPETYPE_ACTIVEVIDPN_SOURCEMODESET_INFO,
      D3DKMT_DMMESCAPETYPE_ACTIVEVIDPN_COFUNCPATHMODALITY_INFO
    };
    constexpr std::size_t data_size = D3DKMT_MAX_DMM_ESCAPE_DATASIZE;
    constexpr std::size_t header_size = offsetof(D3DKMT_DMM_ESCAPE, Data);
    for (const auto type: types) {
      std::vector<std::uint8_t> storage(header_size + data_size);
      auto *dmm = reinterpret_cast<D3DKMT_DMM_ESCAPE *>(storage.data());
      dmm->Type = type;
      dmm->ProvidedBufferSize = data_size;

      D3DKMT_ESCAPE escape {};
      escape.hAdapter = open.hAdapter;
      escape.Type = D3DKMT_ESCAPE_DMM;
      escape.pPrivateDriverData = dmm;
      escape.PrivateDriverDataSize = static_cast<UINT>(storage.size());
      const auto status = D3DKMTEscape(&escape);
      const auto returned_size = std::min<std::size_t>(dmm->MinRequiredBufferSize, data_size);
      std::cout << "dmm_diagnostics_type=" << static_cast<unsigned int>(type)
                << " status=0x" << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<std::uint32_t>(status) << std::dec
                << " required=" << dmm->MinRequiredBufferSize
                << " returned=" << returned_size << '\n';
      if (status < 0 || returned_size == 0) {
        continue;
      }

      for (std::size_t offset = 0; offset < returned_size; offset += 16) {
        std::cout << "dmm_data type=" << static_cast<unsigned int>(type)
                  << " offset=0x" << std::hex << std::setw(8) << std::setfill('0')
                  << offset << " bytes=";
        const auto line_size = std::min<std::size_t>(16, returned_size - offset);
        for (std::size_t index = 0; index < line_size; ++index) {
          std::cout << std::setw(2) << static_cast<unsigned int>(dmm->Data[offset + index]);
        }
        std::cout << std::dec << '\n';
      }
    }

    D3DKMT_CLOSEADAPTER close {open.hAdapter};
    const auto close_status = D3DKMTCloseAdapter(&close);
    std::cout << "dmm_diagnostics_close_status=0x"
              << std::hex << std::setw(8) << std::setfill('0')
              << static_cast<std::uint32_t>(close_status) << std::dec << '\n';
  }

  int probe_idd_hdr_functionalize_current_session(
    const bool claim_shared_owner = false,
    const bool prefer_nongdi_source = false,
    const bool preserve_current_impersonation = false
  ) {
    UINT32 query_flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    auto config = query_display_config(query_flags);
    if (!config) {
      query_flags = QDC_ONLY_ACTIVE_PATHS;
      config = query_display_config(query_flags);
    }
    if (!config || config->paths.size() != 1 ||
        (config->paths.front().flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
      std::cerr << "functionalize proof requires exactly one active display path\n";
      return 1;
    }

    const auto &path = config->paths.front();
    const auto target_luid = path.targetInfo.adapterId;
    const auto target_id = path.targetInfo.id;
    const auto source_id = path.sourceInfo.id;
    const auto adapter_luid = vdd::from_windows_luid(target_luid);
    char user_name[256] {};
    DWORD user_name_size = static_cast<DWORD>(std::size(user_name));
    const bool have_user_name = GetUserNameA(user_name, &user_name_size) != FALSE;
    DWORD session_id {};
    const bool have_session_id = ProcessIdToSessionId(GetCurrentProcessId(), &session_id) != FALSE;
    std::cout << "functionalize_session_id=" << (have_session_id ? session_id : (std::numeric_limits<DWORD>::max)())
              << " identity=" << (have_user_name ? user_name : "unknown")
              << " target_luid=" << target_luid.HighPart << ':' << target_luid.LowPart
              << " target_id=" << target_id << " source_id=" << source_id << '\n';

    LONG native_error = ERROR_SUCCESS;
    const auto advanced_before = query_advanced_color(adapter_luid, target_id, &native_error);
    std::cout << "advanced_before_native_error=" << native_error << '\n';
    if (advanced_before) {
      std::cout << "before_";
      print_advanced_color(*advanced_before);
    }
    (void) query_idd_current_mode_command(target_luid, source_id);
    std::cout << "functionalize_mode_list_before\n";
    (void) query_idd_mode_list_command(target_luid, source_id);
    std::cout << "functionalize_vidpn_ownership_before\n";
    (void) query_vidpn_ownership_current_session();
    if (claim_shared_owner) {
      query_dmm_diagnostics(target_luid);
    }

    LONG nongdi_source_result = ERROR_SUCCESS;
    if (prefer_nongdi_source) {
      ScopedConsoleUserImpersonation nongdi_impersonation;
      DWORD nongdi_impersonation_error = ERROR_SUCCESS;
      const bool nongdi_impersonating = have_session_id &&
        nongdi_impersonation.begin(session_id, &nongdi_impersonation_error);
      char nongdi_user_name[256] {};
      DWORD nongdi_user_name_size = static_cast<DWORD>(std::size(nongdi_user_name));
      const bool have_nongdi_user_name = nongdi_impersonating &&
        GetUserNameA(nongdi_user_name, &nongdi_user_name_size) != FALSE;
      std::cout << "functionalize_nongdi_console_user_impersonation="
                << static_cast<int>(nongdi_impersonating)
                << " source_session=" << nongdi_impersonation.source_session_id()
                << " target_session=" << nongdi_impersonation.target_session_id()
                << " identity=" << (have_nongdi_user_name ? nongdi_user_name : "unknown")
                << " native_error=" << nongdi_impersonation_error << '\n';
      nongdi_source_result = nongdi_impersonating ?
        apply_nongdi_source_topology_result() :
        static_cast<LONG>(nongdi_impersonation_error == ERROR_SUCCESS ?
          ERROR_ACCESS_DENIED : nongdi_impersonation_error);
      DWORD nongdi_revert_error = ERROR_SUCCESS;
      const bool nongdi_reverted = nongdi_impersonation.revert(&nongdi_revert_error);
      std::cout << "functionalize_nongdi_console_user_revert="
                << static_cast<int>(nongdi_reverted)
                << " native_error=" << nongdi_revert_error << '\n';
      std::cout << "functionalize_nongdi_source_result=" << nongdi_source_result << '\n';
      (void) query_idd_current_mode_command(target_luid, source_id);
    }

    D3DKMT_OPENADAPTERFROMLUID owner_adapter {};
    D3DKMT_CREATEDEVICE owner_device {};
    bool owner_adapter_open = false;
    bool owner_device_created = false;
    bool owner_claimed = false;
    if (claim_shared_owner) {
      owner_adapter.AdapterLuid = target_luid;
      const auto open_status = D3DKMTOpenAdapterFromLuid(&owner_adapter);
      std::cout << "functionalize_shared_owner_open_status=0x"
                << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<std::uint32_t>(open_status) << std::dec << '\n';
      owner_adapter_open = open_status >= 0;
      if (owner_adapter_open) {
        D3DKMT_CHECKVIDPNEXCLUSIVEOWNERSHIP ownership_check {};
        ownership_check.hAdapter = owner_adapter.hAdapter;
        ownership_check.VidPnSourceId = source_id;
        const auto ownership_status = D3DKMTCheckVidPnExclusiveOwnership(&ownership_check);
        std::cout << "functionalize_shared_owner_check_status=0x"
                  << std::hex << std::setw(8) << std::setfill('0')
                  << static_cast<std::uint32_t>(ownership_status) << std::dec
                  << " any_exclusive=" << static_cast<unsigned int>(D3DKMTCheckExclusiveOwnership()) << '\n';

        owner_device.hAdapter = owner_adapter.hAdapter;
        const auto create_status = D3DKMTCreateDevice(&owner_device);
        std::cout << "functionalize_shared_owner_create_status=0x"
                  << std::hex << std::setw(8) << std::setfill('0')
                  << static_cast<std::uint32_t>(create_status) << std::dec << '\n';
        owner_device_created = create_status >= 0;
      }
      if (owner_device_created) {
        const D3DKMT_VIDPNSOURCEOWNER_TYPE owner_type = D3DKMT_VIDPNSOURCEOWNER_SHARED;
        const D3DDDI_VIDEO_PRESENT_SOURCE_ID owner_source_id = source_id;
        D3DKMT_SETVIDPNSOURCEOWNER owner_request {};
        owner_request.hDevice = owner_device.hDevice;
        owner_request.pType = &owner_type;
        owner_request.pVidPnSourceId = &owner_source_id;
        owner_request.VidPnSourceCount = 1;
        const auto owner_status = D3DKMTSetVidPnSourceOwner(&owner_request);
        std::cout << "functionalize_shared_owner_claim_status=0x"
                  << std::hex << std::setw(8) << std::setfill('0')
                  << static_cast<std::uint32_t>(owner_status) << std::dec << '\n';
        owner_claimed = owner_status >= 0;
      }
    }

    const bool experiment_ready = (!claim_shared_owner || owner_claimed) &&
      (!prefer_nongdi_source || nongdi_source_result == ERROR_SUCCESS);
    const int gate_exit = experiment_ready ?
      probe_idd_hdr_gate(target_luid, target_id) : 1;
    std::cout << "functionalize_gate_exit=" << gate_exit << '\n';

    ScopedConsoleUserImpersonation impersonation;
    DWORD impersonation_error = ERROR_SUCCESS;
    const bool impersonating = experiment_ready &&
      (preserve_current_impersonation ||
       (have_session_id && impersonation.begin(session_id, &impersonation_error)));
    char impersonated_user_name[256] {};
    DWORD impersonated_user_name_size = static_cast<DWORD>(std::size(impersonated_user_name));
    const bool have_impersonated_user_name = impersonating &&
      GetUserNameA(impersonated_user_name, &impersonated_user_name_size) != FALSE;
    std::cout << "functionalize_console_user_impersonation=" << static_cast<int>(impersonating)
              << " source_session=" << impersonation.source_session_id()
              << " target_session=" << impersonation.target_session_id()
              << " identity=" << (have_impersonated_user_name ? impersonated_user_name : "unknown")
              << " native_error=" << impersonation_error << '\n';

    LONG set_error = ERROR_SUCCESS;
    bool set_requested = experiment_ready && advanced_before && advanced_before->v2 ?
      set_hdr_state(adapter_luid, target_id, true, &set_error) :
      experiment_ready && set_advanced_color(adapter_luid, target_id, true, &set_error);
    std::cout << "functionalize_hdr_set_v2="
              << static_cast<int>(advanced_before && advanced_before->v2)
              << " requested=" << static_cast<int>(set_requested)
              << " native_error=" << set_error << '\n';
    if (experiment_ready &&
        !set_requested && advanced_before && advanced_before->v2) {
      set_error = ERROR_SUCCESS;
      set_requested = set_advanced_color(adapter_luid, target_id, true, &set_error);
      std::cout << "functionalize_hdr_set_legacy requested=" << static_cast<int>(set_requested)
                << " native_error=" << set_error << '\n';
    }

    const LONG topology_result = experiment_ready ?
      apply_extended_topology_result() : ERROR_INVALID_STATE;
    std::cout << "functionalize_topology_result=" << topology_result << '\n';
    DWORD revert_error = ERROR_SUCCESS;
    const bool reverted = preserve_current_impersonation ?
      true : impersonation.revert(&revert_error);
    std::cout << "functionalize_console_user_revert=" << static_cast<int>(reverted)
              << " native_error=" << revert_error << '\n';
    Sleep(2000);

    native_error = ERROR_SUCCESS;
    const auto advanced_after = query_advanced_color(adapter_luid, target_id, &native_error);
    std::cout << "advanced_after_native_error=" << native_error << '\n';
    if (advanced_after) {
      std::cout << "after_";
      print_advanced_color(*advanced_after);
    }
    const int current_mode_exit = query_idd_current_mode_command(target_luid, source_id);
    std::cout << "functionalize_mode_list_after\n";
    (void) query_idd_mode_list_command(target_luid, source_id);
    std::cout << "functionalize_vidpn_ownership_after\n";
    (void) query_vidpn_ownership_current_session();
    dump_display_config_paths();

    NTSTATUS release_status {};
    NTSTATUS destroy_status {};
    NTSTATUS close_status {};
    if (owner_claimed) {
      release_status = D3DKMTReleaseProcessVidPnSourceOwners(GetCurrentProcess());
      std::cout << "functionalize_shared_owner_release_status=0x"
                << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<std::uint32_t>(release_status) << std::dec << '\n';
    }
    if (owner_device_created) {
      D3DKMT_DESTROYDEVICE destroy {owner_device.hDevice};
      destroy_status = D3DKMTDestroyDevice(&destroy);
      std::cout << "functionalize_shared_owner_destroy_status=0x"
                << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<std::uint32_t>(destroy_status) << std::dec << '\n';
    }
    if (owner_adapter_open) {
      D3DKMT_CLOSEADAPTER close {owner_adapter.hAdapter};
      close_status = D3DKMTCloseAdapter(&close);
      std::cout << "functionalize_shared_owner_close_status=0x"
                << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<std::uint32_t>(close_status) << std::dec << '\n';
    }

    const bool owner_cleanup_ok = !claim_shared_owner ||
      (owner_claimed && release_status >= 0 && destroy_status >= 0 && close_status >= 0);
    return experiment_ready && gate_exit == 0 && advanced_after &&
      current_mode_exit == 0 && owner_cleanup_ok ? 0 : 1;
  }

  bool advanced_color_matches(const AdvancedColorInfo &info, const bool enabled) {
    if (!enabled) {
      return !info.active ||
             (info.v2 && !info.hdr_enabled &&
              info.active_color_mode != DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR);
    }

    return info.v2 &&
           info.supported &&
           info.active &&
           info.hdr_supported &&
           info.hdr_enabled &&
           info.active_color_mode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR &&
           !info.limited_by_policy &&
           info.bits_per_color_channel >= 10;
  }

  int set_current_session_hdr_state(const bool enabled) {
    UINT32 query_flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    auto query = query_display_config_result(query_flags);
    if (!query.data) {
      query_flags = QDC_ONLY_ACTIVE_PATHS;
      query = query_display_config_result(query_flags);
    }
    if (!query.data) {
      std::cerr << "current-session HDR path query failed native_error=" << query.native_error << '\n';
      return 1;
    }

    std::uint32_t attempted = 0;
    std::uint32_t matched = 0;
    for (const auto &path: query.data->paths) {
      if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0 || !path.targetInfo.targetAvailable) {
        continue;
      }

      const auto adapter_luid = vdd::from_windows_luid(path.targetInfo.adapterId);
      LONG native_error = ERROR_SUCCESS;
      const auto before = query_advanced_color(adapter_luid, path.targetInfo.id, &native_error);
      std::cout << "current_session_hdr_target=" << attempted
                << " adapter_luid=" << path.targetInfo.adapterId.HighPart << ':' << path.targetInfo.adapterId.LowPart
                << " target_id=" << path.targetInfo.id
                << " source_id=" << path.sourceInfo.id << '\n';
      std::cout << "current_session_hdr_mode_before\n";
      (void) query_idd_current_mode_command(path.targetInfo.adapterId, path.sourceInfo.id);
      if (!before) {
        std::cerr << "current-session advanced-color query failed"
                  << " target_id=" << path.targetInfo.id
                  << " native_error=" << native_error << '\n';
        ++attempted;
        continue;
      }

      std::cout << "before_";
      print_advanced_color(*before);
      LONG set_error = ERROR_SUCCESS;
      bool set = before->v2 ?
        set_hdr_state(adapter_luid, path.targetInfo.id, enabled, &set_error) :
        set_advanced_color(adapter_luid, path.targetInfo.id, enabled, &set_error);
      if (!set) {
        if (before->v2) {
          std::cout << "current_session_hdr_legacy_fallback=1"
                    << " target_id=" << path.targetInfo.id
                    << " v2_native_error=" << set_error << '\n';
          set = set_advanced_color(adapter_luid, path.targetInfo.id, enabled, &set_error);
        }
        if (!set) {
          std::cerr << "current-session HDR state change failed"
                    << " target_id=" << path.targetInfo.id
                    << " native_error=" << set_error << '\n';
          ++attempted;
          continue;
        }
      }

      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      std::optional<AdvancedColorInfo> after;
      do {
        after = query_advanced_color(adapter_luid, path.targetInfo.id, &native_error);
        if (after && advanced_color_matches(*after, enabled)) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      } while (std::chrono::steady_clock::now() < deadline);

      if ((!after || !advanced_color_matches(*after, enabled)) && before->v2) {
        std::cout << "current_session_hdr_legacy_fallback=1"
                  << " target_id=" << path.targetInfo.id << '\n';
        if (set_advanced_color(adapter_luid, path.targetInfo.id, enabled)) {
          const auto fallback_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
          do {
            after = query_advanced_color(adapter_luid, path.targetInfo.id, &native_error);
            if (after && advanced_color_matches(*after, enabled)) {
              break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          } while (std::chrono::steady_clock::now() < fallback_deadline);
        } else {
          std::cerr << "current-session legacy advanced-color state change failed"
                    << " target_id=" << path.targetInfo.id << '\n';
        }
      }

      if (after) {
        std::cout << "after_";
        print_advanced_color(*after);
      }
      std::cout << "current_session_hdr_mode_after\n";
      (void) query_idd_current_mode_command(path.targetInfo.adapterId, path.sourceInfo.id);
      if (after && advanced_color_matches(*after, enabled)) {
        ++matched;
      } else {
        std::cerr << "current-session HDR state did not converge"
                  << " target_id=" << path.targetInfo.id
                  << " requested=" << (enabled ? 1 : 0)
                  << " native_error=" << native_error << '\n';
      }
      ++attempted;
    }

    std::cout << "current_session_hdr_requested=" << (enabled ? 1 : 0)
              << " attempted=" << attempted
              << " matched=" << matched << '\n';
    return attempted != 0 && matched == attempted ? 0 : 1;
  }

  int probe_wcg_prime_hdr_native_user(
    const DWORD expected_session_id,
    const std::uint64_t display_id,
    const std::string_view output_root
  ) {
    DWORD process_session_id = 0xffffffffu;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &process_session_id) ||
        process_session_id != expected_session_id) {
      return 2;
    }

    const std::string root {output_root};
    const std::string functionalize_path = root + "\\functionalize-native.txt";
    const std::string transition_path = root + "\\wcg-prime-transition.txt";
    const std::string final_path = root + "\\wcg-prime-final-state.txt";

    const auto run_as_current_user = [&](const std::string &path, const auto &operation) {
      std::ofstream output {path, std::ios::out | std::ios::trunc};
      if (!output) {
        return 2;
      }
      auto *old_out = std::cout.rdbuf(output.rdbuf());
      auto *old_err = std::cerr.rdbuf(output.rdbuf());
      char user_name[256] {};
      DWORD user_name_size = static_cast<DWORD>(std::size(user_name));
      const bool have_user_name = GetUserNameA(user_name, &user_name_size) != FALSE;
      std::cout << "native_user_process_session=" << process_session_id
                << " identity=" << (have_user_name ? user_name : "unknown") << '\n';
      const int result = operation();
      std::cout << "native_user_operation_last_error=" << GetLastError() << '\n';
      std::cout.flush();
      std::cerr.flush();
      std::cout.rdbuf(old_out);
      std::cerr.rdbuf(old_err);
      return result;
    };

    const int functionalize_result = run_as_current_user(functionalize_path, [&]() {
      return probe_idd_hdr_functionalize_current_session(false, false, true);
    });
    if (functionalize_result != 0) {
      return functionalize_result;
    }

    std::ofstream transition {transition_path, std::ios::out | std::ios::trunc};
    auto opened = vdd::open_remote_control_device_for_session(expected_session_id);
    if (!opened.ok()) {
      transition << "remote_open_status=" << static_cast<std::uint32_t>(opened.status)
                 << " native_error=" << opened.native_error << '\n';
      return 3;
    }
    vdd::ControlClient client {*opened.transport};
    const auto protocol = client.query_protocol_version();
    if (!protocol.ok()) {
      transition << "protocol_status=" << static_cast<std::uint32_t>(protocol.status)
                 << " native_error=" << protocol.native_error << '\n';
      return 3;
    }
    vdd::SetDisplayHdrStateRequest request {};
    request.display_id = display_id;
    request.enabled = 1;
    request.sdr_white_level_nits = vdd::kDefaultSdrWhiteLevelNits;
    const auto transition_result = client.set_display_hdr_state(request);
    transition << "remote_session=" << expected_session_id
               << " display_id=" << display_id
               << " status=" << static_cast<std::uint32_t>(transition_result.status)
               << " native_error=" << transition_result.native_error << '\n';
    if (!transition_result.ok()) {
      return 3;
    }

    Sleep(2000);
    return run_as_current_user(final_path, [&]() {
      return set_current_session_hdr_state(true);
    });
  }

  int remote_current_session_wcg_to_hdr(
    const std::uint64_t display_id,
    const std::string_view output_path
  ) {
    std::ofstream output {std::string {output_path}, std::ios::out | std::ios::trunc};
    if (!output) {
      return 2;
    }

    DWORD session_id = 0xffffffffu;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id) || session_id == 0xffffffffu) {
      output << "process_session_failed native_error=" << GetLastError() << '\n';
      return 2;
    }
    output << "process_session=" << session_id
           << " display_id=" << display_id << '\n';

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::uint32_t last_status = static_cast<std::uint32_t>(vdd::ControlStatus::TransportFailed);
    std::uint32_t last_error = ERROR_FILE_NOT_FOUND;
    do {
      auto opened = vdd::open_remote_control_device_for_session(session_id);
      last_status = static_cast<std::uint32_t>(opened.status);
      last_error = opened.native_error;
      if (opened.ok()) {
        vdd::ControlClient client {*opened.transport};
        const auto state = client.query_display_state();
        last_status = static_cast<std::uint32_t>(state.status);
        last_error = state.native_error;
        if (state.ok()) {
          bool found = false;
          const auto entry_count = (std::min)(state.value.entry_count, vdd::kMaxDisplayStateEntries);
          for (std::uint32_t index = 0; index < entry_count; ++index) {
            const auto &entry = state.value.entries[index];
            output << "before_entry=" << index
                   << " display_id=" << entry.display_id
                   << " flags=0x" << std::hex << entry.flags << std::dec
                   << " mode=" << entry.width << 'x' << entry.height << '@'
                   << entry.refresh_rate_millihz << "mHz\n";
            found = found || entry.display_id == display_id;
          }
          if (found) {
            output << "wcg_settle_ms=5000\n";
            output.flush();
            Sleep(5000);

            vdd::SetDisplayHdrStateRequest request {};
            request.display_id = display_id;
            request.enabled = 1;
            request.sdr_white_level_nits = vdd::kDefaultSdrWhiteLevelNits;
            const auto transition = client.set_display_hdr_state(request);
            output << "transition_status=" << static_cast<std::uint32_t>(transition.status)
                   << " native_error=" << transition.native_error << '\n';
            if (!transition.ok()) {
              return 3;
            }

            const auto after = client.query_display_state();
            output << "after_status=" << static_cast<std::uint32_t>(after.status)
                   << " native_error=" << after.native_error << '\n';
            output.flush();
            return after.ok() ? 0 : 3;
          }
          last_error = ERROR_NOT_FOUND;
        }
      }
      Sleep(100);
    } while (std::chrono::steady_clock::now() < deadline);

    output << "remote_control_timeout status=" << last_status
           << " native_error=" << last_error << '\n';
    return 3;
  }

  struct ScopedUserObjectDaclGrant {
    HANDLE object = nullptr;
    PSECURITY_DESCRIPTOR original_descriptor = nullptr;
    PACL original_dacl = nullptr;
    PACL granted_dacl = nullptr;
    SECURITY_INFORMATION restore_information = DACL_SECURITY_INFORMATION;
    bool applied = false;

    DWORD apply(const HANDLE target, const PSID sid, const ACCESS_MASK access) {
      object = target;
      const DWORD query_status = GetSecurityInfo(
        object,
        SE_WINDOW_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &original_dacl,
        nullptr,
        &original_descriptor
      );
      if (query_status != ERROR_SUCCESS) {
        return query_status;
      }

      SECURITY_DESCRIPTOR_CONTROL control = 0;
      DWORD revision = 0;
      if (!GetSecurityDescriptorControl(original_descriptor, &control, &revision)) {
        return GetLastError();
      }
      restore_information |= (control & SE_DACL_PROTECTED) ?
                               PROTECTED_DACL_SECURITY_INFORMATION :
                               UNPROTECTED_DACL_SECURITY_INFORMATION;

      EXPLICIT_ACCESSW entry = {};
      entry.grfAccessPermissions = access;
      entry.grfAccessMode = GRANT_ACCESS;
      entry.grfInheritance = NO_INHERITANCE;
      entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
      entry.Trustee.ptstrName = static_cast<LPWSTR>(sid);
      const DWORD acl_status = SetEntriesInAclW(1, &entry, original_dacl, &granted_dacl);
      if (acl_status != ERROR_SUCCESS) {
        return acl_status;
      }
      const DWORD set_status = SetSecurityInfo(
        object,
        SE_WINDOW_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        granted_dacl,
        nullptr
      );
      applied = set_status == ERROR_SUCCESS;
      return set_status;
    }

    DWORD restore() {
      DWORD status = ERROR_SUCCESS;
      if (applied) {
        status = SetSecurityInfo(
          object,
          SE_WINDOW_OBJECT,
          restore_information,
          nullptr,
          nullptr,
          original_dacl,
          nullptr
        );
      }
      if (granted_dacl) {
        LocalFree(granted_dacl);
        granted_dacl = nullptr;
      }
      if (original_descriptor) {
        LocalFree(original_descriptor);
        original_descriptor = nullptr;
      }
      applied = false;
      return status;
    }
  };

  int probe_wcg_prime_hdr_inherited_token(
    const HANDLE user_token,
    const DWORD expected_session_id,
    const std::uint64_t display_id,
    const std::string_view output_root
  ) {
    DWORD process_session_id = 0xffffffffu;
    DWORD token_session_id = 0xffffffffu;
    DWORD returned = 0;
    if (!user_token ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &process_session_id) ||
        !GetTokenInformation(
          user_token,
          TokenSessionId,
          &token_session_id,
          sizeof(token_session_id),
          &returned
        ) ||
        process_session_id != expected_session_id ||
        token_session_id != expected_session_id) {
      return 2;
    }

    const std::string root {output_root};
    const std::string launch_path = root + "\\wcg-prime-native-launch.txt";
    std::ofstream launch {launch_path, std::ios::out | std::ios::trunc};
    if (!launch) {
      return 2;
    }

    DWORD logon_sid_bytes = 0;
    GetTokenInformation(user_token, TokenLogonSid, nullptr, 0, &logon_sid_bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || logon_sid_bytes == 0) {
      launch << "token_logon_sid_size_error=" << GetLastError() << '\n';
      return 2;
    }
    std::vector<std::byte> logon_sid_buffer(logon_sid_bytes);
    if (!GetTokenInformation(
          user_token,
          TokenLogonSid,
          logon_sid_buffer.data(),
          logon_sid_bytes,
          &logon_sid_bytes
        )) {
      launch << "token_logon_sid_error=" << GetLastError() << '\n';
      return 2;
    }
    const auto *logon_groups = reinterpret_cast<const TOKEN_GROUPS *>(logon_sid_buffer.data());
    if (logon_groups->GroupCount != 1 || !IsValidSid(logon_groups->Groups[0].Sid)) {
      launch << "token_logon_sid_invalid=1 group_count=" << logon_groups->GroupCount << '\n';
      return 2;
    }
    const PSID logon_sid = logon_groups->Groups[0].Sid;

    const HWINSTA window_station = OpenWindowStationW(
      L"winsta0",
      FALSE,
      READ_CONTROL | WRITE_DAC
    );
    if (!window_station) {
      launch << "window_station_open_error=" << GetLastError() << '\n';
      return 2;
    }
    const HDESK desktop = OpenDesktopW(
      L"default",
      0,
      FALSE,
      READ_CONTROL | WRITE_DAC
    );
    if (!desktop) {
      launch << "desktop_open_error=" << GetLastError() << '\n';
      CloseWindowStation(window_station);
      return 2;
    }

    constexpr ACCESS_MASK kWindowStationUserAccess =
      WINSTA_ALL_ACCESS | STANDARD_RIGHTS_REQUIRED;
    constexpr ACCESS_MASK kDesktopUserAccess =
      DESKTOP_READOBJECTS |
      DESKTOP_CREATEWINDOW |
      DESKTOP_CREATEMENU |
      DESKTOP_HOOKCONTROL |
      DESKTOP_JOURNALRECORD |
      DESKTOP_JOURNALPLAYBACK |
      DESKTOP_ENUMERATE |
      DESKTOP_WRITEOBJECTS |
      DESKTOP_SWITCHDESKTOP |
      STANDARD_RIGHTS_REQUIRED;
    ScopedUserObjectDaclGrant window_station_grant;
    ScopedUserObjectDaclGrant desktop_grant;
    const DWORD window_station_grant_status = window_station_grant.apply(
      window_station,
      logon_sid,
      kWindowStationUserAccess
    );
    const DWORD desktop_grant_status = window_station_grant_status == ERROR_SUCCESS ?
                                         desktop_grant.apply(desktop, logon_sid, kDesktopUserAccess) :
                                         ERROR_ACCESS_DENIED;
    launch << "window_station_grant_status=" << window_station_grant_status
           << " desktop_grant_status=" << desktop_grant_status << '\n';
    if (window_station_grant_status != ERROR_SUCCESS || desktop_grant_status != ERROR_SUCCESS) {
      const DWORD desktop_restore_status = desktop_grant.restore();
      const DWORD window_station_restore_status = window_station_grant.restore();
      launch << "desktop_restore_status=" << desktop_restore_status
             << " window_station_restore_status=" << window_station_restore_status << '\n';
      CloseDesktop(desktop);
      CloseWindowStation(window_station);
      return 2;
    }

    wchar_t module_path[32768] {};
    const DWORD module_chars = GetModuleFileNameW(nullptr, module_path, ARRAYSIZE(module_path));
    if (module_chars == 0 || module_chars >= ARRAYSIZE(module_path)) {
      launch << "module_path_error=" << GetLastError() << '\n';
      desktop_grant.restore();
      window_station_grant.restore();
      CloseDesktop(desktop);
      CloseWindowStation(window_station);
      return 2;
    }
    const auto quote = [](const std::wstring_view value) {
      return L"\"" + std::wstring(value) + L"\"";
    };
    const std::wstring output_root_wide {output_root.begin(), output_root.end()};
    std::wstring command =
      quote(module_path) + L" --probe-wcg-prime-hdr-native-user " +
      std::to_wstring(expected_session_id) + L" " + std::to_wstring(display_id) + L" " +
      quote(output_root_wide);

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<wchar_t *>(L"winsta0\\default");
    PROCESS_INFORMATION process = {};
    const BOOL created = CreateProcessAsUserW(
      user_token,
      module_path,
      command.data(),
      nullptr,
      nullptr,
      FALSE,
      CREATE_NO_WINDOW,
      nullptr,
      nullptr,
      &startup,
      &process
    );
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    if (!created) {
      launch << "native_user_create_error=" << create_error << '\n';
      const DWORD desktop_restore_status = desktop_grant.restore();
      const DWORD window_station_restore_status = window_station_grant.restore();
      launch << "desktop_restore_status=" << desktop_restore_status
             << " window_station_restore_status=" << window_station_restore_status << '\n';
      CloseDesktop(desktop);
      CloseWindowStation(window_station);
      return 3;
    }

    launch << "native_user_created=1 session=" << expected_session_id
           << " pid=" << process.dwProcessId
           << " inherited_token_session=" << token_session_id << '\n';
    CloseHandle(process.hThread);
    constexpr DWORD kNativeUserSequenceTimeoutMs = 50000;
    const DWORD wait = WaitForSingleObject(process.hProcess, kNativeUserSequenceTimeoutMs);
    DWORD exit_code = STILL_ACTIVE;
    GetExitCodeProcess(process.hProcess, &exit_code);
    if (wait == WAIT_TIMEOUT) {
      TerminateProcess(process.hProcess, ERROR_TIMEOUT);
      WaitForSingleObject(process.hProcess, 2000);
      exit_code = ERROR_TIMEOUT;
    }
    launch << "native_user_complete=1 wait=" << wait
           << " exit=0x" << std::hex << std::uppercase << exit_code << std::dec << '\n';
    CloseHandle(process.hProcess);
    const DWORD desktop_restore_status = desktop_grant.restore();
    const DWORD window_station_restore_status = window_station_grant.restore();
    launch << "desktop_restore_status=" << desktop_restore_status
           << " window_station_restore_status=" << window_station_restore_status << '\n';
    CloseDesktop(desktop);
    CloseWindowStation(window_station);
    if (desktop_restore_status != ERROR_SUCCESS || window_station_restore_status != ERROR_SUCCESS) {
      return 5;
    }
    return wait == WAIT_OBJECT_0 ? static_cast<int>(exit_code) : 4;
  }

  struct DisplayPathInfo {
    bool active = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t refresh_millihz = 0;
  };

  std::optional<UINT32> source_mode_index(const DISPLAYCONFIG_PATH_INFO &path, const UINT32 flags) {
    if ((flags & QDC_VIRTUAL_MODE_AWARE) != 0) {
      if (path.sourceInfo.sourceModeInfoIdx == DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID) {
        return std::nullopt;
      }
      return path.sourceInfo.sourceModeInfoIdx;
    }
    if (path.sourceInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
      return std::nullopt;
    }
    return path.sourceInfo.modeInfoIdx;
  }

  std::optional<UINT32> target_mode_index(const DISPLAYCONFIG_PATH_INFO &path, const UINT32 flags) {
    if ((flags & QDC_VIRTUAL_MODE_AWARE) != 0) {
      if (path.targetInfo.targetModeInfoIdx == DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID) {
        return std::nullopt;
      }
      return path.targetInfo.targetModeInfoIdx;
    }
    if (path.targetInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
      return std::nullopt;
    }
    return path.targetInfo.modeInfoIdx;
  }

  DISPLAYCONFIG_VIDEO_SIGNAL_INFO make_signal_info(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_hz
  ) {
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO signal {};
    const auto total_width = vdd::saturating_u32(static_cast<std::uint64_t>(width) + width / 5u);
    const auto total_height = vdd::saturating_u32(static_cast<std::uint64_t>(height) + height / 20u);
    signal.pixelRate = vdd::saturating_mul_u64(
      vdd::saturating_mul_u64(total_width, total_height),
      refresh_hz
    );
    signal.hSyncFreq.Numerator = vdd::saturating_u32(signal.pixelRate);
    signal.hSyncFreq.Denominator = total_width == 0 ? 1 : total_width;
    signal.vSyncFreq.Numerator = vdd::saturating_u32(signal.pixelRate);
    signal.vSyncFreq.Denominator = vdd::saturating_u32(
      (std::max<std::uint64_t>)(1, vdd::saturating_mul_u64(total_width, total_height))
    );
    signal.activeSize.cx = width;
    signal.activeSize.cy = height;
    signal.totalSize.cx = total_width;
    signal.totalSize.cy = total_height;
    signal.AdditionalSignalInfo.videoStandard = 255;
    signal.AdditionalSignalInfo.vSyncFreqDivider = 1;
    signal.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    return signal;
  }

  DISPLAYCONFIG_VIDEO_SIGNAL_INFO make_active_signal_info(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_hz
  ) {
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO signal {};
    signal.pixelRate = vdd::saturating_mul_u64(
      vdd::saturating_mul_u64(width, height),
      refresh_hz
    );
    signal.hSyncFreq.Numerator = vdd::saturating_u32(static_cast<std::uint64_t>(refresh_hz) * height);
    signal.hSyncFreq.Denominator = 1;
    signal.vSyncFreq.Numerator = refresh_hz;
    signal.vSyncFreq.Denominator = 1;
    signal.activeSize.cx = width;
    signal.activeSize.cy = height;
    signal.totalSize.cx = width;
    signal.totalSize.cy = height;
    signal.AdditionalSignalInfo.videoStandard = 255;
    signal.AdditionalSignalInfo.vSyncFreqDivider = 1;
    signal.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    return signal;
  }

  std::optional<DisplayPathInfo> query_display_path(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id
  ) {
    const auto luid = vdd::to_windows_luid(adapter_luid);
    constexpr std::array<UINT32, 2> kQueryFlags {
      QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE,
      QDC_ONLY_ACTIVE_PATHS
    };

    for (const auto flags: kQueryFlags) {
      auto display_config = query_display_config(flags);
      if (!display_config) {
        continue;
      }

      for (const auto &path: display_config->paths) {
        if (!same_luid(path.targetInfo.adapterId, luid) ||
            path.targetInfo.id != target_id) {
          continue;
        }

        DisplayPathInfo info {};
        info.active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        info.refresh_millihz = rational_to_millihz(path.targetInfo.refreshRate);

        const auto target_idx = target_mode_index(path, flags);
        if (target_idx && *target_idx < display_config->modes.size()) {
          const auto &mode = display_config->modes[*target_idx];
          if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
            info.width = mode.targetMode.targetVideoSignalInfo.activeSize.cx;
            info.height = mode.targetMode.targetVideoSignalInfo.activeSize.cy;
            if (info.refresh_millihz == 0 &&
                mode.targetMode.targetVideoSignalInfo.vSyncFreq.Denominator != 0) {
              info.refresh_millihz = rational_to_millihz(mode.targetMode.targetVideoSignalInfo.vSyncFreq);
            }
          }
        }

        const auto source_idx = source_mode_index(path, flags);
        if ((info.width == 0 || info.height == 0) &&
            source_idx &&
            *source_idx < display_config->modes.size()) {
          const auto &mode = display_config->modes[*source_idx];
          if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            info.width = mode.sourceMode.width;
            info.height = mode.sourceMode.height;
          }
        }

        return info;
      }
    }

    return std::nullopt;
  }

  std::optional<DisplayPathInfo> wait_for_display_path(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const bool present,
    const std::function<void()> &keep_alive = {}
  ) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<DisplayPathInfo> latest;
    do {
      latest = query_display_path(adapter_luid, target_id);
      if (present == latest.has_value()) {
        return latest;
      }
      if (keep_alive) {
        keep_alive();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);

    return latest;
  }

  bool display_mode_matches(
    const DisplayPathInfo &path,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_hz
  ) {
    const auto requested_refresh = vdd::refresh_millihz_from_hz(refresh_hz);
    const auto refresh_delta = path.refresh_millihz > requested_refresh ?
      path.refresh_millihz - requested_refresh :
      requested_refresh - path.refresh_millihz;
    return path.active &&
           path.width == width &&
           path.height == height &&
           refresh_delta <= 1000u;
  }

  std::optional<UINT32> active_source_id_for_target(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id
  ) {
    const auto luid = vdd::to_windows_luid(adapter_luid);
    auto display_config = query_display_config(QDC_ONLY_ACTIVE_PATHS);
    if (!display_config) {
      return std::nullopt;
    }

    for (const auto &path: display_config->paths) {
      if (same_luid(path.targetInfo.adapterId, luid) && path.targetInfo.id == target_id) {
        return path.sourceInfo.id;
      }
    }

    return std::nullopt;
  }

  std::optional<std::wstring> gdi_device_name_for_source(
    const vdd::AdapterLuid &adapter_luid,
    const UINT32 source_id
  ) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name {};
    source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source_name.header.size = sizeof(source_name);
    source_name.header.adapterId = vdd::to_windows_luid(adapter_luid);
    source_name.header.id = source_id;
    if (DisplayConfigGetDeviceInfo(&source_name.header) != ERROR_SUCCESS) {
      return std::nullopt;
    }

    return std::wstring {source_name.viewGdiDeviceName};
  }

  struct DxgiCaptureStats {
    std::atomic<bool> ready {false};
    std::atomic<bool> done {false};
    std::atomic<std::uint32_t> frames {};
    std::atomic<std::uint32_t> timeouts {};
    std::atomic<std::uint32_t> errors {};
    std::atomic<HRESULT> last_result {S_OK};
  };

  std::string hresult_hex(const HRESULT hr) {
    char buffer[16] {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(static_cast<std::uint32_t>(hr)));
    return buffer;
  }

  HRESULT duplicate_output_for_gdi_name(
    const std::wstring &gdi_name,
    Microsoft::WRL::ComPtr<ID3D11Device> &device,
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> &duplication
  ) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
      return hr;
    }

    for (UINT adapter_index = 0;; ++adapter_index) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      hr = factory->EnumAdapters1(adapter_index, &adapter);
      if (hr == DXGI_ERROR_NOT_FOUND) {
        return DXGI_ERROR_NOT_FOUND;
      }
      if (FAILED(hr)) {
        return hr;
      }

      for (UINT output_index = 0;; ++output_index) {
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        hr = adapter->EnumOutputs(output_index, &output);
        if (hr == DXGI_ERROR_NOT_FOUND) {
          break;
        }
        if (FAILED(hr)) {
          return hr;
        }

        DXGI_OUTPUT_DESC desc {};
        hr = output->GetDesc(&desc);
        if (FAILED(hr) || gdi_name != desc.DeviceName) {
          continue;
        }

        D3D_FEATURE_LEVEL selected_feature_level {};
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        hr = D3D11CreateDevice(
          adapter.Get(),
          D3D_DRIVER_TYPE_UNKNOWN,
          nullptr,
          D3D11_CREATE_DEVICE_BGRA_SUPPORT,
          nullptr,
          0,
          D3D11_SDK_VERSION,
          &device,
          &selected_feature_level,
          &context
        );
        if (FAILED(hr)) {
          return hr;
        }

        Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
        hr = output.As(&output1);
        if (FAILED(hr)) {
          return hr;
        }

        return output1->DuplicateOutput(device.Get(), &duplication);
      }
    }
  }

  void run_dxgi_duplication_capture(
    const std::wstring gdi_name,
    std::atomic<bool> &stop_requested,
    DxgiCaptureStats &stats
  ) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication;
    HRESULT hr = E_FAIL;
    const auto startup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!stop_requested.load(std::memory_order_acquire)) {
      hr = duplicate_output_for_gdi_name(gdi_name, device, duplication);
      stats.last_result.store(hr, std::memory_order_release);
      if (SUCCEEDED(hr)) {
        break;
      }
      if (std::chrono::steady_clock::now() >= startup_deadline) {
        stats.errors.fetch_add(1, std::memory_order_relaxed);
        stats.done.store(true, std::memory_order_release);
        return;
      }
      device.Reset();
      duplication.Reset();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!duplication) {
      stats.done.store(true, std::memory_order_release);
      return;
    }

    stats.ready.store(true, std::memory_order_release);
    while (!stop_requested.load(std::memory_order_acquire)) {
      DXGI_OUTDUPL_FRAME_INFO frame_info {};
      IDXGIResource *resource_ptr = nullptr;
      hr = duplication->AcquireNextFrame(100, &frame_info, &resource_ptr);
      stats.last_result.store(hr, std::memory_order_release);
      if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        stats.timeouts.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (FAILED(hr)) {
        stats.errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }

      Microsoft::WRL::ComPtr<IDXGIResource> resource;
      resource.Attach(resource_ptr);
      resource.Reset();
      duplication->ReleaseFrame();
      stats.frames.fetch_add(1, std::memory_order_relaxed);
    }
    stats.done.store(true, std::memory_order_release);
  }

  bool wait_for_capture_ready(DxgiCaptureStats &stats, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (stats.ready.load(std::memory_order_acquire)) {
        return true;
      }
      if (stats.done.load(std::memory_order_acquire)) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return stats.ready.load(std::memory_order_acquire);
  }

  class LocalWideString {
  public:
    LocalWideString() = default;
    LocalWideString(const LocalWideString &) = delete;
    LocalWideString &operator=(const LocalWideString &) = delete;

    LocalWideString(LocalWideString &&other) noexcept:
        value_ {std::exchange(other.value_, nullptr)} {}

    LocalWideString &operator=(LocalWideString &&other) noexcept {
      if (this != &other) {
        reset();
        value_ = std::exchange(other.value_, nullptr);
      }
      return *this;
    }

    ~LocalWideString() {
      reset();
    }

    void reset() noexcept {
      if (value_) {
        LocalFree(value_);
        value_ = nullptr;
      }
    }

    [[nodiscard]] LPWSTR *put() {
      reset();
      return &value_;
    }

    [[nodiscard]] const wchar_t *get() const {
      return value_ ? value_ : L"";
    }

  private:
    LPWSTR value_ {};
  };

  class LocalProfileList {
  public:
    LocalProfileList() = default;
    LocalProfileList(const LocalProfileList &) = delete;
    LocalProfileList &operator=(const LocalProfileList &) = delete;

    LocalProfileList(LocalProfileList &&other) noexcept:
        value_ {std::exchange(other.value_, nullptr)} {}

    LocalProfileList &operator=(LocalProfileList &&other) noexcept {
      if (this != &other) {
        reset();
        value_ = std::exchange(other.value_, nullptr);
      }
      return *this;
    }

    ~LocalProfileList() {
      reset();
    }

    void reset() noexcept {
      if (value_) {
        LocalFree(value_);
        value_ = nullptr;
      }
    }

    [[nodiscard]] LPWSTR **put() {
      reset();
      return &value_;
    }

    [[nodiscard]] LPWSTR *get() const {
      return value_;
    }

  private:
    LPWSTR *value_ {};
  };

  using ColorProfileGetDisplayDefaultFn = HRESULT (WINAPI *)(
    WCS_PROFILE_MANAGEMENT_SCOPE,
    LUID,
    UINT32,
    COLORPROFILETYPE,
    COLORPROFILESUBTYPE,
    LPWSTR *
  );
  using ColorProfileGetDisplayListFn = HRESULT (WINAPI *)(
    WCS_PROFILE_MANAGEMENT_SCOPE,
    LUID,
    UINT32,
    LPWSTR **,
    PDWORD
  );
  using ColorProfileGetDisplayUserScopeFn = HRESULT (WINAPI *)(
    LUID,
    UINT32,
    WCS_PROFILE_MANAGEMENT_SCOPE *
  );
  using ColorProfileAddDisplayAssociationFn = HRESULT (WINAPI *)(
    WCS_PROFILE_MANAGEMENT_SCOPE,
    PCWSTR,
    LUID,
    UINT32,
    BOOL,
    BOOL
  );

  struct ColorProfileApi {
    ColorProfileGetDisplayDefaultFn get_default {};
    ColorProfileGetDisplayListFn get_list {};
    ColorProfileGetDisplayUserScopeFn get_user_scope {};
    ColorProfileAddDisplayAssociationFn add_association {};
  };

  const ColorProfileApi *load_color_profile_api() {
    static const ColorProfileApi api = []() {
      ColorProfileApi loaded {};
      const HMODULE module = LoadLibraryExW(L"mscms.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!module) {
        return loaded;
      }

      loaded.get_default = reinterpret_cast<ColorProfileGetDisplayDefaultFn>(
        GetProcAddress(module, "ColorProfileGetDisplayDefault")
      );
      loaded.get_list = reinterpret_cast<ColorProfileGetDisplayListFn>(
        GetProcAddress(module, "ColorProfileGetDisplayList")
      );
      loaded.get_user_scope = reinterpret_cast<ColorProfileGetDisplayUserScopeFn>(
        GetProcAddress(module, "ColorProfileGetDisplayUserScope")
      );
      loaded.add_association = reinterpret_cast<ColorProfileAddDisplayAssociationFn>(
        GetProcAddress(module, "ColorProfileAddDisplayAssociation")
      );
      return loaded;
    }();

    if (!api.get_default || !api.get_list || !api.get_user_scope) {
      return nullptr;
    }
    return &api;
  }

  void print_default_color_profile(
    const ColorProfileApi &api,
    const WCS_PROFILE_MANAGEMENT_SCOPE scope,
    const LUID &adapter_luid,
    const UINT32 source_id,
    const COLORPROFILESUBTYPE subtype,
    const char *label
  ) {
    LocalWideString profile_name;
    const HRESULT result = api.get_default(
      scope,
      adapter_luid,
      source_id,
      CPT_ICC,
      subtype,
      profile_name.put()
    );
    if (FAILED(result)) {
      std::cout << label << "_profile_hresult=0x" << std::hex
                << static_cast<unsigned long>(result) << std::dec << '\n';
      return;
    }

    std::wcout << label << L"_profile=\"" << profile_name.get() << L"\"\n";
  }

  void print_color_profile_list(
    const ColorProfileApi &api,
    const WCS_PROFILE_MANAGEMENT_SCOPE scope,
    const LUID &adapter_luid,
    const UINT32 source_id
  ) {
    LocalProfileList profile_list;
    DWORD profile_count = 0;
    const HRESULT result = api.get_list(
      scope,
      adapter_luid,
      source_id,
      profile_list.put(),
      &profile_count
    );
    if (FAILED(result)) {
      std::cout << "associated_profiles_hresult=0x" << std::hex
                << static_cast<unsigned long>(result) << std::dec << '\n';
      return;
    }

    std::cout << "associated_profile_count=" << profile_count << '\n';
    if (profile_count != 0 && profile_list.get() == nullptr) {
      std::cout << "associated_profiles_error=null_profile_list\n";
      return;
    }
    for (DWORD index = 0; index < profile_count; ++index) {
      const wchar_t *profile_name = profile_list.get()[index] ? profile_list.get()[index] : L"";
      std::wcout << L"associated_profile[" << index << L"]=\"" << profile_name << L"\"\n";
    }
  }

  int query_color_profiles() {
    const ColorProfileApi *color_api = load_color_profile_api();
    if (!color_api) {
      std::cerr << "color profile display APIs are unavailable\n";
      report_helper_event(EVENTLOG_ERROR_TYPE, kEventHelperColorQueryFailed, L"Color profile display APIs are unavailable");
      return 1;
    }

    auto display_config = query_display_config(QDC_ONLY_ACTIVE_PATHS);
    if (!display_config) {
      std::cerr << "color profile query requires active DisplayConfig paths\n";
      report_helper_event(EVENTLOG_ERROR_TYPE, kEventHelperColorQueryFailed, L"Color profile query requires active DisplayConfig paths");
      return 1;
    }

    std::size_t active_paths = 0;
    std::size_t queried = 0;
    for (const auto &path: display_config->paths) {
      if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
        continue;
      }
      ++active_paths;

      WCS_PROFILE_MANAGEMENT_SCOPE scope {};
      const HRESULT scope_result = color_api->get_user_scope(
        path.targetInfo.adapterId,
        path.sourceInfo.id,
        &scope
      );

      std::cout << "color_profile_path[" << queried << "]"
                << " source_luid=" << path.sourceInfo.adapterId.HighPart << ':' << path.sourceInfo.adapterId.LowPart
                << " source_id=" << path.sourceInfo.id
                << " target_luid=" << path.targetInfo.adapterId.HighPart << ':' << path.targetInfo.adapterId.LowPart
                << " target_id=" << path.targetInfo.id;
      if (FAILED(scope_result)) {
        std::cout << " scope_hresult=0x" << std::hex
                  << static_cast<unsigned long>(scope_result) << std::dec << '\n';
        continue;
      }

      std::cout << " scope=" << static_cast<unsigned int>(scope) << '\n';
      print_color_profile_list(*color_api, scope, path.targetInfo.adapterId, path.sourceInfo.id);
      print_default_color_profile(*color_api, scope, path.targetInfo.adapterId, path.sourceInfo.id, kStandardDisplayColorMode, "standard");
      print_default_color_profile(*color_api, scope, path.targetInfo.adapterId, path.sourceInfo.id, kExtendedDisplayColorMode, "extended");
      ++queried;
    }

    if (active_paths != 0 && queried == 0) {
      std::cerr << "color profile query failed for every active path\n";
    }
    std::cout << "color_profile_active_paths=" << active_paths << '\n';
    std::cout << "color_profile_paths=" << queried << '\n';
    report_helper_event(
      queried == 0 ? EVENTLOG_ERROR_TYPE : EVENTLOG_INFORMATION_TYPE,
      queried == 0 ? kEventHelperColorQueryFailed : kEventHelperColorQueryCompleted,
      "Color profile query paths=" + std::to_string(queried) + " active_paths=" + std::to_string(active_paths)
    );
    return queried == 0 ? 1 : 0;
  }

  std::optional<LUID> parse_luid(const std::string_view text) {
    const auto separator = text.find(':');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= text.size()) {
      return std::nullopt;
    }

    const auto high = vdd::parse_probe_i32_token(text.substr(0, separator));
    const auto low = vdd::parse_probe_u32_token(text.substr(separator + 1));
    if (!high || !low) {
      return std::nullopt;
    }

    LUID luid {};
    luid.HighPart = static_cast<LONG>(*high);
    luid.LowPart = static_cast<DWORD>(*low);
    return luid;
  }

  int associate_color_profile(
    const LUID &source_luid,
    const UINT32 source_id,
    const std::wstring &profile_name,
    const bool advanced_color,
    const bool set_default
  ) {
    const ColorProfileApi *color_api = load_color_profile_api();
    if (!color_api || !color_api->add_association) {
      std::cerr << "color profile display association API is unavailable\n";
      report_helper_event(EVENTLOG_ERROR_TYPE, kEventHelperColorAssociationFailed, L"Color profile display association API is unavailable");
      return 1;
    }
    if (profile_name.empty()) {
      std::cerr << "color profile association requires a profile name\n";
      report_helper_event(EVENTLOG_ERROR_TYPE, kEventHelperColorAssociationFailed, L"Color profile association requires a profile name");
      return 1;
    }

    WCS_PROFILE_MANAGEMENT_SCOPE scope {};
    HRESULT result = color_api->get_user_scope(source_luid, source_id, &scope);
    if (FAILED(result)) {
      std::cerr << "color profile scope query failed hresult=0x" << std::hex
                << static_cast<unsigned long>(result) << std::dec << '\n';
      report_helper_event(EVENTLOG_ERROR_TYPE, kEventHelperColorAssociationFailed, L"Color profile scope query failed");
      return 1;
    }

    result = color_api->add_association(
      scope,
      profile_name.c_str(),
      source_luid,
      source_id,
      set_default ? TRUE : FALSE,
      advanced_color ? TRUE : FALSE
    );
    std::cout << "color_profile_association_hresult=0x" << std::hex
              << static_cast<unsigned long>(result) << std::dec << '\n'
              << "advanced_color_association=" << (advanced_color ? 1 : 0) << '\n'
              << "set_default=" << (set_default ? 1 : 0) << '\n';
    if (FAILED(result)) {
      report_helper_event(EVENTLOG_ERROR_TYPE, kEventHelperColorAssociationFailed, L"Color profile association failed");
      return 1;
    }

    report_helper_event(EVENTLOG_INFORMATION_TYPE, kEventHelperColorAssociationCompleted, L"Color profile association completed");
    return 0;
  }

  LONG set_active_display_mode(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_hz
  ) {
    const auto source_id = active_source_id_for_target(adapter_luid, target_id);
    if (!source_id) {
      std::cout << "gdi_set_mode_error=no_active_source\n";
      return DISP_CHANGE_BADPARAM;
    }
    const auto gdi_name = gdi_device_name_for_source(adapter_luid, *source_id);
    if (!gdi_name) {
      std::cout << "gdi_set_mode_error=no_gdi_name source_id=" << *source_id << '\n';
      return DISP_CHANGE_BADPARAM;
    }
    if (!valid_display_mode_dimensions(width, height) || refresh_hz == 0) {
      std::cout << "gdi_set_mode_error=invalid_mode\n";
      return DISP_CHANGE_BADMODE;
    }

    constexpr DWORD kMaxEnumeratedDisplayModes = 4096;
    DWORD matching_modes = 0;
    bool mode_enumeration_limit_reached = true;
    for (DWORD mode_index = 0; mode_index < kMaxEnumeratedDisplayModes; ++mode_index) {
      DEVMODEW enumerated {};
      enumerated.dmSize = sizeof(enumerated);
      if (!EnumDisplaySettingsExW(gdi_name->c_str(), mode_index, &enumerated, 0)) {
        mode_enumeration_limit_reached = false;
        break;
      }
      if (enumerated.dmPelsWidth == width &&
          enumerated.dmPelsHeight == height &&
          enumerated.dmDisplayFrequency == refresh_hz &&
          matching_modes != (std::numeric_limits<DWORD>::max)()) {
        ++matching_modes;
      }
    }
    if (mode_enumeration_limit_reached) {
      std::cout << "gdi_set_mode_warning=mode_enumeration_limit_reached"
                << " limit=" << kMaxEnumeratedDisplayModes << '\n';
    }
    std::wcout << L"gdi_set_mode_device=" << *gdi_name
               << L" source_id=" << *source_id
               << L" requested=" << width << L'x' << height << L'@' << refresh_hz
               << L" matching_modes=" << matching_modes << L'\n';

    DEVMODEW mode {};
    mode.dmSize = sizeof(mode);
    mode.dmPelsWidth = width;
    mode.dmPelsHeight = height;
    mode.dmDisplayFrequency = refresh_hz;
    mode.dmBitsPerPel = 32;
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
    const auto result = ChangeDisplaySettingsExW(gdi_name->c_str(), &mode, nullptr, 0, nullptr);
    std::cout << "gdi_set_mode_result=" << result << '\n';
    return result;
  }

  bool ensure_active_display_mode(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_hz,
    const std::function<void()> &keep_alive = {}
  ) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool mode_set_attempted = false;
    do {
      const auto path = query_display_path(adapter_luid, target_id);
      if (path && display_mode_matches(*path, width, height, refresh_hz)) {
        return true;
      }

      if (path && path->active && !mode_set_attempted) {
        // Windows can reuse the previous mode on a recycled target id. Apply
        // the requested GDI mode explicitly, then poll DisplayConfig again.
        mode_set_attempted = true;
        (void) set_active_display_mode(adapter_luid, target_id, width, height, refresh_hz);
      }

      if (keep_alive) {
        keep_alive();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);

    return false;
  }

  int run_temporary_mode_probe(
    vdd::ControlClient &client,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_hz,
    const std::uint32_t timeout_ms,
    const char *label
  ) {
    auto request = make_temporary_request(width, height, refresh_hz);
    request.requested_timeout_ms = timeout_ms;

    const auto created = client.create_temporary_display(request);
    if (!created.ok()) {
      return fail(std::string {label} + " create temporary display failed", created);
    }

    const vdd::LeaseRequest lease_request {
      vdd::kApiNamespaceGuid,
      request.lease_id,
      request.requested_timeout_ms,
      0
    };
    const auto cleanup_created = [&]() {
      return client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
    };
    const auto feed_lease = [&]() {
      (void) client.feed_lease(lease_request);
    };

    std::cout << label
              << "_display_id=" << created.value.display_id
              << " target_id=" << created.value.target_id
              << " connector_index=" << created.value.connector_index
              << " effective_timeout_ms=" << created.value.effective_timeout_ms
              << " adapter_luid=" << vdd::to_windows_luid(created.value.os_adapter_luid).HighPart
              << ':' << vdd::to_windows_luid(created.value.os_adapter_luid).LowPart << '\n';

    dump_display_config_paths(created.value.os_adapter_luid, created.value.target_id);
    feed_lease();

    const auto activate_result = activate_target_path_result(
      created.value.os_adapter_luid,
      created.value.target_id,
      width,
      height,
      refresh_hz
    );
    std::cout << label << "_activate_result=" << activate_result << '\n';
    if (activate_result != ERROR_SUCCESS) {
      (void) apply_extended_topology();
    }
    feed_lease();

    const auto mode_ready = ensure_active_display_mode(
      created.value.os_adapter_luid,
      created.value.target_id,
      width,
      height,
      refresh_hz,
      feed_lease
    );
    dump_active_paths_for_adapter(created.value.os_adapter_luid);
    const auto active_path = query_display_path(created.value.os_adapter_luid, created.value.target_id);
    if (active_path) {
      std::cout << label << "_active_path=1"
                << " width=" << active_path->width
                << " height=" << active_path->height
                << " refresh_millihz=" << active_path->refresh_millihz << '\n';
    } else {
      std::cout << label << "_active_path=0\n";
    }
    dump_display_config_paths(created.value.os_adapter_luid, created.value.target_id);

    const auto removed = cleanup_created();
    if (!removed.ok()) {
      return fail(std::string {label} + " remove temporary display failed", removed);
    }

    if (!mode_ready || !active_path || !display_mode_matches(*active_path, width, height, refresh_hz)) {
      std::cerr << label << " mode probe failed: expected "
                << width << 'x' << height << '@' << refresh_hz
                << " got "
                << (active_path ? active_path->width : 0) << 'x'
                << (active_path ? active_path->height : 0) << '@'
                << (active_path ? active_path->refresh_millihz : 0) << "mHz"
                << " activate_result=" << activate_result << '\n';
      return 1;
    }

    std::cout << label << "=1"
              << " width=" << width
              << " height=" << height
              << " refresh_hz=" << refresh_hz << '\n';
    return 0;
  }

  int probe_displaymanager_acquire_new_temp_target(
    vdd::ControlClient &client,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_hz
  ) {
#ifdef _MSC_VER
    using namespace winrt::Windows::Devices::Display::Core;
    struct TargetKey {
      std::int32_t adapter_high {};
      std::uint32_t adapter_low {};
      std::uint32_t relative_id {};

      bool operator==(const TargetKey &) const = default;
    };
    const auto key_for_target = [](const DisplayTarget &target) {
      const auto adapter_id = target.Adapter().Id();
      return TargetKey {adapter_id.HighPart, adapter_id.LowPart, target.AdapterRelativeId()};
    };
    const auto print_key = [](const char *prefix, const TargetKey &key) {
      std::cout << prefix << "=" << key.adapter_high << ':' << key.adapter_low
                << ':' << key.relative_id << '\n';
    };

    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    auto manager = DisplayManager::Create(DisplayManagerOptions::None);
    std::vector<TargetKey> baseline_connected;
    for (const auto &target: manager.GetCurrentTargets()) {
      if (target.IsConnected() && !target.IsStale()) {
        baseline_connected.push_back(key_for_target(target));
      }
    }
    std::cout << "displaymanager_acquire_race_baseline_connected="
              << baseline_connected.size() << '\n';

    auto request = make_temporary_request(width, height, refresh_hz);
    std::optional<vdd::ControlResult<vdd::CreateTemporaryDisplayResult>> created;
    std::atomic_bool create_done {false};
    std::thread creator {[&]() {
      created.emplace(client.create_temporary_display(request));
      create_done.store(true, std::memory_order_release);
    }};

    bool target_seen = false;
    bool acquired = false;
    std::uint64_t acquire_attempts = 0;
    std::optional<TargetKey> candidate_key;
    DisplayManagerResult last_result = DisplayManagerResult::UnknownFailure;
    bool have_last_result = false;
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(3);
    try {
      do {
        for (const auto &target: manager.GetCurrentTargets()) {
          if (!target.IsConnected() || target.IsStale()) {
            continue;
          }
          const auto key = key_for_target(target);
          if (std::find(baseline_connected.begin(), baseline_connected.end(), key) !=
              baseline_connected.end()) {
            continue;
          }
          if (!candidate_key || *candidate_key != key) {
            candidate_key = key;
            target_seen = true;
            print_key("displaymanager_acquire_race_candidate", key);
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - started
            );
            std::cout << "displaymanager_acquire_race_first_seen_us=" << elapsed.count() << '\n';
          }

          const auto result = manager.TryAcquireTarget(target);
          ++acquire_attempts;
          if (!have_last_result || result != last_result) {
            std::cout << "displaymanager_acquire_race_result="
                      << static_cast<std::int32_t>(result)
                      << " attempt=" << acquire_attempts << '\n';
            last_result = result;
            have_last_result = true;
          }
          if (result == DisplayManagerResult::Success) {
            acquired = true;
            manager.ReleaseTarget(target);
            std::cout << "displaymanager_acquire_race_released=1\n";
            break;
          }
        }
        if (acquired) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < deadline);
    } catch (const winrt::hresult_error &error) {
      std::cout << "displaymanager_acquire_race_exception=0x" << std::hex
                << std::setw(8) << std::setfill('0')
                << static_cast<std::uint32_t>(error.code().value) << std::dec
                << " message=" << winrt::to_string(error.message()) << '\n';
    }

    creator.join();
    std::cout << "displaymanager_acquire_race_create_done="
              << static_cast<int>(create_done.load(std::memory_order_acquire)) << '\n';
    if (!created || !created->ok()) {
      manager.Stop();
      if (!created) {
        std::cerr << "create temporary display returned no result\n";
        return 1;
      }
      return fail("create temporary display failed", *created);
    }

    std::cout << "displaymanager_acquire_race_created_target="
              << vdd::to_windows_luid(created->value.os_adapter_luid).HighPart << ':'
              << vdd::to_windows_luid(created->value.os_adapter_luid).LowPart << ':'
              << created->value.target_id << '\n';
    const auto removed = client.remove_temporary_display(
      {vdd::kApiNamespaceGuid, request.lease_id, request.display_id}
    );
    manager.Stop();
    if (!removed.ok()) {
      return fail("remove temporary display failed", removed);
    }

    std::cout << "displaymanager_acquire_race_target_seen=" << static_cast<int>(target_seen)
              << " attempts=" << acquire_attempts
              << " acquired=" << static_cast<int>(acquired)
              << " removed=1\n";
    return acquired ? 0 : 1;
#else
    (void) client;
    (void) width;
    (void) height;
    (void) refresh_hz;
    std::cerr << "DisplayManager acquisition-race probe requires MSVC C++/WinRT\n";
    return 1;
#endif
  }

  int probe_displaymanager_acquire_arriving_target(const std::uint32_t timeout_ms) {
#ifdef _MSC_VER
    using namespace winrt::Windows::Devices::Display::Core;
    struct TargetKey {
      std::int32_t adapter_high {};
      std::uint32_t adapter_low {};
      std::uint32_t relative_id {};

      bool operator==(const TargetKey &) const = default;
    };
    const auto key_for_target = [](const DisplayTarget &target) {
      const auto adapter_id = target.Adapter().Id();
      return TargetKey {adapter_id.HighPart, adapter_id.LowPart, target.AdapterRelativeId()};
    };

    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      auto manager = DisplayManager::Create(DisplayManagerOptions::None);
      std::vector<TargetKey> baseline_connected;
      for (const auto &target: manager.GetCurrentTargets()) {
        if (target.IsConnected() && !target.IsStale()) {
          baseline_connected.push_back(key_for_target(target));
        }
      }
      std::cout << "displaymanager_arrival_watch_ready=1 baseline_connected="
                << baseline_connected.size() << " timeout_ms=" << timeout_ms << '\n';
      std::cout.flush();

      bool target_seen = false;
      bool acquired = false;
      std::uint64_t acquire_attempts = 0;
      std::optional<TargetKey> candidate_key;
      DisplayManagerResult last_result = DisplayManagerResult::UnknownFailure;
      bool have_last_result = false;
      const auto started = std::chrono::steady_clock::now();
      const auto deadline = started + std::chrono::milliseconds(timeout_ms);
      do {
        for (const auto &target: manager.GetCurrentTargets()) {
          if (!target.IsConnected() || target.IsStale()) {
            continue;
          }
          const auto key = key_for_target(target);
          if (std::find(baseline_connected.begin(), baseline_connected.end(), key) !=
              baseline_connected.end()) {
            continue;
          }
          if (!candidate_key || *candidate_key != key) {
            candidate_key = key;
            target_seen = true;
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - started
            );
            std::cout << "displaymanager_arrival_candidate=" << key.adapter_high << ':'
                      << key.adapter_low << ':' << key.relative_id
                      << " first_seen_us=" << elapsed.count() << '\n';
          }

          const auto result = manager.TryAcquireTarget(target);
          ++acquire_attempts;
          if (!have_last_result || result != last_result) {
            std::cout << "displaymanager_arrival_acquire_result="
                      << static_cast<std::int32_t>(result)
                      << " attempt=" << acquire_attempts << '\n';
            last_result = result;
            have_last_result = true;
          }
          if (result == DisplayManagerResult::Success) {
            acquired = true;
            manager.ReleaseTarget(target);
            std::cout << "displaymanager_arrival_released=1\n";
            break;
          }
        }
        if (acquired) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < deadline);

      manager.Stop();
      std::cout << "displaymanager_arrival_target_seen=" << static_cast<int>(target_seen)
                << " attempts=" << acquire_attempts
                << " acquired=" << static_cast<int>(acquired) << '\n';
      return acquired ? 0 : 1;
    } catch (const winrt::hresult_error &error) {
      std::cerr << "displaymanager_arrival_exception=0x" << std::hex
                << std::setw(8) << std::setfill('0')
                << static_cast<std::uint32_t>(error.code().value) << std::dec
                << " message=" << winrt::to_string(error.message()) << '\n';
      return 1;
    }
#else
    (void) timeout_ms;
    std::cerr << "DisplayManager arrival probe requires MSVC C++/WinRT\n";
    return 1;
#endif
  }

  int launch_displaymanager_owner2_probe_in_session(
    const DWORD target_session_id,
    const std::string_view output_path
  ) {
    DWORD privilege_error = ERROR_SUCCESS;
    const bool debug_enabled = enable_process_privilege(L"SeDebugPrivilege", &privilege_error);
    std::cout << "owner2_launcher_debug_privilege=" << static_cast<int>(debug_enabled)
              << " native_error=" << privilege_error << '\n';

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
      std::cerr << "owner2_launcher_snapshot_error=" << GetLastError() << '\n';
      return 1;
    }

    HANDLE source_token = nullptr;
    DWORD source_pid = 0;
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
      do {
        DWORD process_session_id = 0xffffffffu;
        if (_wcsicmp(entry.szExeFile, L"explorer.exe") != 0 ||
            !ProcessIdToSessionId(entry.th32ProcessID, &process_session_id) ||
            process_session_id != target_session_id) {
          continue;
        }
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) {
          std::cout << "owner2_launcher_candidate_pid=" << entry.th32ProcessID
                    << " process_open_error=" << GetLastError() << '\n';
          continue;
        }
        constexpr DWORD token_access = TOKEN_DUPLICATE | TOKEN_QUERY;
        if (OpenProcessToken(process, token_access, &source_token)) {
          source_pid = entry.th32ProcessID;
          CloseHandle(process);
          break;
        }
        std::cout << "owner2_launcher_candidate_pid=" << entry.th32ProcessID
                  << " token_open_error=" << GetLastError() << '\n';
        CloseHandle(process);
      } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (!source_token) {
      std::cerr << "owner2_launcher_explorer_token_error=" << GetLastError()
                << " target_session=" << target_session_id << '\n';
      return 1;
    }

    HANDLE primary_token = nullptr;
    const bool duplicated = DuplicateTokenEx(
      source_token,
      MAXIMUM_ALLOWED,
      nullptr,
      SecurityImpersonation,
      TokenPrimary,
      &primary_token
    ) != FALSE;
    const DWORD duplicate_error = duplicated ? ERROR_SUCCESS : GetLastError();
    CloseHandle(source_token);
    if (!duplicated) {
      std::cerr << "owner2_launcher_duplicate_error=" << duplicate_error << '\n';
      return 1;
    }

    DWORD token_session_id = 0xffffffffu;
    DWORD returned = 0;
    const bool token_session_valid = GetTokenInformation(
      primary_token,
      TokenSessionId,
      &token_session_id,
      sizeof(token_session_id),
      &returned
    ) != FALSE && token_session_id == target_session_id;
    std::cout << "owner2_launcher_source_pid=" << source_pid
              << " token_session=" << token_session_id
              << " target_session=" << target_session_id
              << " token_session_valid=" << static_cast<int>(token_session_valid) << '\n';
    if (!token_session_valid) {
      CloseHandle(primary_token);
      return 1;
    }

    wchar_t module_path[32768] {};
    const DWORD module_chars = GetModuleFileNameW(nullptr, module_path, ARRAYSIZE(module_path));
    if (module_chars == 0 || module_chars >= ARRAYSIZE(module_path)) {
      std::cerr << "owner2_launcher_module_error=" << GetLastError() << '\n';
      CloseHandle(primary_token);
      return 1;
    }
    const auto quote = [](const std::wstring_view value) {
      return L"\"" + std::wstring(value) + L"\"";
    };
    const auto output_path_wide = widen_ascii(output_path);
    std::wstring command = quote(module_path) +
      L" --probe-displaymanager-desktop-owner2-hdr-functionalize-current-session " +
      quote(output_path_wide);
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<wchar_t *>(L"winsta0\\default");
    PROCESS_INFORMATION process {};
    const BOOL created = CreateProcessWithTokenW(
      primary_token,
      LOGON_WITH_PROFILE,
      module_path,
      command.data(),
      CREATE_NO_WINDOW,
      nullptr,
      nullptr,
      &startup,
      &process
    );
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(primary_token);
    std::cout << "owner2_launcher_create=" << static_cast<int>(created != FALSE)
              << " native_error=" << create_error
              << " child_pid=" << (created ? process.dwProcessId : 0) << '\n';
    if (!created) {
      return 1;
    }

    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 60'000);
    DWORD exit_code = STILL_ACTIVE;
    (void) GetExitCodeProcess(process.hProcess, &exit_code);
    if (wait == WAIT_TIMEOUT) {
      (void) TerminateProcess(process.hProcess, ERROR_TIMEOUT);
      (void) WaitForSingleObject(process.hProcess, 2'000);
      exit_code = ERROR_TIMEOUT;
    }
    CloseHandle(process.hProcess);
    std::cout << "owner2_launcher_wait=" << wait
              << " child_exit=0x" << std::hex << std::setw(8) << std::setfill('0')
              << exit_code << std::dec << '\n';
    return wait == WAIT_OBJECT_0 ? static_cast<int>(exit_code) : 1;
  }

  std::string to_utf8(const std::wstring &value) {
    if (value.empty()) {
      return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
      return "<utf8 conversion failed>";
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), required, nullptr, nullptr) <= 0) {
      return "<utf8 conversion failed>";
    }
    if (!result.empty() && result.back() == '\0') {
      result.pop_back();
    }
    return result;
  }

  int diagnose_control_devices() {
    std::uint32_t enumerate_error = 0;
    const auto devices = vdd::enumerate_control_devices(&enumerate_error);
    bool any_openable = false;

    std::cout << "control_interface_count=" << devices.size()
              << " enumerate_error=" << enumerate_error << '\n';
    for (std::size_t index = 0; index < devices.size(); ++index) {
      const auto &device = devices[index];
      any_openable = any_openable || device.openable;
      std::cout << "control_interface[" << index << "]"
                << " openable=" << (device.openable ? 1 : 0)
                << " native_error=" << device.native_error
                << " path=\"" << to_utf8(device.device_path) << "\"\n";
    }

    return any_openable ? 0 : 1;
  }
#endif
}  // namespace

int main(const int argc, char **argv) {
#ifndef _WIN32
  (void) argc;
  (void) argv;
  std::cerr << "virtualdisplay_probe is only supported on Windows.\n";
  return 1;
#else
  if (argc < 2) {
    print_usage();
    return 2;
  }

  const std::string command {argv[1]};
  if (command == "--probe-wcg-prime-hdr-native-user") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    std::uint32_t session_id {};
    std::uint64_t display_id {};
    if (!read_u32_arg(argc, argv, 2, 0, "session id", session_id) ||
        !read_u64_arg(argc, argv, 3, 0, "display id", display_id) ||
        display_id == 0) {
      return 2;
    }
    return probe_wcg_prime_hdr_native_user(session_id, display_id, argv[4]);
  }
  if (command == "--remote-current-session-wcg-to-hdr") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    std::uint64_t display_id {};
    if (!read_u64_arg(argc, argv, 2, 0, "display id", display_id) || display_id == 0) {
      return 2;
    }
    return remote_current_session_wcg_to_hdr(display_id, argv[3]);
  }
  if (command == "--probe-wcg-prime-hdr-inherited-token") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    std::uint64_t token_value {};
    std::uint32_t session_id {};
    std::uint64_t display_id {};
    if (!read_u64_arg(argc, argv, 2, 0, "inherited token handle", token_value) ||
        !read_u32_arg(argc, argv, 3, 0, "session id", session_id) ||
        !read_u64_arg(argc, argv, 4, 0, "display id", display_id) ||
        token_value == 0 || display_id == 0) {
      return 2;
    }
    return probe_wcg_prime_hdr_inherited_token(
      reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(token_value)),
      session_id,
      display_id,
      argv[5]
    );
  }
  if (command.find("private-functionalize") != std::string::npos) {
    std::ofstream invocation {
      "C:\\Windows\\Temp\\sunshine-vdd-private-functionalize-invocation.txt",
      std::ios::out | std::ios::trunc
    };
    if (invocation) {
      invocation << "argc=" << argc << '\n';
      for (int index = 0; index < argc; ++index) {
        invocation << "argv[" << index << "]=" << argv[index] << '\n';
      }
    }
  }
  if (command == "--diagnose") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    return diagnose_control_devices();
  }

  if (command == "--probe-displaymanager-acquire-arriving-target") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    std::uint32_t timeout_ms {};
    if (!read_u32_arg(argc, argv, 2, 5000u, "timeout_ms", timeout_ms) ||
        timeout_ms < 100u || timeout_ms > 30'000u) {
      return 2;
    }
    return probe_displaymanager_acquire_arriving_target(timeout_ms);
  }

  if (command == "--launch-displaymanager-owner2-probe-in-session") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    std::uint32_t session_id {};
    if (!read_u32_arg(argc, argv, 2, 0, "session id", session_id) ||
        session_id == 0 || session_id == 0xffffffffu) {
      return 2;
    }
    return launch_displaymanager_owner2_probe_in_session(session_id, argv[3]);
  }

  if (command == "--apply-extended-topology-current-session") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    const LONG result = apply_extended_topology_result();
    if (result != ERROR_SUCCESS) {
      std::cerr << "apply current-session extended topology failed native_error=" << result << '\n';
      return 1;
    }
    std::cout << "current_session_extended_topology_applied=1\n";
    return 0;
  }

  if (command == "--probe-idd-hdr-functionalize-current-session" ||
      command == "--probe-idd-hdr-functionalize-shared-owner-current-session" ||
      command == "--probe-idd-hdr-functionalize-nongdi-source-current-session") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    if (argc == 2) {
      return probe_idd_hdr_functionalize_current_session(
        command == "--probe-idd-hdr-functionalize-shared-owner-current-session",
        command == "--probe-idd-hdr-functionalize-nongdi-source-current-session"
      );
    }
    std::ofstream output {argv[2], std::ios::out | std::ios::trunc};
    if (!output) {
      std::cerr << "could not open functionalize output path\n";
      return 1;
    }
    auto *old_out = std::cout.rdbuf(output.rdbuf());
    auto *old_err = std::cerr.rdbuf(output.rdbuf());
    const int result = probe_idd_hdr_functionalize_current_session(
      command == "--probe-idd-hdr-functionalize-shared-owner-current-session",
      command == "--probe-idd-hdr-functionalize-nongdi-source-current-session"
    );
    std::cout.flush();
    std::cerr.flush();
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    return result;
  }

  if (command == "--dump-display-config-current-session") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    dump_display_config_paths();
    return 0;
  }

  if (command == "--query-private-functionalize-current-session") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    if (argc == 2) {
      return query_private_functionalize_current_session();
    }
    std::ofstream output {argv[2], std::ios::out | std::ios::trunc};
    if (!output) {
      std::cerr << "could not open private functionalize output path\n";
      return 1;
    }
    auto *old_out = std::cout.rdbuf(output.rdbuf());
    auto *old_err = std::cerr.rdbuf(output.rdbuf());
    const int result = query_private_functionalize_current_session();
    std::cout.flush();
    std::cerr.flush();
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    return result;
  }

  if (command == "--probe-displaymanager-fp16-functionalize-current-session" ||
      command == "--probe-displaymanager-fp16-enforce-functionalize-current-session" ||
      command == "--probe-displaymanager-desktop-hdr-functionalize-current-session" ||
      command == "--probe-displaymanager-desktop-owner2-hdr-functionalize-current-session") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    const bool enforce = command == "--probe-displaymanager-fp16-enforce-functionalize-current-session";
    const bool start_for_desktop =
      command == "--probe-displaymanager-desktop-hdr-functionalize-current-session" ||
      command == "--probe-displaymanager-desktop-owner2-hdr-functionalize-current-session";
    const bool claim_display_manager_owner =
      command == "--probe-displaymanager-desktop-owner2-hdr-functionalize-current-session";
    if (argc == 2) {
      return probe_displaymanager_fp16_functionalize_current_session(
        enforce,
        start_for_desktop,
        claim_display_manager_owner
      );
    }
    std::ofstream output {argv[2], std::ios::out | std::ios::trunc};
    if (!output) {
      std::cerr << "could not open DisplayManager functionalize output path\n";
      return 1;
    }
    auto *old_out = std::cout.rdbuf(output.rdbuf());
    auto *old_err = std::cerr.rdbuf(output.rdbuf());
    if (claim_display_manager_owner) {
      std::cout << "displaymanager_owner2_desktop_settle_ms=5000\n" << std::flush;
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    const int result = probe_displaymanager_fp16_functionalize_current_session(
      enforce,
      start_for_desktop,
      claim_display_manager_owner
    );
    std::cout.flush();
    std::cerr.flush();
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    return result;
  }

  if (command == "--query-hdr-target") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    const auto target_luid = parse_luid(argv[2]);
    if (!target_luid) {
      std::cerr << "invalid target_luid\n";
      return 2;
    }
    std::uint32_t target_id {};
    if (!read_u32_arg(argc, argv, 3, 0, "target_id", target_id)) {
      return 2;
    }
    LONG native_error = ERROR_SUCCESS;
    const auto info = query_advanced_color(
      vdd::from_windows_luid(*target_luid),
      target_id,
      &native_error
    );
    std::cout << "hdr_target_luid=" << target_luid->HighPart << ':' << target_luid->LowPart
              << " target_id=" << target_id << '\n';
    if (!info) {
      std::cerr << "HDR target query failed native_error=" << native_error << '\n';
      return 1;
    }
    print_advanced_color(*info);
    return 0;
  }

  if (command == "--query-d3dkmt-current-mode") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    const auto adapter_luid = parse_luid(argv[2]);
    if (!adapter_luid) {
      std::cerr << "invalid adapter_luid\n";
      return 2;
    }
    std::uint32_t source_id {};
    if (!read_u32_arg(argc, argv, 3, 0, "source_id", source_id)) {
      return 2;
    }
    return query_idd_current_mode_command(*adapter_luid, source_id);
  }

  if (command == "--query-d3dkmt-mode-list") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    const auto adapter_luid = parse_luid(argv[2]);
    if (!adapter_luid) {
      std::cerr << "invalid adapter_luid\n";
      return 2;
    }
    std::uint32_t source_id {};
    if (!read_u32_arg(argc, argv, 3, 0, "source_id", source_id)) {
      return 2;
    }
    return query_idd_mode_list_command(*adapter_luid, source_id);
  }

  if (command == "--query-vidpn-ownership-current-session") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    if (argc == 3) {
      std::ofstream output {argv[2], std::ios::out | std::ios::trunc};
      if (!output) {
        std::cerr << "could not open VidPn ownership output path\n";
        return 1;
      }
      auto *old_out = std::cout.rdbuf(output.rdbuf());
      auto *old_err = std::cerr.rdbuf(output.rdbuf());
      const int result = query_vidpn_ownership_current_session();
      std::cout.flush();
      std::cerr.flush();
      std::cout.rdbuf(old_out);
      std::cerr.rdbuf(old_err);
      return result;
    }
    return query_vidpn_ownership_current_session();
  }

  if (command == "--probe-idd-hdr-gate") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    const auto target_luid = parse_luid(argv[2]);
    if (!target_luid) {
      std::cerr << "invalid target_luid\n";
      return 2;
    }
    std::uint32_t target_id {};
    if (!read_u32_arg(argc, argv, 3, 0, "target_id", target_id)) {
      return 2;
    }
    return probe_idd_hdr_gate(*target_luid, target_id);
  }

  if (command == "--set-hdr-target") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    const auto target_luid = parse_luid(argv[2]);
    if (!target_luid) {
      std::cerr << "invalid target_luid\n";
      return 2;
    }
    std::uint32_t target_id {};
    std::uint32_t enabled {};
    if (!read_u32_arg(argc, argv, 3, 0, "target_id", target_id) ||
        !read_u32_arg(argc, argv, 4, 1, "HDR state", enabled)) {
      return 2;
    }
    const auto adapter_luid = vdd::from_windows_luid(*target_luid);
    LONG native_error = ERROR_SUCCESS;
    const auto before = query_advanced_color(adapter_luid, target_id, &native_error);
    std::cout << "hdr_target_luid=" << target_luid->HighPart << ':' << target_luid->LowPart
              << " target_id=" << target_id << '\n';
    if (!before) {
      std::cerr << "HDR target query failed native_error=" << native_error << '\n';
      return 1;
    }
    std::cout << "before_";
    print_advanced_color(*before);

    LONG set_error = ERROR_SUCCESS;
    bool requested = before->v2 ?
      set_hdr_state(adapter_luid, target_id, enabled != 0, &set_error) :
      set_advanced_color(adapter_luid, target_id, enabled != 0, &set_error);
    if (!requested) {
      if (before->v2) {
        std::cout << "hdr_target_legacy_fallback=1 v2_native_error=" << set_error << '\n';
        requested = set_advanced_color(adapter_luid, target_id, enabled != 0, &set_error);
      }
      if (!requested) {
        std::cerr << "HDR target state change failed native_error=" << set_error << '\n';
        return 1;
      }
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<AdvancedColorInfo> after;
    do {
      after = query_advanced_color(adapter_luid, target_id, &native_error);
      if (after && advanced_color_matches(*after, enabled != 0)) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);

    if ((!after || !advanced_color_matches(*after, enabled != 0)) && before->v2) {
      std::cout << "hdr_target_legacy_fallback=1\n";
      if (set_advanced_color(adapter_luid, target_id, enabled != 0)) {
        const auto fallback_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
          after = query_advanced_color(adapter_luid, target_id, &native_error);
          if (after && advanced_color_matches(*after, enabled != 0)) {
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while (std::chrono::steady_clock::now() < fallback_deadline);
      }
    }

    if (after) {
      std::cout << "after_";
      print_advanced_color(*after);
    }
    return after && advanced_color_matches(*after, enabled != 0) ? 0 : 1;
  }

  if (command == "--set-hdr-current-session") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    if (argc == 4) {
      FILE *redirected = nullptr;
      if (freopen_s(&redirected, argv[3], "w", stdout) != 0 || redirected == nullptr) {
        std::cerr << "open HDR proof output failed path=" << argv[3] << '\n';
        return 2;
      }
      std::cerr.rdbuf(std::cout.rdbuf());
    }
    std::uint32_t enabled {};
    if (!read_u32_arg(argc, argv, 2, 1, "HDR state", enabled)) {
      return 2;
    }
    return set_current_session_hdr_state(enabled != 0);
  }

  if (command == "--apply-extended-topology") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    if (vdd::probe_command_execution_stage(command) == vdd::ProbeCommandExecutionStage::ActiveSessionBeforeControlDevice) {
      if (const int session_status = require_active_console_session(command); session_status != 0) {
        return session_status;
      }
    }

    const LONG result = apply_extended_topology_result();
    if (result != ERROR_SUCCESS) {
      std::cerr << "apply extended topology failed native_error=" << result << '\n';
      return 1;
    }

    std::cout << "extended_topology_applied=1\n";
    return 0;
  }

  if (command == "--query-color-profiles") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    if (vdd::probe_command_execution_stage(command) == vdd::ProbeCommandExecutionStage::ActiveSessionBeforeControlDevice) {
      if (const int session_status = require_active_console_session(command); session_status != 0) {
        return session_status;
      }
    }
    return query_color_profiles();
  }

  if (command == "--associate-color-profile") {
    if (!require_command_arg_count(command, argc)) {
      print_usage();
      return 2;
    }
    if (vdd::probe_command_execution_stage(command) == vdd::ProbeCommandExecutionStage::ActiveSessionBeforeControlDevice) {
      if (const int session_status = require_active_console_session(command); session_status != 0) {
        return session_status;
      }
    }

    const auto source_luid = parse_luid(argv[2]);
    if (!source_luid) {
      std::cerr << "invalid source_luid\n";
      return 2;
    }

    bool advanced_color = false;
    bool set_default = false;
    for (int index = 5; index < argc; ++index) {
      const std::string_view flag {argv[index]};
      if (flag == "--advanced-color") {
        advanced_color = true;
      } else if (flag == "--default") {
        set_default = true;
      } else {
        print_usage();
        return 2;
      }
    }

    std::uint32_t source_id {};
    if (!read_u32_arg(argc, argv, 3, 0, "source_id", source_id)) {
      return 2;
    }

    return associate_color_profile(*source_luid, source_id, widen_ascii(argv[4]), advanced_color, set_default);
  }

  const bool remote_query = command == "--remote-query-permanent" || command == "--remote-query-state";
  const bool remote_set = command == "--remote-set-permanent" ||
                          command == "--remote-set-hdr" ||
                          command == "--remote-set-mode";
  std::uint32_t remote_session_id {};
  if ((remote_query || remote_set) &&
      !read_u32_arg(argc, argv, 2, 0, "remote session id", remote_session_id)) {
    return 2;
  }

  auto opened = (remote_query || remote_set) ?
    vdd::open_remote_control_device_for_session(remote_session_id) :
    vdd::open_first_control_device();
  if (!opened.ok()) {
    return fail("open control device failed", {opened.status, opened.native_error});
  }

  vdd::ControlClient client {*opened.transport};
  const auto protocol = client.query_protocol_version();
  if (!protocol.ok()) {
    return fail("protocol check failed", protocol);
  }

  if (command == "--check") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    std::cout << "protocol=" << protocol.value.major << '.'
              << protocol.value.minor << '.' << protocol.value.patch << '\n';
    return 0;
  }

  if (vdd::probe_command_execution_stage(command) == vdd::ProbeCommandExecutionStage::ControlDeviceBeforeActiveSession) {
    if (const int session_status = require_active_console_session(command); session_status != 0) {
      return session_status;
    }
  }

  if (command == "--query-permanent") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    const auto result = client.query_permanent_display_count();
    if (!result.ok()) {
      return fail("query permanent count failed", result);
    }
    std::cout << "permanent=" << result.value.current_display_count
              << " max=" << result.value.max_display_count
              << " temporary=" << result.value.temporary_display_count << '\n';
    return 0;
  }

  if (command == "--remote-query-permanent") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    const auto result = client.query_permanent_display_count();
    if (!result.ok()) {
      return fail("query remote permanent count failed", result);
    }
    std::cout << "remote_session=" << remote_session_id
              << " permanent=" << result.value.current_display_count
              << " max=" << result.value.max_display_count
              << " temporary=" << result.value.temporary_display_count
              << " mode=" << result.value.width << 'x' << result.value.height << '@'
              << (result.value.refresh_rate_millihz / 1000.0) << "Hz"
              << " name=" << result.value.display_name << '\n';
    return 0;
  }

  if (command == "--remote-query-state") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    const auto result = client.query_display_state();
    if (!result.ok()) {
      return fail("query remote display state failed", result);
    }
    std::cout << "remote_session=" << remote_session_id
              << " permanent=" << result.value.permanent_display_count
              << " temporary=" << result.value.temporary_display_count
              << " entries=" << result.value.entry_count << '\n';
    const auto entry_count = (std::min)(result.value.entry_count, vdd::kMaxDisplayStateEntries);
    for (std::uint32_t index = 0; index < entry_count; ++index) {
      const auto &entry = result.value.entries[index];
      std::cout << "entry=" << index
                << " kind=" << entry.kind
                << " flags=0x" << std::hex << entry.flags << std::dec
                << " display_id=" << entry.display_id
                << " connector=" << entry.connector_index
                << " mode=" << entry.width << 'x' << entry.height << '@'
                << (entry.refresh_rate_millihz / 1000.0) << "Hz"
                << " name=" << entry.display_name << '\n';
    }
    return 0;
  }

  if (command == "--apply-manifest-topology") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    return apply_manifest_topology(client);
  }

  if (command == "--set-permanent") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    vdd::PermanentDisplayCountRequest request {};
    if (!read_u32_arg(argc, argv, 2, 0, "permanent count", request.display_count)) {
      return 2;
    }
    const auto result = client.set_permanent_display_count(request);
    if (!result.ok()) {
      return fail("set permanent count failed", result);
    }
    std::cout << "permanent=" << result.value.current_display_count
              << " max=" << result.value.max_display_count
              << " temporary=" << result.value.temporary_display_count << '\n';
    return 0;
  }

  if (command == "--remote-set-permanent") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    vdd::PermanentDisplayCountRequest request {};
    if (!read_u32_arg(argc, argv, 3, 0, "permanent count", request.display_count)) {
      return 2;
    }
    const auto result = client.set_permanent_display_count(request);
    if (!result.ok()) {
      return fail("set remote permanent count failed", result);
    }
    std::cout << "remote_session=" << remote_session_id
              << " permanent=" << result.value.current_display_count
              << " max=" << result.value.max_display_count
              << " temporary=" << result.value.temporary_display_count << '\n';
    return 0;
  }

  if (command == "--remote-set-hdr") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    vdd::SetDisplayHdrStateRequest request {};
    if (!read_u64_arg(argc, argv, 3, 0, "display id", request.display_id) ||
        !read_u32_arg(argc, argv, 4, 0, "HDR enabled state", request.enabled) ||
        !read_u32_arg(
          argc,
          argv,
          5,
          vdd::kDefaultSdrWhiteLevelNits,
          "SDR white level",
          request.sdr_white_level_nits
        )) {
      return 2;
    }
    const auto result = client.set_display_hdr_state(request);
    if (!result.ok()) {
      return fail("set remote HDR state failed", result);
    }
    std::cout << "remote_session=" << remote_session_id
              << " display_id=" << request.display_id
              << " hdr_enabled=" << request.enabled
              << " sdr_white_level_nits=" << request.sdr_white_level_nits << '\n';
    return 0;
  }

  if (command == "--remote-set-mode") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    vdd::SetDisplayModeRequest request {};
    if (!read_u64_arg(argc, argv, 3, 0, "display id", request.display_id) ||
        !read_u32_arg(argc, argv, 4, 0, "width", request.width) ||
        !read_u32_arg(argc, argv, 5, 0, "height", request.height) ||
        !read_u32_arg(argc, argv, 6, 0, "refresh rate (millihertz)", request.refresh_rate_millihz)) {
      return 2;
    }
    const auto result = client.set_display_mode(request);
    if (!result.ok()) {
      return fail("set remote display mode failed", result);
    }
    std::cout << "remote_session=" << remote_session_id
              << " display_id=" << request.display_id
              << " mode=" << request.width << 'x' << request.height << '@'
              << (request.refresh_rate_millihz / 1000.0) << "Hz\n";
    return 0;
  }

  if (command == "--self-test-permanent") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    const auto before = client.query_permanent_display_count();
    if (!before.ok()) {
      return fail("query permanent count failed", before);
    }

    std::uint32_t requested {};
    if (!read_u32_arg(
          argc,
          argv,
          2,
          before.value.current_display_count == 0 ? 1u : 0u,
          "permanent count",
          requested
        )) {
      return 2;
    }
    if (requested > before.value.max_display_count) {
      std::cerr << "requested permanent count " << requested
                << " exceeds max " << before.value.max_display_count << '\n';
      return 2;
    }

    vdd::PermanentDisplayCountRequest request {};
    request.display_count = requested;
    const auto changed = client.set_permanent_display_count(request);
    if (!changed.ok()) {
      return fail("set permanent count failed", changed);
    }

    bool restore_needed = true;
    const auto restore_previous_count = [&]() {
      const auto current = client.query_permanent_display_count();
      if (!current.ok()) {
        return current;
      }
      if (current.value.current_display_count != requested) {
        std::cerr << "not restoring permanent count because another client changed it"
                  << " current=" << current.value.current_display_count
                  << " expected=" << requested << '\n';
        return current;
      }

      vdd::PermanentDisplayCountRequest restore {};
      restore.display_count = before.value.current_display_count;
      return client.set_permanent_display_count(restore);
    };
    struct RestorePermanentCountOnExit {
      bool &restore_needed;
      const std::function<vdd::ControlResult<vdd::PermanentDisplayCountResult>()> restore;
      ~RestorePermanentCountOnExit() {
        if (restore_needed) {
          (void) restore();
        }
      }
    } restore_on_exit {restore_needed, restore_previous_count};

    if (changed.value.current_display_count != requested) {
      std::cerr << "set permanent count returned " << changed.value.current_display_count
                << " after requesting " << requested << '\n';
      return 1;
    }

    const auto restored = restore_previous_count();
    if (!restored.ok()) {
      return fail("restore permanent count failed", restored);
    }
    if (restored.value.current_display_count != before.value.current_display_count) {
      restore_needed = false;
      std::cerr << "restore permanent count returned " << restored.value.current_display_count
                << " after requesting " << before.value.current_display_count << '\n';
      return 1;
    }
    restore_needed = false;

    std::cout << "permanent_self_test=" << requested
              << " restored=" << restored.value.current_display_count
              << " max=" << before.value.max_display_count
              << " temporary=" << restored.value.temporary_display_count << '\n';
    return 0;
  }

  if (command == "--self-test-temp") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t refresh_hz {};
    if (!read_u32_arg(argc, argv, 2, 1920u, "width", width) ||
        !read_u32_arg(argc, argv, 3, 1080u, "height", height) ||
        !read_u32_arg(argc, argv, 4, 60u, "refresh_hz", refresh_hz)) {
      return 2;
    }
    const auto request = make_temporary_request(width, height, refresh_hz);

    const auto created = client.create_temporary_display(request);
    if (!created.ok()) {
      return fail("create temporary display failed", created);
    }

    const vdd::LeaseRequest lease_request {
      vdd::kApiNamespaceGuid,
      request.lease_id,
      request.requested_timeout_ms,
      0
    };
    const auto queried = client.query_lease(lease_request);
    if (!queried.ok()) {
      const auto cleanup = client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
      if (!cleanup.ok()) {
        return fail("remove temporary display after query lease failed", cleanup);
      }
      return fail("query lease failed", queried);
    }

    const auto removed = client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
    if (!removed.ok()) {
      return fail("remove temporary display failed", removed);
    }

    std::cout << "created_display_id=" << created.value.display_id
              << " target_id=" << created.value.target_id
              << " connector_index=" << created.value.connector_index
              << " lease_temporary_count=" << queried.value.temporary_display_count << '\n';
    return 0;
  }

  if (command == "--probe-displaymanager-acquire-new-temp-target") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t refresh_hz {};
    if (!read_u32_arg(argc, argv, 2, 1920u, "width", width) ||
        !read_u32_arg(argc, argv, 3, 1080u, "height", height) ||
        !read_u32_arg(argc, argv, 4, 60u, "refresh_hz", refresh_hz)) {
      return 2;
    }
    return probe_displaymanager_acquire_new_temp_target(client, width, height, refresh_hz);
  }

  if (command == "--self-test-4k240") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    std::uint32_t timeout_ms {};
    if (!read_u32_arg(argc, argv, 2, 10'000u, "timeout_ms", timeout_ms)) {
      return 2;
    }
    return run_temporary_mode_probe(client, 3840u, 2160u, 240u, timeout_ms, "self_test_4k240");
  }

  if (command == "--self-test-hdr") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t refresh_hz {};
    if (!read_u32_arg(argc, argv, 2, 1920u, "width", width) ||
        !read_u32_arg(argc, argv, 3, 1080u, "height", height) ||
        !read_u32_arg(argc, argv, 4, 60u, "refresh_hz", refresh_hz)) {
      return 2;
    }
    const auto request = make_temporary_request(width, height, refresh_hz);

    const auto created = client.create_temporary_display(request);
    if (!created.ok()) {
      return fail("create temporary HDR display failed", created);
    }

    const auto remove_created = [&]() {
      return client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
    };

    if (!activate_target_path(created.value.os_adapter_luid, created.value.target_id, width, height, refresh_hz)) {
      (void) apply_extended_topology();
    }
    if (!ensure_active_display_mode(created.value.os_adapter_luid, created.value.target_id, width, height, refresh_hz)) {
      (void) remove_created();
      std::cerr << "temporary HDR display resolution mismatch after activation\n";
      return 1;
    }

    LONG advanced_color_error = ERROR_SUCCESS;
    const auto before = wait_for_advanced_color(
      created.value.os_adapter_luid,
      created.value.target_id,
      false,
      &advanced_color_error
    );
    if (!before) {
      (void) remove_created();
      std::cerr << "advanced color query failed for target " << created.value.target_id
                << " native_error=" << advanced_color_error << '\n';
      return 1;
    }

    const bool hdr_set = set_hdr_state(created.value.os_adapter_luid, created.value.target_id, true);
    const bool advanced_color_set = set_advanced_color(created.value.os_adapter_luid, created.value.target_id, true);
    const auto after = wait_for_advanced_color(
      created.value.os_adapter_luid,
      created.value.target_id,
      true,
      &advanced_color_error
    );

    const auto removed = remove_created();
    if (!removed.ok()) {
      return fail("remove temporary HDR display failed", removed);
    }

    if (!after) {
      std::cerr << "advanced color query did not return after HDR request\n";
      return 1;
    }

    print_advanced_color(*after);

    if (!after->v2 || !after->hdr_supported) {
      std::cerr << "temporary display is not reported as HDR-supported by Windows\n";
      return 1;
    }
    if (!hdr_set && !advanced_color_set) {
      std::cerr << "Windows rejected both HDR and Advanced Color enable requests\n";
      return 1;
    }
    if (after->limited_by_policy || !after->supported || !after->hdr_enabled || after->bits_per_color_channel < 10) {
      std::cerr << "temporary display did not enter HDR 10-bit mode after request\n";
      return 1;
    }

    std::cout << "hdr_self_test=1"
              << " created_display_id=" << created.value.display_id
              << " target_id=" << created.value.target_id
              << " connector_index=" << created.value.connector_index << '\n';
    return 0;
  }

  if (command == "--self-test-initial-remote-hdr") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t refresh_hz {};
    if (!read_u32_arg(argc, argv, 2, 1920u, "width", width) ||
        !read_u32_arg(argc, argv, 3, 1080u, "height", height) ||
        !read_u32_arg(argc, argv, 4, 60u, "refresh_hz", refresh_hz)) {
      return 2;
    }
    auto request = make_temporary_request(width, height, refresh_hz);
    request.flags |= vdd::kCreateTemporaryDisplayFlagInitialRemoteHdr;

    const auto created = client.create_temporary_display(request);
    if (!created.ok()) {
      return fail("create initial-remote-HDR proof display failed", created);
    }
    const auto remove_created = [&]() {
      return client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
    };

    LONG native_error = ERROR_SUCCESS;
    const auto state = wait_for_advanced_color(
      created.value.os_adapter_luid,
      created.value.target_id,
      true,
      &native_error
    );
    const auto removed = remove_created();
    if (!removed.ok()) {
      return fail("remove initial-remote-HDR proof display failed", removed);
    }
    if (!state) {
      std::cerr << "initial remote HDR proof query failed native_error=" << native_error << '\n';
      return 1;
    }
    print_advanced_color(*state);
    const bool initial_hdr = state->v2 && state->supported && state->active &&
                             state->hdr_supported && state->hdr_enabled &&
                             !state->limited_by_policy && state->bits_per_color_channel >= 10;
    std::cout << "initial_remote_hdr=" << (initial_hdr ? 1 : 0)
              << " created_display_id=" << created.value.display_id
              << " target_id=" << created.value.target_id
              << " connector_index=" << created.value.connector_index << '\n';
    return initial_hdr ? 0 : 1;
  }

  if (command == "--self-test-lease-expiry") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t refresh_hz {};
    std::uint32_t timeout_ms {};
    if (!read_u32_arg(argc, argv, 2, 1920u, "width", width) ||
        !read_u32_arg(argc, argv, 3, 1080u, "height", height) ||
        !read_u32_arg(argc, argv, 4, 60u, "refresh_hz", refresh_hz) ||
        !read_u32_arg(argc, argv, 5, 3'000u, "timeout_ms", timeout_ms)) {
      return 2;
    }
    auto request = make_temporary_request(width, height, refresh_hz);
    request.requested_timeout_ms = timeout_ms;

    const auto created = client.create_temporary_display(request);
    if (!created.ok()) {
      return fail("create lease-expiry display failed", created);
    }

    const vdd::LeaseRequest lease_request {
      vdd::kApiNamespaceGuid,
      request.lease_id,
      request.requested_timeout_ms,
      0
    };

    const auto active = client.query_lease(lease_request);
    if (!active.ok()) {
      (void) client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
      return fail("query active lease failed", active);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(created.value.effective_timeout_ms + 2'000u));

    const auto expired = client.query_lease(lease_request);
    if (!expired.ok()) {
      (void) client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
      return fail("query expired lease failed", expired);
    }

    if (active.value.lease_exists == 0 ||
        active.value.temporary_display_count != 1 ||
        expired.value.lease_exists != 0 ||
        expired.value.temporary_display_count != 0) {
      (void) client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
      std::cerr << "lease expiry state mismatch"
                << " active_exists=" << active.value.lease_exists
                << " active_temporary=" << active.value.temporary_display_count
                << " expired_exists=" << expired.value.lease_exists
                << " expired_temporary=" << expired.value.temporary_display_count << '\n';
      return 1;
    }

    std::cout << "lease_expiry_self_test=1"
              << " display_id=" << created.value.display_id
              << " target_id=" << created.value.target_id
              << " connector_index=" << created.value.connector_index
              << " effective_timeout_ms=" << created.value.effective_timeout_ms
              << " expired=1\n";
    return 0;
  }

  if (command == "--qa-multi-temp-lease") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    std::uint32_t count {};
    std::uint32_t timeout_ms {};
    if (!read_u32_arg(argc, argv, 2, 3u, "count", count) ||
        !read_u32_arg(argc, argv, 3, 3'000u, "timeout_ms", timeout_ms)) {
      return 2;
    }
    if (count == 0 || count > 8) {
      std::cerr << "multi-temp QA count must be in the range 1..8\n";
      return 2;
    }

    const auto before_count = client.query_permanent_display_count();
    if (!before_count.ok()) {
      return fail("query permanent count failed", before_count);
    }

    const auto lease_id = transient_id(0x717a0000);
    std::vector<vdd::CreateTemporaryDisplayResult> created_displays;
    std::vector<std::uint32_t> connector_indexes;
    created_displays.reserve(count);
    connector_indexes.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
      constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 4> kSizes {{
        {1280u, 720u},
        {1920u, 1080u},
        {2560u, 1440u},
        {3840u, 2160u}
      }};
      const auto [width, height] = kSizes[index % kSizes.size()];
      auto request = make_temporary_request(width, height, index % 2 == 0 ? 60u : 120u);
      request.lease_id = lease_id;
      request.display_id = transient_id(0x517a0000 + index);
      request.requested_timeout_ms = timeout_ms;

      const auto created = client.create_temporary_display(request);
      if (!created.ok()) {
        (void) client.release_lease({vdd::kApiNamespaceGuid, lease_id, timeout_ms, 0});
        return fail("create multi-temp QA display failed", created);
      }
      if (std::find(connector_indexes.begin(), connector_indexes.end(), created.value.connector_index) != connector_indexes.end()) {
        (void) client.release_lease({vdd::kApiNamespaceGuid, lease_id, timeout_ms, 0});
        std::cerr << "multi-temp QA reused connector index " << created.value.connector_index << '\n';
        return 1;
      }
      connector_indexes.push_back(created.value.connector_index);
      created_displays.push_back(created.value);
    }

    const vdd::LeaseRequest lease_request {
      vdd::kApiNamespaceGuid,
      lease_id,
      timeout_ms,
      0
    };
    const auto active = client.query_lease(lease_request);
    if (!active.ok()) {
      (void) client.release_lease(lease_request);
      return fail("query multi-temp QA lease failed", active);
    }
    if (active.value.lease_exists == 0 || active.value.temporary_display_count != count) {
      (void) client.release_lease(lease_request);
      std::cerr << "multi-temp QA lease count mismatch: expected " << count
                << " got " << active.value.temporary_display_count << '\n';
      return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(active.value.effective_timeout_ms + 2'000u));
    const auto expired = client.query_lease(lease_request);
    if (!expired.ok()) {
      (void) client.release_lease(lease_request);
      return fail("query expired multi-temp QA lease failed", expired);
    }
    if (expired.value.lease_exists != 0 || expired.value.temporary_display_count != 0) {
      (void) client.release_lease(lease_request);
      std::cerr << "multi-temp QA lease did not expire cleanly"
                << " exists=" << expired.value.lease_exists
                << " temporary=" << expired.value.temporary_display_count << '\n';
      return 1;
    }

    const auto after_count = client.query_permanent_display_count();
    if (!after_count.ok()) {
      return fail("query permanent count after multi-temp QA failed", after_count);
    }
    if (after_count.value.temporary_display_count != before_count.value.temporary_display_count) {
      std::cerr << "multi-temp QA leaked temporary displays: before="
                << before_count.value.temporary_display_count
                << " after=" << after_count.value.temporary_display_count << '\n';
      return 1;
    }

    std::cout << "qa_multi_temp_lease=1"
              << " count=" << count
              << " effective_timeout_ms=" << active.value.effective_timeout_ms
              << " expired=1";
    for (const auto connector_index: connector_indexes) {
      std::cout << " connector_index=" << connector_index;
    }
    std::cout << '\n';
    return 0;
  }

  if (command == "--qa-temp-identity-retention") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t refresh_hz {};
    std::uint32_t timeout_ms {};
    if (!read_u32_arg(argc, argv, 2, 1920u, "width", width) ||
        !read_u32_arg(argc, argv, 3, 1080u, "height", height) ||
        !read_u32_arg(argc, argv, 4, 60u, "refresh_hz", refresh_hz) ||
        !read_u32_arg(argc, argv, 5, 30'000u, "timeout_ms", timeout_ms)) {
      return 2;
    }

    const auto before_count = client.query_permanent_display_count();
    if (!before_count.ok()) {
      return fail("query permanent count failed", before_count);
    }

    const auto stable_display_id = transient_id(0x51dd1000);
    const auto first_lease_id = transient_id(0x717e1000);
    auto first_request = make_temporary_request(width, height, refresh_hz);
    first_request.lease_id = first_lease_id;
    first_request.display_id = stable_display_id;
    first_request.requested_timeout_ms = timeout_ms;
    std::strncpy(first_request.display_name, "Sunshine Retain", sizeof(first_request.display_name) - 1);

    const auto first = client.create_temporary_display(first_request);
    if (!first.ok()) {
      return fail("create retained identity display failed", first);
    }

    const auto remove_first = [&]() {
      return client.remove_temporary_display({vdd::kApiNamespaceGuid, first_request.lease_id, first_request.display_id});
    };
    const vdd::LeaseRequest first_lease {
      vdd::kApiNamespaceGuid,
      first_request.lease_id,
      first_request.requested_timeout_ms,
      0
    };
    const auto feed_first = [&]() {
      (void) client.feed_lease(first_lease);
    };

    if (!activate_target_path(first.value.os_adapter_luid, first.value.target_id, width, height, refresh_hz)) {
      (void) apply_extended_topology();
    }
    if (!ensure_active_display_mode(first.value.os_adapter_luid, first.value.target_id, width, height, refresh_hz, feed_first)) {
      (void) remove_first();
      std::cerr << "retained identity display resolution mismatch after first activation\n";
      return 1;
    }

    LONG advanced_color_error = ERROR_SUCCESS;
    const auto first_before_color = wait_for_advanced_color(
      first.value.os_adapter_luid,
      first.value.target_id,
      false,
      &advanced_color_error,
      feed_first
    );
    if (!first_before_color) {
      (void) remove_first();
      std::cerr << "advanced color query failed before retained identity HDR request"
                << " native_error=" << advanced_color_error << '\n';
      return 1;
    }

    const bool hdr_set = set_hdr_state(first.value.os_adapter_luid, first.value.target_id, true);
    const bool advanced_color_set = set_advanced_color(first.value.os_adapter_luid, first.value.target_id, true);
    const auto first_after_color = wait_for_advanced_color(
      first.value.os_adapter_luid,
      first.value.target_id,
      true,
      &advanced_color_error,
      feed_first
    );
    if (!first_after_color) {
      (void) remove_first();
      std::cerr << "advanced color query did not return after retained identity HDR request\n";
      return 1;
    }
    if (!first_after_color->v2 || !first_after_color->hdr_supported) {
      (void) remove_first();
      std::cerr << "retained identity display is not reported as HDR-supported by Windows\n";
      return 1;
    }
    if (!hdr_set && !advanced_color_set) {
      (void) remove_first();
      std::cerr << "Windows rejected both retained identity HDR and Advanced Color enable requests\n";
      return 1;
    }
    if (first_after_color->limited_by_policy ||
        !first_after_color->supported ||
        !first_after_color->hdr_enabled ||
        first_after_color->bits_per_color_channel < 10) {
      (void) remove_first();
      std::cerr << "retained identity display did not enter HDR 10-bit mode after request\n";
      return 1;
    }

    auto removed_first = remove_first();
    if (!removed_first.ok()) {
      return fail("remove retained identity display failed", removed_first);
    }
    if (wait_for_display_path(first.value.os_adapter_luid, first.value.target_id, false)) {
      std::cerr << "retained identity display path did not depart after removal\n";
      return 1;
    }

    const auto filler_lease_id = transient_id(0x717e2000);
    const vdd::LeaseRequest filler_lease {
      vdd::kApiNamespaceGuid,
      filler_lease_id,
      timeout_ms,
      0
    };
    std::vector<vdd::CreateTemporaryDisplayResult> fillers;
    fillers.reserve(2);
    for (std::uint32_t index = 0; index < 2; ++index) {
      auto filler_request = make_temporary_request(index == 0 ? 1280u : 2560u, index == 0 ? 720u : 1440u, index == 0 ? 60u : 120u);
      filler_request.lease_id = filler_lease_id;
      filler_request.display_id = transient_id(0x51dd2000 + index);
      filler_request.requested_timeout_ms = timeout_ms;
      const auto filler = client.create_temporary_display(filler_request);
      if (!filler.ok()) {
        (void) client.release_lease(filler_lease);
        return fail("create filler identity display failed", filler);
      }
      if (filler.value.connector_index == first.value.connector_index) {
        (void) client.release_lease(filler_lease);
        std::cerr << "filler display reused retained identity connector "
                  << first.value.connector_index << '\n';
        return 1;
      }
      fillers.push_back(filler.value);
    }

    const auto second_lease_id = transient_id(0x717e3000);
    auto second_request = make_temporary_request(width, height, refresh_hz);
    second_request.lease_id = second_lease_id;
    second_request.display_id = stable_display_id;
    second_request.requested_timeout_ms = timeout_ms;
    std::strncpy(second_request.display_name, "Sunshine Retain", sizeof(second_request.display_name) - 1);

    const auto second = client.create_temporary_display(second_request);
    if (!second.ok()) {
      (void) client.release_lease(filler_lease);
      return fail("recreate retained identity display failed", second);
    }

    const auto cleanup_second = [&]() {
      return client.remove_temporary_display({vdd::kApiNamespaceGuid, second_request.lease_id, second_request.display_id});
    };
    const vdd::LeaseRequest second_lease {
      vdd::kApiNamespaceGuid,
      second_request.lease_id,
      second_request.requested_timeout_ms,
      0
    };
    const auto feed_second = [&]() {
      (void) client.feed_lease(second_lease);
      (void) client.feed_lease(filler_lease);
    };

    if (second.value.connector_index != first.value.connector_index ||
        second.value.target_id != first.value.target_id) {
      (void) cleanup_second();
      (void) client.release_lease(filler_lease);
      std::cerr << "retained identity changed connector/target"
                << " first_connector=" << first.value.connector_index
                << " second_connector=" << second.value.connector_index
                << " first_target=" << first.value.target_id
                << " second_target=" << second.value.target_id << '\n';
      return 1;
    }

    if (!activate_target_path(second.value.os_adapter_luid, second.value.target_id, width, height, refresh_hz)) {
      (void) apply_extended_topology();
    }
    if (!ensure_active_display_mode(second.value.os_adapter_luid, second.value.target_id, width, height, refresh_hz, feed_second)) {
      (void) cleanup_second();
      (void) client.release_lease(filler_lease);
      std::cerr << "retained identity display resolution mismatch after recreation\n";
      return 1;
    }

    const auto restored_color = wait_for_advanced_color(
      second.value.os_adapter_luid,
      second.value.target_id,
      true,
      &advanced_color_error,
      feed_second
    );
    if (!restored_color) {
      (void) cleanup_second();
      (void) client.release_lease(filler_lease);
      std::cerr << "advanced color query did not return after retained identity recreation\n";
      return 1;
    }
    print_advanced_color(*restored_color);
    if (restored_color->limited_by_policy ||
        !restored_color->supported ||
        !restored_color->hdr_supported ||
        !restored_color->hdr_enabled ||
        restored_color->bits_per_color_channel < 10) {
      (void) cleanup_second();
      (void) client.release_lease(filler_lease);
      std::cerr << "HDR profile was not retained for recreated temporary display\n";
      return 1;
    }

    const auto removed_second = cleanup_second();
    (void) client.release_lease(filler_lease);
    if (!removed_second.ok()) {
      return fail("remove recreated retained identity display failed", removed_second);
    }

    const auto after_count = client.query_permanent_display_count();
    if (!after_count.ok()) {
      return fail("query permanent count after retained identity QA failed", after_count);
    }
    if (after_count.value.temporary_display_count != before_count.value.temporary_display_count) {
      std::cerr << "retained identity QA leaked temporary displays: before="
                << before_count.value.temporary_display_count
                << " after=" << after_count.value.temporary_display_count << '\n';
      return 1;
    }

    std::cout << "qa_temp_identity_retention=1"
              << " display_id=" << stable_display_id
              << " target_id=" << second.value.target_id
              << " connector_index=" << second.value.connector_index
              << " filler_connector_0=" << fillers[0].connector_index
              << " filler_connector_1=" << fillers[1].connector_index
              << " hdr_retained=1\n";
    return 0;
  }

  if (command == "--debug-temp-config") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t refresh_hz {};
    std::uint32_t timeout_ms {};
    if (!read_u32_arg(argc, argv, 2, 1920u, "width", width) ||
        !read_u32_arg(argc, argv, 3, 1080u, "height", height) ||
        !read_u32_arg(argc, argv, 4, 60u, "refresh_hz", refresh_hz) ||
        !read_u32_arg(argc, argv, 5, 10'000u, "timeout_ms", timeout_ms)) {
      return 2;
    }
    auto request = make_temporary_request(width, height, refresh_hz);
    request.requested_timeout_ms = timeout_ms;

    const auto created = client.create_temporary_display(request);
    if (!created.ok()) {
      return fail("create debug temporary display failed", created);
    }

    std::cout << "debug_display_id=" << created.value.display_id
              << " target_id=" << created.value.target_id
              << " connector_index=" << created.value.connector_index
              << " adapter_luid=" << vdd::to_windows_luid(created.value.os_adapter_luid).HighPart
              << ':' << vdd::to_windows_luid(created.value.os_adapter_luid).LowPart << '\n';

    dump_display_config_paths(created.value.os_adapter_luid, created.value.target_id);

    const vdd::LeaseRequest lease_request {
      vdd::kApiNamespaceGuid,
      request.lease_id,
      request.requested_timeout_ms,
      0
    };
    const auto feed_debug_lease = [&]() {
      (void) client.feed_lease(lease_request);
    };

    feed_debug_lease();
    const auto activate_result = activate_target_path_result(
      created.value.os_adapter_luid,
      created.value.target_id,
      width,
      height,
      refresh_hz
    );
    std::cout << "activate_result=" << activate_result << '\n';
    if (activate_result != ERROR_SUCCESS) {
      (void) apply_extended_topology();
    }
    const auto mode_ready = ensure_active_display_mode(
      created.value.os_adapter_luid,
      created.value.target_id,
      width,
      height,
      refresh_hz,
      feed_debug_lease
    );
    dump_active_paths_for_adapter(created.value.os_adapter_luid);
    const auto active_path = query_display_path(created.value.os_adapter_luid, created.value.target_id);
    if (active_path) {
      std::cout << "debug_active_path=1"
                << " width=" << active_path->width
                << " height=" << active_path->height
                << " refresh_millihz=" << active_path->refresh_millihz << '\n';
    } else {
      std::cout << "debug_active_path=0\n";
    }
    dump_display_config_paths(created.value.os_adapter_luid, created.value.target_id);

    const auto removed = client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
    if (!removed.ok()) {
      return fail("remove debug temporary display failed", removed);
    }
    if (!mode_ready || !active_path || !display_mode_matches(*active_path, width, height, refresh_hz)) {
      std::cerr << "debug display resolution mismatch: expected "
                << width << 'x' << height << '@' << refresh_hz
                << " got "
                << (active_path ? active_path->width : 0) << 'x'
                << (active_path ? active_path->height : 0) << '@'
                << (active_path ? active_path->refresh_millihz : 0) << "mHz\n";
      return 1;
    }
    return 0;
  }

  if (command == "--stress-capture-remove") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    std::uint32_t iterations {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t refresh_hz {};
    if (!read_u32_arg(argc, argv, 2, 10u, "iterations", iterations) ||
        !read_u32_arg(argc, argv, 3, 1280u, "width", width) ||
        !read_u32_arg(argc, argv, 4, 720u, "height", height) ||
        !read_u32_arg(argc, argv, 5, 60u, "refresh_hz", refresh_hz)) {
      return 2;
    }
    if (iterations == 0) {
      std::cerr << "iterations must be non-zero\n";
      return 2;
    }

    for (std::uint32_t iteration = 1; iteration <= iterations; ++iteration) {
      auto request = make_temporary_request(width, height, refresh_hz);
      request.requested_timeout_ms = 30'000;

      const auto created = client.create_temporary_display(request);
      if (!created.ok()) {
        return fail("stress create temporary display failed", created);
      }

      const vdd::LeaseRequest lease_request {
        vdd::kApiNamespaceGuid,
        request.lease_id,
        request.requested_timeout_ms,
        0
      };
      const auto cleanup_created = [&]() {
        return client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
      };
      const auto feed_stress_lease = [&]() {
        (void) client.feed_lease(lease_request);
      };

      feed_stress_lease();
      const auto activate_result = activate_target_path_result(
        created.value.os_adapter_luid,
        created.value.target_id,
        width,
        height,
        refresh_hz
      );
      if (activate_result != ERROR_SUCCESS) {
        (void) apply_extended_topology();
      }
      if (!ensure_active_display_mode(
            created.value.os_adapter_luid,
            created.value.target_id,
            width,
            height,
            refresh_hz,
            feed_stress_lease
          )) {
        (void) cleanup_created();
        std::cerr << "stress iteration " << iteration << " failed to activate display\n";
        return 1;
      }

      const auto source_id = active_source_id_for_target(created.value.os_adapter_luid, created.value.target_id);
      if (!source_id) {
        (void) cleanup_created();
        std::cerr << "stress iteration " << iteration << " could not find active source id\n";
        return 1;
      }
      const auto gdi_name = gdi_device_name_for_source(created.value.os_adapter_luid, *source_id);
      if (!gdi_name || gdi_name->empty()) {
        (void) cleanup_created();
        std::cerr << "stress iteration " << iteration << " could not resolve GDI display name\n";
        return 1;
      }

      std::atomic<bool> stop_capture {false};
      DxgiCaptureStats stats;
      std::thread capture_thread {
        [gdi_name = *gdi_name, &stop_capture, &stats]() {
          run_dxgi_duplication_capture(gdi_name, stop_capture, stats);
        }
      };

      if (!wait_for_capture_ready(stats, std::chrono::seconds(3))) {
        stop_capture.store(true, std::memory_order_release);
        if (capture_thread.joinable()) {
          capture_thread.join();
        }
        (void) cleanup_created();
        std::cerr << "stress iteration " << iteration
                  << " failed to start DXGI duplication capture"
                  << " result=" << hresult_hex(stats.last_result.load(std::memory_order_acquire))
                  << '\n';
        return 1;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      const auto removed = cleanup_created();
      if (!removed.ok()) {
        stop_capture.store(true, std::memory_order_release);
        if (capture_thread.joinable()) {
          capture_thread.join();
        }
        return fail("stress remove temporary display failed", removed);
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      stop_capture.store(true, std::memory_order_release);
      if (capture_thread.joinable()) {
        capture_thread.join();
      }

      const auto departed_path = wait_for_display_path(created.value.os_adapter_luid, created.value.target_id, false);
      if (departed_path) {
        std::cerr << "stress iteration " << iteration << " display path still present after removal\n";
        return 1;
      }

      std::cout << "stress_iteration=" << iteration
                << " display_id=" << created.value.display_id
                << " target_id=" << created.value.target_id
                << " frames=" << stats.frames.load(std::memory_order_acquire)
                << " timeouts=" << stats.timeouts.load(std::memory_order_acquire)
                << " errors=" << stats.errors.load(std::memory_order_acquire)
                << " last_result=" << hresult_hex(stats.last_result.load(std::memory_order_acquire))
                << '\n';
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    std::cout << "stress_capture_remove=1 iterations=" << iterations << '\n';
    return 0;
  }

  if (command == "--qa-temp-lease") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t refresh_hz {};
    std::uint32_t timeout_ms {};
    if (!read_u32_arg(argc, argv, 2, 1920u, "width", width) ||
        !read_u32_arg(argc, argv, 3, 1080u, "height", height) ||
        !read_u32_arg(argc, argv, 4, 60u, "refresh_hz", refresh_hz) ||
        !read_u32_arg(argc, argv, 5, 3'000u, "timeout_ms", timeout_ms)) {
      return 2;
    }
    auto request = make_temporary_request(width, height, refresh_hz);
    request.requested_timeout_ms = timeout_ms;

    const auto before_count = client.query_permanent_display_count();
    if (!before_count.ok()) {
      return fail("query permanent count failed", before_count);
    }

    const auto created = client.create_temporary_display(request);
    if (!created.ok()) {
      return fail("create QA temporary display failed", created);
    }

    const auto cleanup_created = [&]() {
      return client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
    };

    const vdd::LeaseRequest lease_request {
      vdd::kApiNamespaceGuid,
      request.lease_id,
      request.requested_timeout_ms,
      0
    };
    const auto feed_qa_lease = [&]() {
      (void) client.feed_lease(lease_request);
    };

    if (!activate_target_path(created.value.os_adapter_luid, created.value.target_id, width, height, refresh_hz)) {
      (void) apply_extended_topology();
    }
    if (!ensure_active_display_mode(created.value.os_adapter_luid, created.value.target_id, width, height, refresh_hz, feed_qa_lease)) {
      (void) cleanup_created();
      std::cerr << "QA display resolution mismatch after activation\n";
      return 1;
    }

    const auto queried = client.query_lease(lease_request);
    if (!queried.ok()) {
      (void) cleanup_created();
      return fail("query QA lease failed", queried);
    }
    if (queried.value.lease_exists == 0 || queried.value.temporary_display_count != 1) {
      (void) cleanup_created();
      std::cerr << "QA lease was not active immediately after create\n";
      return 1;
    }

    // This QA path intentionally uses very short leases. Keep the display alive
    // while Windows converges on topology/HDR, then stop feeding below to verify expiry.
    feed_qa_lease();
    const auto path = wait_for_display_path(created.value.os_adapter_luid, created.value.target_id, true, feed_qa_lease);
    if (!path || !path->active) {
      (void) cleanup_created();
      std::cerr << "created display did not appear in active Windows display paths\n";
      return 1;
    }
    if (path->width != width || path->height != height) {
      (void) cleanup_created();
      std::cerr << "created display resolution mismatch: expected "
                << width << 'x' << height << " got "
                << path->width << 'x' << path->height << '\n';
      return 1;
    }

    LONG advanced_color_error = ERROR_SUCCESS;
    const auto before_color = wait_for_advanced_color(
      created.value.os_adapter_luid,
      created.value.target_id,
      false,
      &advanced_color_error,
      feed_qa_lease
    );
    if (!before_color) {
      (void) cleanup_created();
      std::cerr << "advanced color query failed for QA target " << created.value.target_id
                << " native_error=" << advanced_color_error << '\n';
      return 1;
    }
    const bool hdr_set = set_hdr_state(created.value.os_adapter_luid, created.value.target_id, true);
    const bool advanced_color_set = set_advanced_color(created.value.os_adapter_luid, created.value.target_id, true);
    const auto after_color = wait_for_advanced_color(
      created.value.os_adapter_luid,
      created.value.target_id,
      true,
      &advanced_color_error,
      feed_qa_lease
    );
    if (!after_color) {
      (void) cleanup_created();
      std::cerr << "advanced color query did not return after QA HDR request\n";
      return 1;
    }

    print_advanced_color(*after_color);
    if (!after_color->v2 || !after_color->hdr_supported) {
      (void) cleanup_created();
      std::cerr << "QA display is not reported as HDR-supported by Windows\n";
      return 1;
    }
    if (!hdr_set && !advanced_color_set) {
      (void) cleanup_created();
      std::cerr << "Windows rejected both QA HDR and Advanced Color enable requests\n";
      return 1;
    }
    if (after_color->limited_by_policy ||
        !after_color->supported ||
        !after_color->hdr_enabled ||
        after_color->bits_per_color_channel < 10) {
      (void) cleanup_created();
      std::cerr << "QA display did not enter HDR 10-bit mode after request\n";
      return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(created.value.effective_timeout_ms + 2'000u));

    const auto expired = client.query_lease(lease_request);
    if (!expired.ok()) {
      (void) cleanup_created();
      return fail("query expired QA lease failed", expired);
    }
    if (expired.value.lease_exists != 0 || expired.value.temporary_display_count != 0) {
      (void) cleanup_created();
      std::cerr << "QA lease did not expire after " << created.value.effective_timeout_ms << " ms\n";
      return 1;
    }

    const auto departed_path = wait_for_display_path(created.value.os_adapter_luid, created.value.target_id, false);
    if (departed_path) {
      (void) cleanup_created();
      std::cerr << "expired QA display is still present in active Windows display paths\n";
      return 1;
    }

    const auto after_count = client.query_permanent_display_count();
    if (!after_count.ok()) {
      return fail("query permanent count after QA failed", after_count);
    }
    if (after_count.value.temporary_display_count != before_count.value.temporary_display_count) {
      std::cerr << "temporary count mismatch after QA lease expiry: before="
                << before_count.value.temporary_display_count
                << " after=" << after_count.value.temporary_display_count << '\n';
      return 1;
    }

    std::cout << "qa_temp_lease=1"
              << " display_id=" << created.value.display_id
              << " target_id=" << created.value.target_id
              << " connector_index=" << created.value.connector_index
              << " resolution=" << path->width << 'x' << path->height
              << " refresh_millihz=" << path->refresh_millihz
              << " effective_timeout_ms=" << created.value.effective_timeout_ms
              << " expired=1\n";
    return 0;
  }

  print_usage();
  return 2;
#endif
}
