#include "virtual_display/driver/control_client.h"
#include "virtual_display/driver/probe_commands.h"
#include "virtual_display/driver/device_identity.h"
#include "virtual_display/driver/windows_control_client.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
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
  #include <dxgi1_2.h>
  #include <Icm.h>
  #include <wrl/client.h>
  #include <windows.h>
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
      << "  --dump-display-config-current-session\n"
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
    const bool enabled
  ) {
    DisplayConfigSetHdrState state {};
    state.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE;
    state.header.size = sizeof(state);
    state.header.adapterId = vdd::to_windows_luid(adapter_luid);
    state.header.id = target_id;
    state.enable_hdr = enabled ? 1u : 0u;
    return DisplayConfigSetDeviceInfo(&state.header) == ERROR_SUCCESS;
  }

  bool set_advanced_color(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const bool enabled
  ) {
    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE state {};
    state.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    state.header.size = sizeof(state);
    state.header.adapterId = vdd::to_windows_luid(adapter_luid);
    state.header.id = target_id;
    state.enableAdvancedColor = enabled ? 1u : 0u;
    return DisplayConfigSetDeviceInfo(&state.header) == ERROR_SUCCESS;
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
                << " target_id=" << path.targetInfo.id << '\n';
      if (!before) {
        std::cerr << "current-session advanced-color query failed"
                  << " target_id=" << path.targetInfo.id
                  << " native_error=" << native_error << '\n';
        ++attempted;
        continue;
      }

      std::cout << "before_";
      print_advanced_color(*before);
      const bool set = before->v2 ?
        set_hdr_state(adapter_luid, path.targetInfo.id, enabled) :
        set_advanced_color(adapter_luid, path.targetInfo.id, enabled);
      if (!set) {
        std::cerr << "current-session HDR state change failed"
                  << " target_id=" << path.targetInfo.id
                  << " native_error=" << GetLastError() << '\n';
        ++attempted;
        continue;
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
  if (command == "--diagnose") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    return diagnose_control_devices();
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

  if (command == "--dump-display-config-current-session") {
    if (!require_command_arg_count(command, argc)) {
      return 2;
    }
    dump_display_config_paths();
    return 0;
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
  const bool remote_set = command == "--remote-set-permanent" || command == "--remote-set-hdr";
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
