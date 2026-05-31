#include "virtual_display/driver/control_client.h"
#include "virtual_display/driver/windows_control_client.h"
#include "virtual_display/driver/windows_cli_utils.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <Aclapi.h>
#include <NewDev.h>
#include <SetupAPI.h>
#include <Shellapi.h>
#include <sddl.h>
#endif

namespace vdd = virtual_display::driver;

namespace {
  constexpr std::string_view kDriverInstallCommand {"driver"};
  constexpr std::string_view kDriverInstallSubcommand {"install"};

  void print_usage() {
    std::cout
      << "virtualdisplay commands:\n"
      << "  driver install [--inf PATH]\n"
      << "  broker install|start|stop|status|uninstall\n"
      << "  broker protocol|query-state|query-manifest|helper-diagnose|helper-apply-extended-topology|helper-apply-manifest-topology|helper-query-color-profiles\n"
      << "  broker helper-associate-color-profile <source_luid high:low> <source_id> <profile> [--advanced-color] [--default]\n"
      << "  status\n"
      << "  display query\n"
      << "  spawn [--width N] [--height N] [--physical-width-mm N] [--physical-height-mm N] [--refresh HZ] [--name TEXT]\n"
      << "  permanent query\n"
      << "  permanent set --count N [--width N] [--height N] [--physical-width-mm N] [--physical-height-mm N] [--refresh HZ] [--name TEXT]\n"
      << "  permanent profile set --slot N [--width N] [--height N] [--physical-width-mm N] [--physical-height-mm N] [--refresh HZ] [--name TEXT] [--layout none|apply|persist] [--x N] [--y N] [--hdr 0|1]\n"
      << "  permanent off\n";
  }

  int fail(const std::string &message, const vdd::ControlOperationResult &result) {
    std::cerr << message << ": " << vdd::to_string(result.status);
    if (result.native_error != 0) {
      std::cerr << " native_error=" << result.native_error;
    }
    std::cerr << '\n';
    return 1;
  }

#ifdef _WIN32
  constexpr wchar_t kBrokerPipeName[] = L"\\\\.\\pipe\\SunshineVirtualDisplayBroker";
  constexpr wchar_t kBrokerServiceName[] = L"SunshineVirtualDisplayBroker";
  constexpr wchar_t kBrokerServiceDisplayName[] = L"Sunshine Virtual Display Broker";
  constexpr wchar_t kBrokerServiceSecurityDescriptor[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
  constexpr wchar_t kBrokerServiceRequiredPrivileges[] =
    L"SeAssignPrimaryTokenPrivilege\0"
    L"SeIncreaseQuotaPrivilege\0"
    L"SeTcbPrivilege\0";
  constexpr DWORD kBrokerServiceInstallAccess {
    SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_QUERY_CONFIG | WRITE_DAC | DELETE
  };
  constexpr DWORD kBrokerServiceUpdateAccess {
    SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | WRITE_DAC
  };
  constexpr DWORD kBrokerServiceStateTimeoutMs {30'000};
  constexpr DWORD kBrokerServiceStatePollMs {250};
  constexpr DWORD kBrokerPipeIoTimeoutMs {30'000};
  constexpr DWORD kBrokerPipeCancelWaitMs {1'000};
  constexpr std::size_t kMaxBrokerResponseBytes {1024 * 1024};

  struct DevInfoSet {
    HDEVINFO value {INVALID_HANDLE_VALUE};

    explicit DevInfoSet(const HDEVINFO handle):
        value {handle} {
    }

    ~DevInfoSet() {
      if (value != INVALID_HANDLE_VALUE) {
        SetupDiDestroyDeviceInfoList(value);
      }
    }

    DevInfoSet(const DevInfoSet &) = delete;
    DevInfoSet &operator=(const DevInfoSet &) = delete;
  };

  struct ServiceHandle {
    SC_HANDLE value {};

    explicit ServiceHandle(const SC_HANDLE handle):
        value {handle} {
    }

    ~ServiceHandle() {
      if (value) {
        CloseServiceHandle(value);
      }
    }

    ServiceHandle(const ServiceHandle &) = delete;
    ServiceHandle &operator=(const ServiceHandle &) = delete;
  };

  struct LocalMemory {
    HLOCAL value {};

    explicit LocalMemory(const HLOCAL handle):
        value {handle} {
    }

    ~LocalMemory() {
      if (value) {
        LocalFree(value);
      }
    }

    void *get() const {
      return value;
    }

    LocalMemory(const LocalMemory &) = delete;
    LocalMemory &operator=(const LocalMemory &) = delete;
  };

  struct LocalHandle {
    HANDLE value {};

    explicit LocalHandle(const HANDLE handle):
        value {handle} {
    }

    ~LocalHandle() {
      if (value && value != INVALID_HANDLE_VALUE) {
        CloseHandle(value);
      }
    }

    HANDLE get() const {
      return value;
    }

    LocalHandle(const LocalHandle &) = delete;
    LocalHandle &operator=(const LocalHandle &) = delete;
  };

  struct BrokerResponse {
    bool pipe_available {};
    bool ok {};
    std::string text;
    std::uint32_t native_error {};
  };

  struct OverlappedRequest {
    LocalHandle event {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    OVERLAPPED value {};

    OverlappedRequest() {
      value.hEvent = event.get();
    }

    bool valid() const {
      return event.get() != nullptr;
    }
  };

  std::wstring widen(const std::string_view value) {
    if (value.empty()) {
      return {};
    }

    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
      return {};
    }

    const int input_size = static_cast<int>(value.size());
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, nullptr, 0);
    if (size <= 0) {
      return {};
    }

    std::wstring output(static_cast<std::size_t>(size), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, output.data(), size);
    if (written != size) {
      return {};
    }
    return output;
  }

  bool create_known_sid(const WELL_KNOWN_SID_TYPE type, std::vector<BYTE> &buffer) {
    DWORD sid_size = SECURITY_MAX_SID_SIZE;
    buffer.assign(sid_size, 0);
    if (!CreateWellKnownSid(type, nullptr, buffer.data(), &sid_size)) {
      return false;
    }
    buffer.resize(sid_size);
    return true;
  }

  bool is_unprivileged_sid(const PSID sid) {
    std::vector<BYTE> everyone_sid;
    std::vector<BYTE> authenticated_users_sid;
    std::vector<BYTE> builtin_users_sid;
    if (!create_known_sid(WinWorldSid, everyone_sid) ||
        !create_known_sid(WinAuthenticatedUserSid, authenticated_users_sid) ||
        !create_known_sid(WinBuiltinUsersSid, builtin_users_sid)) {
      return false;
    }

    return EqualSid(sid, everyone_sid.data()) ||
           EqualSid(sid, authenticated_users_sid.data()) ||
           EqualSid(sid, builtin_users_sid.data());
  }

  bool grants_file_replace_access(const ACCESS_MASK mask) {
    constexpr ACCESS_MASK kUnsafeAccess =
      GENERIC_ALL |
      GENERIC_WRITE |
      WRITE_DAC |
      WRITE_OWNER |
      DELETE |
      FILE_WRITE_DATA |
      FILE_APPEND_DATA |
      FILE_WRITE_EA |
      FILE_WRITE_ATTRIBUTES |
      FILE_ADD_FILE |
      FILE_ADD_SUBDIRECTORY |
      FILE_DELETE_CHILD;
    return (mask & kUnsafeAccess) != 0;
  }

  bool dacl_allows_unprivileged_write(const PACL dacl) {
    if (!dacl) {
      return true;
    }

    for (DWORD index = 0; index < dacl->AceCount; ++index) {
      void *ace_pointer {};
      if (!GetAce(dacl, index, &ace_pointer) || !ace_pointer) {
        return true;
      }

      const ACE_HEADER *header = static_cast<const ACE_HEADER *>(ace_pointer);
      if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
        continue;
      }

      const auto *allowed = static_cast<const ACCESS_ALLOWED_ACE *>(ace_pointer);
      const PSID sid = const_cast<PSID>(static_cast<const void *>(&allowed->SidStart));
      if (IsValidSid(sid) && is_unprivileged_sid(sid) && grants_file_replace_access(allowed->Mask)) {
        return true;
      }
    }

    return false;
  }

  bool path_allows_unprivileged_write(const std::filesystem::path &path) {
    PACL dacl {};
    PSECURITY_DESCRIPTOR descriptor {};
    const auto path_text = path.wstring();
    const DWORD result = GetNamedSecurityInfoW(
      const_cast<LPWSTR>(path_text.c_str()),
      SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION,
      nullptr,
      nullptr,
      &dacl,
      nullptr,
      &descriptor
    );
    if (result != ERROR_SUCCESS) {
      std::cerr << "inspect broker path security failed native_error=" << result << " path=" << path.string() << '\n';
      return true;
    }

    const LocalMemory security_descriptor {descriptor};
    return dacl_allows_unprivileged_write(dacl);
  }

  bool broker_executable_path_is_protected(const std::filesystem::path &broker_path) {
    const auto directory = broker_path.parent_path();
    if (directory.empty() || path_allows_unprivileged_write(directory) || path_allows_unprivileged_write(broker_path)) {
      std::cerr << "broker executable must be installed in a directory not writable by standard users: "
                << broker_path.string() << '\n';
      return false;
    }
    return true;
  }

  bool is_process_elevated() {
    HANDLE token {};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
      return false;
    }

    TOKEN_ELEVATION elevation {};
    DWORD returned {};
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
  }

  int relaunch_elevated(const std::vector<std::string> &args) {
    wchar_t executable[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (length == 0 || length >= std::size(executable)) {
      std::cerr << "failed to resolve executable path for elevation native_error=" << GetLastError() << '\n';
      return 1;
    }

    std::vector<std::wstring> wide_args;
    wide_args.reserve(args.size());
    for (const auto &arg : args) {
      wide_args.push_back(widen(arg));
    }
    const auto parameter_text = vdd::build_windows_command_parameters(wide_args);

    SHELLEXECUTEINFOW execute {};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.lpVerb = L"runas";
    execute.lpFile = executable;
    execute.lpParameters = parameter_text.c_str();
    execute.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&execute)) {
      std::cerr << "elevation failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    WaitForSingleObject(execute.hProcess, INFINITE);
    DWORD exit_code {};
    if (!GetExitCodeProcess(execute.hProcess, &exit_code)) {
      std::cerr << "elevated process finished, but exit code was unavailable native_error=" << GetLastError() << '\n';
      CloseHandle(execute.hProcess);
      return 1;
    }
    CloseHandle(execute.hProcess);
    return static_cast<int>(exit_code);
  }

  const char *service_state_name(const DWORD state) {
    switch (state) {
      case SERVICE_STOPPED:
        return "stopped";
      case SERVICE_START_PENDING:
        return "start_pending";
      case SERVICE_STOP_PENDING:
        return "stop_pending";
      case SERVICE_RUNNING:
        return "running";
      case SERVICE_CONTINUE_PENDING:
        return "continue_pending";
      case SERVICE_PAUSE_PENDING:
        return "pause_pending";
      case SERVICE_PAUSED:
        return "paused";
      default:
        return "unknown";
    }
  }

  std::filesystem::path broker_executable_path() {
    wchar_t executable[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (length == 0 || length >= std::size(executable)) {
      return {};
    }
    return std::filesystem::path {executable}.parent_path() / "virtualdisplay_broker.exe";
  }

  bool same_existing_path(const std::filesystem::path &lhs, const std::filesystem::path &rhs) {
    std::error_code error;
    const bool same = std::filesystem::equivalent(lhs, rhs, error);
    return !error && same;
  }

  std::optional<ULONG> broker_pipe_server_pid(HANDLE pipe) {
    ULONG server_pid {};
    if (!GetNamedPipeServerProcessId(pipe, &server_pid) || server_pid == 0) {
      return std::nullopt;
    }
    return server_pid;
  }

  bool broker_pipe_server_matches_service(const ULONG server_pid) {
    ServiceHandle manager {OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value) {
      return false;
    }

    ServiceHandle service {OpenServiceW(manager.value, kBrokerServiceName, SERVICE_QUERY_STATUS)};
    if (!service.value) {
      return false;
    }

    SERVICE_STATUS_PROCESS status {};
    DWORD bytes_needed {};
    if (!QueryServiceStatusEx(
          service.value,
          SC_STATUS_PROCESS_INFO,
          reinterpret_cast<LPBYTE>(&status),
          sizeof(status),
          &bytes_needed
        )) {
      return false;
    }

    return status.dwCurrentState == SERVICE_RUNNING &&
           status.dwProcessId != 0 &&
           status.dwProcessId == server_pid;
  }

  bool broker_pipe_server_has_expected_image(const ULONG server_pid) {
    LocalHandle process {
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server_pid)
    };
    if (!process.get()) {
      return false;
    }

    std::wstring server_path(MAX_PATH, L'\0');
    DWORD path_chars = static_cast<DWORD>(server_path.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, server_path.data(), &path_chars) || path_chars == 0) {
      return false;
    }
    server_path.resize(path_chars);

    const auto expected_path = broker_executable_path();
    return !expected_path.empty() && same_existing_path(std::filesystem::path {server_path}, expected_path);
  }

  bool verify_broker_pipe_server(HANDLE pipe) {
    const auto server_pid = broker_pipe_server_pid(pipe);
    if (!server_pid) {
      return false;
    }

    return broker_pipe_server_matches_service(*server_pid) &&
           broker_pipe_server_has_expected_image(*server_pid);
  }

  bool wait_for_broker_pipe_io(
    HANDLE pipe,
    OVERLAPPED &overlapped,
    DWORD &bytes_transferred,
    DWORD &native_error
  ) {
    const auto wait_result = WaitForSingleObject(overlapped.hEvent, kBrokerPipeIoTimeoutMs);
    if (wait_result == WAIT_TIMEOUT) {
      native_error = ERROR_TIMEOUT;
      CancelIoEx(pipe, &overlapped);
      (void) WaitForSingleObject(overlapped.hEvent, kBrokerPipeCancelWaitMs);
      return false;
    }
    if (wait_result != WAIT_OBJECT_0) {
      native_error = GetLastError();
      CancelIoEx(pipe, &overlapped);
      return false;
    }

    if (!GetOverlappedResult(pipe, &overlapped, &bytes_transferred, FALSE)) {
      native_error = GetLastError();
      return false;
    }

    return true;
  }

  bool write_broker_pipe(
    HANDLE pipe,
    const void *buffer,
    const DWORD requested,
    DWORD &bytes_written,
    DWORD &native_error
  ) {
    OverlappedRequest request;
    if (!request.valid()) {
      native_error = GetLastError();
      return false;
    }

    if (WriteFile(pipe, buffer, requested, &bytes_written, &request.value)) {
      return true;
    }

    const auto error = GetLastError();
    if (error != ERROR_IO_PENDING) {
      native_error = error;
      return false;
    }

    return wait_for_broker_pipe_io(pipe, request.value, bytes_written, native_error);
  }

  bool read_broker_pipe(
    HANDLE pipe,
    void *buffer,
    const DWORD buffer_size,
    DWORD &bytes_read,
    bool &more_data,
    DWORD &native_error
  ) {
    more_data = false;
    OverlappedRequest request;
    if (!request.valid()) {
      native_error = GetLastError();
      return false;
    }

    if (ReadFile(pipe, buffer, buffer_size, &bytes_read, &request.value)) {
      return true;
    }

    const auto error = GetLastError();
    if (error == ERROR_MORE_DATA) {
      more_data = true;
      return true;
    }
    if (error != ERROR_IO_PENDING) {
      native_error = error;
      return false;
    }

    if (!wait_for_broker_pipe_io(pipe, request.value, bytes_read, native_error)) {
      if (native_error == ERROR_MORE_DATA) {
        more_data = true;
        return true;
      }
      return false;
    }

    return true;
  }

  std::optional<SERVICE_STATUS_PROCESS> query_broker_service_status(
    SC_HANDLE service,
    std::uint32_t &native_error
  ) {
    SERVICE_STATUS_PROCESS status {};
    DWORD bytes_needed {};
    if (!QueryServiceStatusEx(
          service,
          SC_STATUS_PROCESS_INFO,
          reinterpret_cast<LPBYTE>(&status),
          sizeof(status),
          &bytes_needed
        )) {
      native_error = GetLastError();
      return std::nullopt;
    }
    native_error = ERROR_SUCCESS;
    return status;
  }

  int print_broker_service_status(SC_HANDLE service) {
    std::uint32_t native_error {};
    const auto status = query_broker_service_status(service, native_error);
    if (!status) {
      std::cerr << "query broker service failed native_error=" << native_error << '\n';
      return 1;
    }

    std::cout << "broker_service_state=" << service_state_name(status->dwCurrentState) << '\n';
    std::cout << "broker_service_state_code=" << status->dwCurrentState << '\n';
    return 0;
  }

  bool wait_for_broker_service_state(SC_HANDLE service, const DWORD target_state) {
    const ULONGLONG deadline = GetTickCount64() + kBrokerServiceStateTimeoutMs;
    std::uint32_t native_error {};
    std::optional<SERVICE_STATUS_PROCESS> last_status;

    do {
      last_status = query_broker_service_status(service, native_error);
      if (!last_status) {
        std::cerr << "query broker service failed native_error=" << native_error << '\n';
        return false;
      }
      if (last_status->dwCurrentState == target_state) {
        return true;
      }

      const ULONGLONG now = GetTickCount64();
      if (now >= deadline) {
        break;
      }
      Sleep(static_cast<DWORD>((std::min<ULONGLONG>)(kBrokerServiceStatePollMs, deadline - now)));
    } while (true);

    std::cerr
      << "broker service did not reach " << service_state_name(target_state)
      << " state current_state=" << service_state_name(last_status ? last_status->dwCurrentState : 0)
      << " current_state_code=" << (last_status ? last_status->dwCurrentState : 0)
      << '\n';
    return false;
  }

  bool harden_broker_service(SC_HANDLE service) {
    SERVICE_SID_INFO sid_info {};
    sid_info.dwServiceSidType = SERVICE_SID_TYPE_UNRESTRICTED;
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_SERVICE_SID_INFO, &sid_info)) {
      std::cerr << "configure broker service SID failed native_error=" << GetLastError() << '\n';
      return false;
    }

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kBrokerServiceSecurityDescriptor,
          SDDL_REVISION_1,
          &descriptor,
          nullptr
        )) {
      std::cerr << "build broker service security failed native_error=" << GetLastError() << '\n';
      return false;
    }

    const LocalMemory security_descriptor {descriptor};
    if (!SetServiceObjectSecurity(service, DACL_SECURITY_INFORMATION, security_descriptor.get())) {
      std::cerr << "apply broker service security failed native_error=" << GetLastError() << '\n';
      return false;
    }

    SERVICE_REQUIRED_PRIVILEGES_INFOW privileges {};
    privileges.pmszRequiredPrivileges = const_cast<LPWSTR>(kBrokerServiceRequiredPrivileges);
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &privileges)) {
      std::cerr << "configure broker service privileges failed native_error=" << GetLastError() << '\n';
      return false;
    }

    return true;
  }

  std::optional<std::wstring> query_service_binary_path(SC_HANDLE service) {
    DWORD bytes_needed {};
    if (QueryServiceConfigW(service, nullptr, 0, &bytes_needed) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      std::cerr << "query broker service config failed native_error=" << GetLastError() << '\n';
      return std::nullopt;
    }

    std::vector<BYTE> buffer(bytes_needed);
    auto *config = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(buffer.data());
    if (!QueryServiceConfigW(service, config, static_cast<DWORD>(buffer.size()), &bytes_needed)) {
      std::cerr << "query broker service config failed native_error=" << GetLastError() << '\n';
      return std::nullopt;
    }

    if (!config->lpBinaryPathName || config->lpBinaryPathName[0] == L'\0') {
      std::cerr << "broker service config is missing binary path\n";
      return std::nullopt;
    }

    return std::wstring {config->lpBinaryPathName};
  }

  void restore_service_binary_path(SC_HANDLE service, const std::wstring &previous_binary_path) {
    if (!ChangeServiceConfigW(
          service,
          SERVICE_NO_CHANGE,
          SERVICE_NO_CHANGE,
          SERVICE_NO_CHANGE,
          previous_binary_path.c_str(),
          nullptr,
          nullptr,
          nullptr,
          nullptr,
          nullptr,
          kBrokerServiceDisplayName
        )) {
      std::cerr << "restore broker service binary path failed native_error=" << GetLastError() << '\n';
    }
  }

  int install_broker_service(const std::vector<std::string> &args) {
    if (!is_process_elevated()) {
      return relaunch_elevated(args);
    }

    const auto broker_path = broker_executable_path();
    if (broker_path.empty() || !std::filesystem::exists(broker_path)) {
      std::cerr << "broker executable not found: " << broker_path.string() << '\n';
      return 1;
    }
    if (!broker_executable_path_is_protected(broker_path)) {
      return 1;
    }

    ServiceHandle manager {OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE)};
    if (!manager.value) {
      std::cerr << "open service manager failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    const std::wstring command_line = vdd::quote_windows_command_argument(broker_path.wstring()) + L" --service";
    SC_HANDLE service_handle = CreateServiceW(
      manager.value,
      kBrokerServiceName,
      kBrokerServiceDisplayName,
      kBrokerServiceInstallAccess,
      SERVICE_WIN32_OWN_PROCESS,
      SERVICE_AUTO_START,
      SERVICE_ERROR_NORMAL,
      command_line.c_str(),
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr
    );

    bool updated = false;
    if (!service_handle && GetLastError() == ERROR_SERVICE_EXISTS) {
      service_handle = OpenServiceW(manager.value, kBrokerServiceName, kBrokerServiceUpdateAccess);
      if (!service_handle) {
        std::cerr << "open broker service failed native_error=" << GetLastError() << '\n';
        return 1;
      }
      ServiceHandle service {service_handle};
      const auto previous_binary_path = query_service_binary_path(service.value);
      if (!previous_binary_path) {
        return 1;
      }
      if (!ChangeServiceConfigW(
            service.value,
            SERVICE_NO_CHANGE,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            command_line.c_str(),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            kBrokerServiceDisplayName
          )) {
        std::cerr << "update broker service failed native_error=" << GetLastError() << '\n';
        return 1;
      }
      if (!harden_broker_service(service.value)) {
        restore_service_binary_path(service.value, *previous_binary_path);
        return 1;
      }
      updated = true;
      std::cout << "broker_service_installed=1\n";
      std::cout << "broker_service_updated=1\n";
      return print_broker_service_status(service.value);
    }

    if (!service_handle) {
      std::cerr << "create broker service failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    ServiceHandle service {service_handle};
    if (!harden_broker_service(service.value)) {
      if (!DeleteService(service.value)) {
        std::cerr << "delete incomplete broker service failed native_error=" << GetLastError() << '\n';
      }
      return 1;
    }
    std::cout << "broker_service_installed=1\n";
    std::cout << "broker_service_updated=" << (updated ? 1 : 0) << '\n';
    return print_broker_service_status(service.value);
  }

  int start_broker_service(const std::vector<std::string> &args) {
    if (!is_process_elevated()) {
      return relaunch_elevated(args);
    }

    ServiceHandle manager {OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value) {
      std::cerr << "open service manager failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    ServiceHandle service {OpenServiceW(manager.value, kBrokerServiceName, SERVICE_QUERY_STATUS | SERVICE_START)};
    if (!service.value) {
      std::cerr << "open broker service failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    std::uint32_t native_error {};
    const auto before = query_broker_service_status(service.value, native_error);
    if (!before) {
      std::cerr << "query broker service failed native_error=" << native_error << '\n';
      return 1;
    }
    if (before->dwCurrentState != SERVICE_RUNNING && !StartServiceW(service.value, 0, nullptr)) {
      const auto error = GetLastError();
      if (error != ERROR_SERVICE_ALREADY_RUNNING) {
        std::cerr << "start broker service failed native_error=" << error << '\n';
        return 1;
      }
    }
    if (!wait_for_broker_service_state(service.value, SERVICE_RUNNING)) {
      return 1;
    }

    std::cout << "broker_service_started=1\n";
    return print_broker_service_status(service.value);
  }

  int stop_broker_service(const std::vector<std::string> &args) {
    if (!is_process_elevated()) {
      return relaunch_elevated(args);
    }

    ServiceHandle manager {OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value) {
      std::cerr << "open service manager failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    ServiceHandle service {OpenServiceW(manager.value, kBrokerServiceName, SERVICE_QUERY_STATUS | SERVICE_STOP)};
    if (!service.value) {
      std::cerr << "open broker service failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    std::uint32_t native_error {};
    const auto before = query_broker_service_status(service.value, native_error);
    if (!before) {
      std::cerr << "query broker service failed native_error=" << native_error << '\n';
      return 1;
    }
    if (before->dwCurrentState != SERVICE_STOPPED) {
      SERVICE_STATUS status {};
      if (!ControlService(service.value, SERVICE_CONTROL_STOP, &status)) {
        const auto error = GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE) {
          std::cerr << "stop broker service failed native_error=" << error << '\n';
          return 1;
        }
      }
    }
    if (!wait_for_broker_service_state(service.value, SERVICE_STOPPED)) {
      return 1;
    }

    std::cout << "broker_service_stopped=1\n";
    return print_broker_service_status(service.value);
  }

  int uninstall_broker_service(const std::vector<std::string> &args) {
    if (!is_process_elevated()) {
      return relaunch_elevated(args);
    }

    ServiceHandle manager {OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value) {
      std::cerr << "open service manager failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    ServiceHandle service {OpenServiceW(manager.value, kBrokerServiceName, DELETE | SERVICE_QUERY_STATUS | SERVICE_STOP)};
    if (!service.value) {
      std::cerr << "open broker service failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    std::uint32_t native_error {};
    const auto before = query_broker_service_status(service.value, native_error);
    if (before && before->dwCurrentState != SERVICE_STOPPED) {
      SERVICE_STATUS status {};
      if (!ControlService(service.value, SERVICE_CONTROL_STOP, &status)) {
        const auto error = GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE) {
          std::cerr << "stop broker service failed native_error=" << error << '\n';
          return 1;
        }
      }

      if (!wait_for_broker_service_state(service.value, SERVICE_STOPPED)) {
        return 1;
      }
    }

    if (!DeleteService(service.value)) {
      std::cerr << "delete broker service failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    std::cout << "broker_service_uninstalled=1\n";
    return 0;
  }

  int query_broker_service() {
    ServiceHandle manager {OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value) {
      std::cerr << "open service manager failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    ServiceHandle service {OpenServiceW(manager.value, kBrokerServiceName, SERVICE_QUERY_STATUS)};
    if (!service.value) {
      std::cerr << "open broker service failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    return print_broker_service_status(service.value);
  }

  std::filesystem::path default_driver_inf_path() {
    wchar_t executable[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (length == 0 || length >= std::size(executable)) {
      return {};
    }

    const auto tool_dir = std::filesystem::path {executable}.parent_path();
    const std::vector<std::filesystem::path> candidates {
      tool_dir.parent_path() / "driver" / "SunshineVirtualDisplayDriver.inf",
      tool_dir / "windows_driver" / "SunshineVirtualDisplayDriver.inf",
      tool_dir / "SunshineVirtualDisplayDriver.inf"
    };

    for (const auto &candidate : candidates) {
      if (std::filesystem::exists(candidate)) {
        return candidate;
      }
    }

    return candidates.front();
  }

  std::optional<std::filesystem::path> parse_driver_inf_path(const std::vector<std::string> &args) {
    const auto result = vdd::parse_driver_install_inf_path(args, default_driver_inf_path());
    switch (result.status) {
      case vdd::DriverInstallInfPathStatus::Ok:
        return result.inf_path;
      case vdd::DriverInstallInfPathStatus::UnknownOption:
        std::cerr << "unknown option: " << result.option << '\n';
        return std::nullopt;
      case vdd::DriverInstallInfPathStatus::MissingInfValue:
        std::cerr << "--inf requires a value\n";
        return std::nullopt;
      case vdd::DriverInstallInfPathStatus::InvalidInfPath:
        std::cerr << "invalid --inf path: " << result.option << '\n';
        return std::nullopt;
      case vdd::DriverInstallInfPathStatus::EmptyDefaultPath:
        break;
    }

      std::cerr << "failed to resolve default driver INF path\n";
    return std::nullopt;
  }

  bool multi_sz_contains(const std::byte *values, const DWORD byte_count, const std::wstring_view expected) {
    if (!values || byte_count == 0 || (byte_count % sizeof(wchar_t)) != 0) {
      return false;
    }

    const auto *wide_values = reinterpret_cast<const wchar_t *>(values);
    const std::size_t char_count = byte_count / sizeof(wchar_t);
    if (char_count < 2 || wide_values[char_count - 1] != L'\0' || wide_values[char_count - 2] != L'\0') {
      return false;
    }

    for (std::size_t offset = 0; offset < char_count && wide_values[offset] != L'\0';) {
      const auto remaining = char_count - offset;
      const auto *entry_begin = wide_values + offset;
      const auto *entry_end = std::find(entry_begin, wide_values + char_count, L'\0');
      if (entry_end == wide_values + char_count) {
        return false;
      }

      const std::wstring_view entry {entry_begin, static_cast<std::size_t>(entry_end - entry_begin)};
      if (entry == expected) {
        return true;
      }
      offset += entry.size() + 1;
      if (offset > char_count) {
        return false;
      }
    }
    return false;
  }

  bool root_device_exists(std::uint32_t &native_error) {
    DevInfoSet devices {SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES)};
    if (devices.value == INVALID_HANDLE_VALUE) {
      native_error = GetLastError();
      return false;
    }

    SP_DEVINFO_DATA device_info {};
    device_info.cbSize = sizeof(device_info);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devices.value, index, &device_info); ++index) {
      DWORD property_type {};
      DWORD required_size {};
      (void) SetupDiGetDeviceRegistryPropertyW(
        devices.value,
        &device_info,
        SPDRP_HARDWAREID,
        &property_type,
        nullptr,
        0,
        &required_size
      );

      if (required_size == 0 || property_type != REG_MULTI_SZ) {
        continue;
      }

      std::vector<std::byte> buffer(required_size);
      if (!SetupDiGetDeviceRegistryPropertyW(
            devices.value,
            &device_info,
            SPDRP_HARDWAREID,
            &property_type,
            reinterpret_cast<PBYTE>(buffer.data()),
            static_cast<DWORD>(buffer.size()),
            nullptr
          )) {
        continue;
      }

      if (multi_sz_contains(buffer.data(), required_size, L"Root\\SunshineVirtualDisplay")) {
        native_error = ERROR_SUCCESS;
        return true;
      }
    }

    native_error = GetLastError();
    if (native_error == ERROR_NO_MORE_ITEMS) {
      native_error = ERROR_FILE_NOT_FOUND;
    }
    return false;
  }

  bool create_root_device(std::uint32_t &native_error) {
    if (root_device_exists(native_error)) {
      native_error = ERROR_SUCCESS;
      return true;
    }

    if (native_error != ERROR_FILE_NOT_FOUND) {
      return false;
    }

    const GUID display_class_guid {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x02, 0xbe, 0x10, 0x31, 0x8}};
    DevInfoSet devices {SetupDiCreateDeviceInfoList(&display_class_guid, nullptr)};
    if (devices.value == INVALID_HANDLE_VALUE) {
      native_error = GetLastError();
      return false;
    }

    SP_DEVINFO_DATA device_info {};
    device_info.cbSize = sizeof(device_info);
    if (!SetupDiCreateDeviceInfoW(
          devices.value,
          L"SunshineVirtualDisplay",
          &display_class_guid,
          nullptr,
          nullptr,
          DICD_GENERATE_ID,
          &device_info
        )) {
      native_error = GetLastError();
      return false;
    }

    const wchar_t hardware_id[] = L"Root\\SunshineVirtualDisplay\0";
    if (!SetupDiSetDeviceRegistryPropertyW(
          devices.value,
          &device_info,
          SPDRP_HARDWAREID,
          reinterpret_cast<const BYTE *>(hardware_id),
          sizeof(hardware_id)
        )) {
      native_error = GetLastError();
      return false;
    }

    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devices.value, &device_info)) {
      native_error = GetLastError();
      return false;
    }

    native_error = ERROR_SUCCESS;
    return true;
  }

  int install_driver(const std::vector<std::string> &args) {
    if (!is_process_elevated()) {
      return relaunch_elevated(args);
    }

    const auto inf_path = parse_driver_inf_path(args);
    if (!inf_path) {
      return 2;
    }

    if (!std::filesystem::exists(*inf_path)) {
      std::cerr << "driver INF not found: " << inf_path->string() << '\n';
      return 1;
    }

    std::uint32_t native_error {};
    (void) create_root_device(native_error);
    if (native_error != ERROR_SUCCESS && native_error != ERROR_DEVINST_ALREADY_EXISTS) {
      std::cerr << "create root device failed native_error=" << native_error << '\n';
      return 1;
    }

    BOOL reboot_required = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(
          nullptr,
          L"Root\\SunshineVirtualDisplay",
          inf_path->wstring().c_str(),
          INSTALLFLAG_FORCE,
          &reboot_required
        )) {
      std::cerr << "driver install failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    std::cout << "driver_installed=1\n";
    std::cout << "reboot_required=" << (reboot_required ? 1 : 0) << '\n';
    return reboot_required ? 3 : 0;
  }

  BrokerResponse request_broker(const std::string_view command) {
    LocalHandle pipe {CreateFileW(
      kBrokerPipeName,
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
      nullptr
    )};
    if (pipe.get() == INVALID_HANDLE_VALUE) {
      return {false, false, {}, GetLastError()};
    }

    if (!verify_broker_pipe_server(pipe.get())) {
      const auto error = GetLastError();
      return {true, false, "error untrusted_broker_server\n", error == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : error};
    }

    if (command.size() > (std::numeric_limits<DWORD>::max)()) {
      return {true, false, "error command_too_large\n", ERROR_INVALID_PARAMETER};
    }

    DWORD bytes_written = 0;
    const auto requested = static_cast<DWORD>(command.size());
    DWORD native_error = ERROR_SUCCESS;
    if (!write_broker_pipe(pipe.get(), command.data(), requested, bytes_written, native_error) ||
        bytes_written != requested) {
      return {true, false, "error write_failed\n", native_error == ERROR_SUCCESS ? ERROR_WRITE_FAULT : native_error};
    }

    std::string text;
    std::array<char, 4096> response {};
    while (true) {
      DWORD bytes_read = 0;
      bool more_data = false;
      native_error = ERROR_SUCCESS;
      if (!read_broker_pipe(pipe.get(), response.data(), static_cast<DWORD>(response.size()), bytes_read, more_data, native_error)) {
        return {true, false, "error read_failed\n", native_error};
      }

      if (bytes_read != 0) {
        if (text.size() + bytes_read > kMaxBrokerResponseBytes) {
          return {true, false, "error response_too_large\n", ERROR_MORE_DATA};
        }
        text.append(response.data(), bytes_read);
      }

      if (!more_data) {
        break;
      }
    }

    return {true, text.starts_with("ok ") || text.starts_with("ok\n"), text, ERROR_SUCCESS};
  }

  int query_broker(const std::string_view command) {
    const auto response = request_broker(command);
    if (!response.pipe_available) {
      std::cerr << "open broker pipe failed native_error=" << response.native_error << '\n';
      return 1;
    }
    std::cout << response.text;
    return response.ok ? 0 : 1;
  }

  std::string broker_payload(const std::string &response) {
    if (response.starts_with("ok\n")) {
      return response.substr(3);
    }
    if (response.starts_with("ok ")) {
      return response.substr(3);
    }
    return response;
  }

  int require_broker_command(const std::string_view command, const bool print_payload) {
    const auto response = request_broker(command);
    if (!response.pipe_available) {
      std::cerr << "open broker pipe failed native_error=" << response.native_error << '\n';
      std::cerr << "start the broker service before using display management commands\n";
      return 1;
    }
    if (response.ok) {
      std::cout << (print_payload ? broker_payload(response.text) : response.text);
      return 0;
    }

    std::cerr << response.text;
    return 1;
  }
#else
  int install_driver(const std::vector<std::string> &) {
    std::cerr << "driver install is only supported on Windows\n";
    return 2;
  }
#endif

  template<class T>
  int fail(const std::string &message, const vdd::ControlResult<T> &result) {
    return fail(message, {result.status, result.native_error});
  }

  bool parse_u32(const std::string_view value, std::uint32_t &parsed) {
    std::uint32_t output {};
    const auto *begin = value.data();
    const auto *end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, output);
    if (result.ec != std::errc {} || result.ptr != end) {
      return false;
    }
    parsed = output;
    return true;
  }

  bool parse_i32(const std::string_view value, std::int32_t &parsed) {
    std::int32_t output {};
    const auto *begin = value.data();
    const auto *end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, output);
    if (result.ec != std::errc {} || result.ptr != end) {
      return false;
    }
    parsed = output;
    return true;
  }

  bool parse_refresh_millihz(const std::string_view value, std::uint32_t &parsed) {
    std::string text {value};
    if (text.empty() || std::any_of(text.begin(), text.end(), [](const unsigned char ch) {
          return std::isspace(ch) != 0;
        })) {
      return false;
    }

    errno = 0;
    char *end = nullptr;
    const double refresh = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || end != text.c_str() + text.size() || !std::isfinite(refresh) || refresh <= 0.0) {
      return false;
    }

    const double millihz = refresh <= 1000.0 ? refresh * 1000.0 : refresh;
    if (!std::isfinite(millihz) ||
        millihz < 1.0 ||
        millihz > static_cast<double>((std::numeric_limits<std::uint32_t>::max)())) {
      return false;
    }

    parsed = static_cast<std::uint32_t>(millihz);
    return true;
  }

  void set_display_name(char (&target)[vdd::kDisplayNameChars], const std::string &name) {
    std::fill(std::begin(target), std::end(target), '\0');
    std::memcpy(target, name.data(), (std::min)(name.size(), static_cast<std::size_t>(vdd::kDisplayNameChars - 1)));
  }

  bool is_safe_display_name(const std::string_view value) {
    if (value.empty() || value.size() >= vdd::kDisplayNameChars) {
      return false;
    }

    return std::none_of(value.begin(), value.end(), [](const unsigned char ch) {
      return ch < 0x20 || ch == 0x7f;
    });
  }

  std::string display_name(const char (&value)[vdd::kDisplayNameChars]) {
    return std::string {vdd::trim_display_name(value)};
  }

  std::string escape_key_value(const std::string_view value) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size());
    for (const unsigned char ch : value) {
      if (ch == '\\') {
        output += "\\\\";
      } else if (ch == '\n') {
        output += "\\n";
      } else if (ch == '\r') {
        output += "\\r";
      } else if (ch == '\t') {
        output += "\\t";
      } else if (ch < 0x20 || ch == 0x7f) {
        output += "\\x";
        output += kHex[ch >> 4];
        output += kHex[ch & 0x0f];
      } else {
        output += static_cast<char>(ch);
      }
    }
    return output;
  }

  std::string output_display_name(const char (&value)[vdd::kDisplayNameChars]) {
    return escape_key_value(display_name(value));
  }

  std::string guid_string(const vdd::Guid &guid) {
    char text[37] {};
    std::snprintf(
      text,
      sizeof(text),
      "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
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

  const char *display_kind(const std::uint32_t kind) {
    if (kind == vdd::kDisplayStateKindPermanent) {
      return "permanent";
    }
    if (kind == vdd::kDisplayStateKindTemporary) {
      return "temporary";
    }
    return "unknown";
  }

  void print_permanent_state(const vdd::PermanentDisplayCountResult &state) {
    std::cout
      << "permanent_displays=" << state.current_display_count << '\n'
      << "max_permanent_displays=" << state.max_display_count << '\n'
      << "temporary_displays=" << state.temporary_display_count << '\n'
      << "mode=" << state.width << 'x' << state.height << '@'
      << (state.refresh_rate_millihz / 1000.0) << "Hz\n"
      << "physical_size_mm=" << state.physical_width_mm << 'x' << state.physical_height_mm << '\n'
      << "name=" << output_display_name(state.display_name) << '\n';
  }

  void print_display_state(const vdd::QueryDisplayStateResult &state) {
    const auto entry_count = (std::min)(state.entry_count, vdd::kMaxDisplayStateEntries);
    std::cout
      << "permanent_displays=" << state.permanent_display_count << '\n'
      << "temporary_displays=" << state.temporary_display_count << '\n'
      << "display_entries=" << entry_count << '\n';

    if (state.entry_count != entry_count) {
      std::cout << "display_entries_reported=" << state.entry_count << '\n';
    }

    for (std::uint32_t index = 0; index < entry_count; ++index) {
      const auto &entry = state.entries[index];
      std::cout
        << "display." << index << ".kind=" << display_kind(entry.kind) << '\n'
        << "display." << index << ".display_id=" << entry.display_id << '\n'
        << "display." << index << ".lease_id=" << entry.lease_id << '\n'
        << "display." << index << ".connector_index=" << entry.connector_index << '\n'
        << "display." << index << ".container_id=" << guid_string(entry.container_id) << '\n'
        << "display." << index << ".product_code=" << entry.product_code << '\n'
        << "display." << index << ".serial_number=" << entry.serial_number << '\n'
        << "display." << index << ".mode=" << entry.width << 'x' << entry.height << '@'
        << (entry.refresh_rate_millihz / 1000.0) << "Hz\n"
        << "display." << index << ".physical_size_mm=" << entry.physical_width_mm << 'x'
        << entry.physical_height_mm << '\n'
        << "display." << index << ".hdr_supported="
        << ((entry.flags & vdd::kDisplayStateFlagHdrSupported) ? 1 : 0) << '\n'
        << "display." << index << ".retain_identity="
        << ((entry.flags & vdd::kDisplayStateFlagRetainIdentity) ? 1 : 0) << '\n'
        << "display." << index << ".name=" << output_display_name(entry.display_name) << '\n';
    }
  }

  struct PermanentOptions {
    std::uint32_t count {1};
    std::uint32_t width {1920};
    std::uint32_t height {1080};
    std::uint32_t physical_width_mm {vdd::kDefaultPhysicalWidthMillimeters};
    std::uint32_t physical_height_mm {vdd::kDefaultPhysicalHeightMillimeters};
    std::uint32_t refresh_rate_millihz {60'000};
    std::string name {"Sunshine Display"};
  };

  struct ProfileOptions: PermanentOptions {
    std::uint32_t slot {};
    std::uint32_t hdr_supported {1};
    std::uint32_t layout_policy {vdd::kDisplayManifestLayoutPolicyNone};
    std::int32_t position_x {};
    std::int32_t position_y {};
  };

#ifdef _WIN32
  std::string permanent_set_broker_command(const PermanentOptions &options) {
    std::ostringstream command;
    command
      << "permanent-set " << options.count
      << ' ' << options.width
      << ' ' << options.height
      << ' ' << options.physical_width_mm
      << ' ' << options.physical_height_mm
      << ' ' << options.refresh_rate_millihz
      << ' ' << options.name;
    return command.str();
  }

  std::string manifest_profile_set_broker_command(const ProfileOptions &options) {
    std::ostringstream command;
    command
      << "manifest-profile-set " << options.slot
      << ' ' << options.width
      << ' ' << options.height
      << ' ' << options.physical_width_mm
      << ' ' << options.physical_height_mm
      << ' ' << options.refresh_rate_millihz
      << ' ' << options.hdr_supported
      << ' ' << options.layout_policy
      << ' ' << options.position_x
      << ' ' << options.position_y
      << ' ' << options.name;
    return command.str();
  }

  std::optional<std::string> color_profile_association_broker_command(const std::vector<std::string> &args) {
    if (args.size() < 5) {
      return std::nullopt;
    }

    bool advanced_color = false;
    bool set_default = false;
    for (std::size_t index = 5; index < args.size(); ++index) {
      if (args[index] == "--advanced-color") {
        advanced_color = true;
      } else if (args[index] == "--default") {
        set_default = true;
      } else {
        return std::nullopt;
      }
    }

    std::ostringstream command;
    command
      << "helper-associate-color-profile "
      << args[2] << ' '
      << args[3] << ' '
      << (advanced_color ? "advanced" : "standard") << ' '
      << (set_default ? "default" : "nodefault") << ' '
      << args[4];
    return command.str();
  }
#endif

  std::optional<PermanentOptions> parse_permanent_options(
    const std::vector<std::string> &args,
    const std::size_t first,
    const bool require_count
  ) {
    PermanentOptions options {};
    bool saw_count = false;

    for (std::size_t index = first; index < args.size(); ++index) {
      const auto &arg = args[index];
      const auto need_value = [&]() -> std::optional<std::string> {
        if (index + 1 >= args.size()) {
          std::cerr << arg << " requires a value\n";
          return std::nullopt;
        }
        return args[++index];
      };

      if (arg == "--count") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.count)) {
          std::cerr << "invalid --count value\n";
          return std::nullopt;
        }
        saw_count = true;
      } else if (arg == "--width") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.width)) {
          std::cerr << "invalid --width value\n";
          return std::nullopt;
        }
      } else if (arg == "--height") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.height)) {
          std::cerr << "invalid --height value\n";
          return std::nullopt;
        }
      } else if (arg == "--physical-width-mm") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.physical_width_mm)) {
          std::cerr << "invalid --physical-width-mm value\n";
          return std::nullopt;
        }
      } else if (arg == "--physical-height-mm") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.physical_height_mm)) {
          std::cerr << "invalid --physical-height-mm value\n";
          return std::nullopt;
        }
      } else if (arg == "--refresh") {
        const auto value = need_value();
        if (!value || !parse_refresh_millihz(*value, options.refresh_rate_millihz)) {
          std::cerr << "invalid --refresh value\n";
          return std::nullopt;
        }
      } else if (arg == "--name") {
        const auto value = need_value();
        if (!value || !is_safe_display_name(*value)) {
          std::cerr << "invalid --name value\n";
          return std::nullopt;
        }
        options.name = *value;
      } else {
        std::cerr << "unknown option: " << arg << '\n';
        return std::nullopt;
      }
    }

    if (require_count && !saw_count) {
      std::cerr << "permanent set requires --count\n";
      return std::nullopt;
    }
    return options;
  }

  std::optional<ProfileOptions> parse_profile_options(
    const std::vector<std::string> &args,
    const std::size_t first
  ) {
    ProfileOptions options {};
    bool saw_slot = false;

    for (std::size_t index = first; index < args.size(); ++index) {
      const auto &arg = args[index];
      const auto need_value = [&]() -> std::optional<std::string> {
        if (index + 1 >= args.size()) {
          std::cerr << arg << " requires a value\n";
          return std::nullopt;
        }
        return args[++index];
      };

      if (arg == "--slot") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.slot)) {
          std::cerr << "invalid --slot value\n";
          return std::nullopt;
        }
        saw_slot = true;
      } else if (arg == "--width") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.width)) {
          std::cerr << "invalid --width value\n";
          return std::nullopt;
        }
      } else if (arg == "--height") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.height)) {
          std::cerr << "invalid --height value\n";
          return std::nullopt;
        }
      } else if (arg == "--physical-width-mm") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.physical_width_mm)) {
          std::cerr << "invalid --physical-width-mm value\n";
          return std::nullopt;
        }
      } else if (arg == "--physical-height-mm") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.physical_height_mm)) {
          std::cerr << "invalid --physical-height-mm value\n";
          return std::nullopt;
        }
      } else if (arg == "--refresh") {
        const auto value = need_value();
        if (!value || !parse_refresh_millihz(*value, options.refresh_rate_millihz)) {
          std::cerr << "invalid --refresh value\n";
          return std::nullopt;
        }
      } else if (arg == "--name") {
        const auto value = need_value();
        if (!value || !is_safe_display_name(*value)) {
          std::cerr << "invalid --name value\n";
          return std::nullopt;
        }
        options.name = *value;
      } else if (arg == "--layout") {
        const auto value = need_value();
        if (!value) {
          return std::nullopt;
        }
        if (*value == "none") {
          options.layout_policy = vdd::kDisplayManifestLayoutPolicyNone;
        } else if (*value == "apply") {
          options.layout_policy = vdd::kDisplayManifestLayoutPolicyApply;
        } else if (*value == "persist") {
          options.layout_policy = vdd::kDisplayManifestLayoutPolicyApplyAndPersist;
        } else {
          std::cerr << "invalid --layout value\n";
          return std::nullopt;
        }
      } else if (arg == "--x") {
        const auto value = need_value();
        if (!value || !parse_i32(*value, options.position_x)) {
          std::cerr << "invalid --x value\n";
          return std::nullopt;
        }
      } else if (arg == "--y") {
        const auto value = need_value();
        if (!value || !parse_i32(*value, options.position_y)) {
          std::cerr << "invalid --y value\n";
          return std::nullopt;
        }
      } else if (arg == "--hdr") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.hdr_supported) || options.hdr_supported > 1) {
          std::cerr << "invalid --hdr value\n";
          return std::nullopt;
        }
      } else {
        std::cerr << "unknown option: " << arg << '\n';
        return std::nullopt;
      }
    }

    if (!saw_slot) {
      std::cerr << "permanent profile set requires --slot\n";
      return std::nullopt;
    }
    return options;
  }

  vdd::PermanentDisplayCountRequest make_request(const PermanentOptions &options) {
    vdd::PermanentDisplayCountRequest request {};
    request.display_count = options.count;
    request.width = options.width;
    request.height = options.height;
    request.physical_width_mm = options.physical_width_mm;
    request.physical_height_mm = options.physical_height_mm;
    request.refresh_rate_millihz = options.refresh_rate_millihz;
    set_display_name(request.display_name, options.name);
    return request;
  }

  int set_permanent(vdd::ControlClient &client, const PermanentOptions &options) {
    auto request = make_request(options);
    const auto validation = vdd::validate_permanent_display_count(request, 64);
    if (validation != vdd::ValidationError::None && validation != vdd::ValidationError::PermanentDisplayCountTooHigh) {
      std::cerr << "invalid display settings: " << vdd::to_string(validation) << '\n';
      return 2;
    }

    const auto result = client.set_permanent_display_count(request);
    if (!result.ok()) {
      return fail("set permanent display failed", result);
    }

    print_permanent_state(result.value);
    return 0;
  }
}  // namespace

int main(int argc, char **argv) {
  if (argc < 2 || std::string_view {argv[1]} == "--help" || std::string_view {argv[1]} == "help") {
    print_usage();
    return argc < 2 ? 2 : 0;
  }

  const std::vector<std::string> args {argv + 1, argv + argc};
  if (args[0] == kDriverInstallCommand) {
    if (args.size() >= 2 && args[1] == kDriverInstallSubcommand) {
      return install_driver(args);
    }

    print_usage();
    return 2;
  }

  if (args[0] == "broker") {
    if (args.size() == 2 && args[1] == "install") {
#ifdef _WIN32
      return install_broker_service(args);
#else
      std::cerr << "broker service management is only supported on Windows.\n";
      return 1;
#endif
    }
    if (args.size() == 2 && args[1] == "start") {
#ifdef _WIN32
      return start_broker_service(args);
#else
      std::cerr << "broker service management is only supported on Windows.\n";
      return 1;
#endif
    }
    if (args.size() == 2 && args[1] == "stop") {
#ifdef _WIN32
      return stop_broker_service(args);
#else
      std::cerr << "broker service management is only supported on Windows.\n";
      return 1;
#endif
    }
    if (args.size() == 2 && args[1] == "status") {
#ifdef _WIN32
      return query_broker_service();
#else
      std::cerr << "broker service management is only supported on Windows.\n";
      return 1;
#endif
    }
    if (args.size() == 2 && args[1] == "uninstall") {
#ifdef _WIN32
      return uninstall_broker_service(args);
#else
      std::cerr << "broker service management is only supported on Windows.\n";
      return 1;
#endif
    }
    if (args.size() == 2 &&
        (args[1] == "protocol" ||
         args[1] == "query-state" ||
         args[1] == "query-manifest" ||
         args[1] == "helper-diagnose" ||
         args[1] == "helper-apply-extended-topology" ||
         args[1] == "helper-apply-manifest-topology" ||
         args[1] == "helper-query-color-profiles")) {
#ifdef _WIN32
      return query_broker(args[1]);
#else
      std::cerr << "broker queries are only supported on Windows.\n";
      return 1;
#endif
    }
    if (args.size() >= 5 && args[1] == "helper-associate-color-profile") {
#ifdef _WIN32
      const auto command = color_profile_association_broker_command(args);
      if (!command) {
        print_usage();
        return 2;
      }
      return query_broker(*command);
#else
      std::cerr << "broker queries are only supported on Windows.\n";
      return 1;
#endif
    }

    print_usage();
    return 2;
  }

#ifdef _WIN32
  if (args[0] == "status" && args.size() == 1) {
    return require_broker_command("permanent-query", true);
  }

  if (args[0] == "permanent" && args.size() == 2 && args[1] == "query") {
    return require_broker_command("permanent-query", true);
  }

  if (args[0] == "display" && args.size() == 2 && args[1] == "query") {
    return require_broker_command("display-query", true);
  }

  if (args[0] == "spawn") {
    const auto options = parse_permanent_options(args, 1, false);
    if (!options) {
      return 2;
    }
    return require_broker_command(permanent_set_broker_command(*options), true);
  }

  if (args[0] == "permanent" && args.size() == 2 && args[1] == "off") {
    PermanentOptions options {};
    options.count = 0;
    return require_broker_command(permanent_set_broker_command(options), true);
  }

  if (args[0] == "permanent" && args.size() >= 2 && args[1] == "set") {
    const auto options = parse_permanent_options(args, 2, true);
    if (!options) {
      return 2;
    }
    return require_broker_command(permanent_set_broker_command(*options), true);
  }

  if (args[0] == "permanent" && args.size() >= 3 && args[1] == "profile" && args[2] == "set") {
    const auto options = parse_profile_options(args, 3);
    if (!options) {
      return 2;
    }
    return require_broker_command(manifest_profile_set_broker_command(*options), true);
  }
#endif

  print_usage();
  return 2;
}
