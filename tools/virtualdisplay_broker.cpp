#include "virtual_display/driver/control_client.h"
#include "virtual_display/driver/broker_commands.h"
#include "virtual_display/driver/device_identity.h"
#include "virtual_display/driver/display_identity.h"
#include "virtual_display/driver/lease_store.h"
#include "virtual_display/driver/windows_control_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <Windows.h>
  #include <sddl.h>
  #include <userenv.h>
  #include <wtsapi32.h>
#endif

namespace vdd = virtual_display::driver;

namespace {
  constexpr const wchar_t *kServiceName = vdd::kBrokerServiceNameW.data();
  constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\SunshineVirtualDisplayBroker";
  constexpr wchar_t kPipeSecurityDescriptor[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
  constexpr wchar_t kBrokerStateSubkey[] = L"SOFTWARE\\Sunshine\\VirtualDisplayBroker";
  constexpr wchar_t kBrokerDisplayManifestValue[] = L"DisplayManifest";
  constexpr wchar_t kBrokerRegistrySecurityDescriptor[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
  constexpr wchar_t kSessionHelperExecutable[] = L"virtualdisplay_probe.exe";
  constexpr DWORD kPipeBufferBytes = 4096;
  constexpr DWORD kPipeClientReadTimeoutMs = 5000;
  constexpr DWORD kPipeClientPollMs = 50;
  constexpr DWORD kHelperProcessTimeoutMs = 120000;
  constexpr DWORD kEventServiceStarting = 0x1000;
  constexpr DWORD kEventServiceRunning = 0x1001;
  constexpr DWORD kEventServiceStopping = 0x1002;
  constexpr DWORD kEventServiceStopped = 0x1003;
  constexpr DWORD kEventServiceFailed = 0x1004;
  constexpr DWORD kEventHelperStarting = 0x2000;
  constexpr DWORD kEventHelperFinished = 0x2001;

  void print_usage() {
    std::cout
      << "virtualdisplay_broker commands:\n"
      << "  --run-console\n"
      << "  --service\n";
  }

  std::string format_status(const vdd::ControlOperationResult &result) {
    std::string text {vdd::to_string(result.status)};
    if (result.native_error != 0) {
      text += " native_error=" + std::to_string(result.native_error);
    }
    return text;
  }

  template<class T>
  std::string format_status(const vdd::ControlResult<T> &result) {
    return format_status(vdd::ControlOperationResult {result.status, result.native_error});
  }

#ifdef _WIN32
  void report_event_log(WORD type, DWORD event_id, std::wstring_view text);

  class LocalMemory {
  public:
    explicit LocalMemory(void *value):
        value_ {value} {
    }

    ~LocalMemory() {
      if (value_) {
        LocalFree(value_);
      }
    }

    LocalMemory(const LocalMemory &) = delete;
    LocalMemory &operator=(const LocalMemory &) = delete;

    [[nodiscard]] void *get() const {
      return value_;
    }

  private:
    void *value_ {};
  };

  class ScopedHandle {
  public:
    ScopedHandle() = default;

    explicit ScopedHandle(HANDLE value):
        value_ {value} {
    }

    ~ScopedHandle() {
      reset();
    }

    ScopedHandle(const ScopedHandle &) = delete;
    ScopedHandle &operator=(const ScopedHandle &) = delete;

    [[nodiscard]] HANDLE get() const {
      return value_;
    }

    [[nodiscard]] HANDLE *put() {
      reset();
      return &value_;
    }

    [[nodiscard]] bool valid() const {
      return value_ && value_ != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE value = nullptr) {
      if (valid()) {
        CloseHandle(value_);
      }
      value_ = value;
    }

  private:
    HANDLE value_ {};
  };

  class EnvironmentBlock {
  public:
    explicit EnvironmentBlock(void *value):
        value_ {value} {
    }

    ~EnvironmentBlock() {
      if (value_) {
        DestroyEnvironmentBlock(value_);
      }
    }

    EnvironmentBlock(const EnvironmentBlock &) = delete;
    EnvironmentBlock &operator=(const EnvironmentBlock &) = delete;

    [[nodiscard]] void *get() const {
      return value_;
    }

  private:
    void *value_ {};
  };

  struct PipeSecurity {
    SECURITY_ATTRIBUTES attributes {};
    LocalMemory descriptor {nullptr};

    PipeSecurity(SECURITY_ATTRIBUTES security_attributes, void *security_descriptor):
        attributes {security_attributes},
        descriptor {security_descriptor} {
    }
  };

  std::optional<PipeSecurity> make_pipe_security() {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kPipeSecurityDescriptor,
          SDDL_REVISION_1,
          &descriptor,
          nullptr
        )) {
      return std::nullopt;
    }

    SECURITY_ATTRIBUTES attributes {};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    return std::optional<PipeSecurity> {std::in_place, attributes, descriptor};
  }

  bool token_is_member_of_well_known_sid(HANDLE token, const WELL_KNOWN_SID_TYPE sid_type) {
    std::array<BYTE, SECURITY_MAX_SID_SIZE> sid_buffer {};
    DWORD sid_size = static_cast<DWORD>(sid_buffer.size());
    if (!CreateWellKnownSid(sid_type, nullptr, sid_buffer.data(), &sid_size)) {
      return false;
    }

    BOOL member = FALSE;
    return CheckTokenMembership(token, sid_buffer.data(), &member) && member;
  }

  bool authorize_pipe_client(HANDLE pipe) {
    if (!ImpersonateNamedPipeClient(pipe)) {
      return false;
    }

    ScopedHandle token;
    const bool opened_token = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, token.put());
    const bool authorized = opened_token &&
      (token_is_member_of_well_known_sid(token.get(), WinLocalSystemSid) ||
       token_is_member_of_well_known_sid(token.get(), WinBuiltinAdministratorsSid));
    if (!RevertToSelf()) {
      report_event_log(EVENTLOG_ERROR_TYPE, kEventServiceFailed, L"RevertToSelf failed after pipe client impersonation");
      std::terminate();
    }
    return authorized;
  }

  class RegistryKey {
  public:
    RegistryKey() = default;

    ~RegistryKey() {
      reset();
    }

    RegistryKey(const RegistryKey &) = delete;
    RegistryKey &operator=(const RegistryKey &) = delete;

    RegistryKey(RegistryKey &&other) noexcept:
        key_ {other.key_} {
      other.key_ = nullptr;
    }

    RegistryKey &operator=(RegistryKey &&other) noexcept {
      if (this != &other) {
        reset(other.key_);
        other.key_ = nullptr;
      }
      return *this;
    }

    [[nodiscard]] HKEY get() const {
      return key_;
    }

    [[nodiscard]] HKEY *put() {
      reset();
      return &key_;
    }

    [[nodiscard]] bool valid() const {
      return key_ != nullptr;
    }

    void reset(HKEY value = nullptr) {
      if (key_) {
        RegCloseKey(key_);
      }
      key_ = value;
    }

  private:
    HKEY key_ {};
  };

  struct RegistrySecurity {
    SECURITY_ATTRIBUTES attributes {};
    LocalMemory descriptor {nullptr};

    RegistrySecurity(SECURITY_ATTRIBUTES security_attributes, void *security_descriptor):
        attributes {security_attributes},
        descriptor {security_descriptor} {
    }
  };

  std::optional<RegistrySecurity> make_registry_security() {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kBrokerRegistrySecurityDescriptor,
          SDDL_REVISION_1,
          &descriptor,
          nullptr
        )) {
      return std::nullopt;
    }

    SECURITY_ATTRIBUTES attributes {};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    return std::optional<RegistrySecurity> {std::in_place, attributes, descriptor};
  }

  std::optional<RegistryKey> open_broker_state_key(const REGSAM access) {
    RegistryKey key;
    const auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kBrokerStateSubkey, 0, access, key.put());
    if (status != ERROR_SUCCESS) {
      return std::nullopt;
    }
    return std::optional<RegistryKey> {std::move(key)};
  }

  std::optional<RegistryKey> create_broker_state_key(const REGSAM access) {
    auto security = make_registry_security();
    if (!security) {
      return std::nullopt;
    }

    RegistryKey key;
    DWORD disposition {};
    const auto status = RegCreateKeyExW(
      HKEY_LOCAL_MACHINE,
      kBrokerStateSubkey,
      0,
      nullptr,
      REG_OPTION_NON_VOLATILE,
      access | WRITE_DAC,
      &security->attributes,
      key.put(),
      &disposition
    );
    if (status != ERROR_SUCCESS) {
      return std::nullopt;
    }
    constexpr SECURITY_INFORMATION kRegistrySecurityInfo =
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION;
    if (RegSetKeySecurity(key.get(), kRegistrySecurityInfo, security->attributes.lpSecurityDescriptor) != ERROR_SUCCESS) {
      return std::nullopt;
    }
    return std::optional<RegistryKey> {std::move(key)};
  }

  std::optional<vdd::DisplayManifest> load_persisted_display_manifest(const std::uint32_t max_display_count) {
    auto key = open_broker_state_key(KEY_QUERY_VALUE);
    if (!key) {
      return std::nullopt;
    }

    vdd::DisplayManifest manifest {};
    DWORD type {};
    DWORD size = sizeof(manifest);
    const auto status = RegQueryValueExW(
      key->get(),
      kBrokerDisplayManifestValue,
      nullptr,
      &type,
      reinterpret_cast<BYTE *>(&manifest),
      &size
    );
    if (status != ERROR_SUCCESS ||
        type != REG_BINARY ||
        size != sizeof(manifest) ||
        vdd::validate_display_manifest(manifest, max_display_count) != vdd::ValidationError::None) {
      return std::nullopt;
    }

    return manifest;
  }

  bool save_display_manifest(const vdd::DisplayManifest &manifest, const std::uint32_t max_display_count) {
    if (vdd::validate_display_manifest(manifest, max_display_count) != vdd::ValidationError::None) {
      return false;
    }

    auto key = create_broker_state_key(KEY_SET_VALUE);
    if (!key) {
      return false;
    }

    const auto status = RegSetValueExW(
      key->get(),
      kBrokerDisplayManifestValue,
      0,
      REG_BINARY,
      reinterpret_cast<const BYTE *>(&manifest),
      sizeof(manifest)
    );
    return status == ERROR_SUCCESS;
  }

  struct BrokerContext {
    SERVICE_STATUS_HANDLE service_status_handle {};
    SERVICE_STATUS service_status {};
    std::mutex stop_event_mutex;
    HANDLE stop_event {};
    std::atomic<bool> stop_requested {false};
  };

  BrokerContext g_context {};

  ScopedHandle duplicate_stop_event() {
    std::lock_guard lock {g_context.stop_event_mutex};
    if (!g_context.stop_event) {
      return {};
    }

    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(
          GetCurrentProcess(),
          g_context.stop_event,
          GetCurrentProcess(),
          &duplicate,
          SYNCHRONIZE | EVENT_MODIFY_STATE,
          FALSE,
          0
        )) {
      return {};
    }
    return ScopedHandle {duplicate};
  }

  void set_stop_event_handle(HANDLE stop_event) {
    std::lock_guard lock {g_context.stop_event_mutex};
    if (g_context.stop_event) {
      CloseHandle(g_context.stop_event);
    }
    g_context.stop_event = stop_event;
  }

  void close_stop_event_handle() {
    std::lock_guard lock {g_context.stop_event_mutex};
    if (g_context.stop_event) {
      CloseHandle(g_context.stop_event);
      g_context.stop_event = nullptr;
    }
  }

  void signal_stop_event() {
    auto stop_event = duplicate_stop_event();
    if (stop_event.valid()) {
      SetEvent(stop_event.get());
    }
  }

  void report_event_log(const WORD type, const DWORD event_id, const std::wstring_view text) {
    HANDLE source = RegisterEventSourceW(nullptr, kServiceName);
    if (!source) {
      return;
    }

    const std::wstring message {text};
    LPCWSTR strings[] {message.c_str()};
    (void) ReportEventW(
      source,
      type,
      0,
      event_id,
      nullptr,
      static_cast<WORD>(std::size(strings)),
      0,
      strings,
      nullptr
    );
    DeregisterEventSource(source);
  }

  void report_event_log(const WORD type, const DWORD event_id, const std::string_view text) {
    report_event_log(type, event_id, vdd::widen_ascii(text));
  }

  void report_service_status(const DWORD state, const DWORD win32_exit_code = NO_ERROR) {
    if (!g_context.service_status_handle) {
      return;
    }

    g_context.service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_context.service_status.dwCurrentState = state;
    g_context.service_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
    g_context.service_status.dwWin32ExitCode = win32_exit_code;
    g_context.service_status.dwServiceSpecificExitCode = 0;
    g_context.service_status.dwCheckPoint = 0;
    g_context.service_status.dwWaitHint = 0;
    (void) SetServiceStatus(g_context.service_status_handle, &g_context.service_status);
  }

  std::wstring executable_directory() {
    wchar_t path[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(MAX_PATH));
    if (length == 0 || length >= MAX_PATH) {
      return {};
    }

    std::wstring value {path, length};
    const auto slash = value.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
      return {};
    }
    value.resize(slash);
    return value;
  }

  std::optional<DWORD> pipe_client_session_id(HANDLE pipe) {
    DWORD client_pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &client_pid)) {
      return std::nullopt;
    }

    DWORD session_id = 0;
    if (!ProcessIdToSessionId(client_pid, &session_id)) {
      return std::nullopt;
    }
    return session_id;
  }

  std::optional<DWORD> active_console_session_id() {
    const DWORD session_id = WTSGetActiveConsoleSessionId();
    if (session_id == 0xffffffffu) {
      return std::nullopt;
    }
    return session_id;
  }

  struct HelperLaunchResult {
    DWORD exit_code {ERROR_SUCCESS};
    std::string output;
  };

  std::string read_pipe_to_string(HANDLE pipe) {
    std::string output;
    std::array<char, 4096> buffer {};
    for (;;) {
      DWORD bytes_read = 0;
      if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr)) {
        const DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE) {
          break;
        }
        output += "helper_output_read_error=" + std::to_string(error) + "\n";
        break;
      }
      if (bytes_read == 0) {
        break;
      }
      output.append(buffer.data(), bytes_read);
    }
    return output;
  }

  HelperLaunchResult launch_session_helper(const DWORD session_id, const std::wstring &arguments) {
    if (session_id == 0xffffffffu) {
      return {ERROR_NO_TOKEN, {}};
    }

    ScopedHandle impersonation_token;
    if (!WTSQueryUserToken(session_id, impersonation_token.put())) {
      return {GetLastError(), {}};
    }

    ScopedHandle primary_token;
    if (!DuplicateTokenEx(
          impersonation_token.get(),
          TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
          nullptr,
          SecurityImpersonation,
          TokenPrimary,
          primary_token.put()
        )) {
      return {GetLastError(), {}};
    }

    void *environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, primary_token.get(), FALSE)) {
      return {GetLastError(), {}};
    }
    EnvironmentBlock environment_block {environment};

    const auto directory = executable_directory();
    if (directory.empty()) {
      return {ERROR_FILE_NOT_FOUND, {}};
    }

    const std::wstring helper_path = directory + L"\\" + kSessionHelperExecutable;
    std::wstring command_line = vdd::quote_broker_argument(helper_path);
    if (!arguments.empty()) {
      command_line += L" ";
      command_line += arguments;
    }

    SECURITY_ATTRIBUTES pipe_security {};
    pipe_security.nLength = sizeof(pipe_security);
    pipe_security.bInheritHandle = TRUE;
    ScopedHandle output_read;
    ScopedHandle output_write;
    if (!CreatePipe(output_read.put(), output_write.put(), &pipe_security, 0)) {
      return {GetLastError(), {}};
    }
    if (!SetHandleInformation(output_read.get(), HANDLE_FLAG_INHERIT, 0)) {
      return {GetLastError(), {}};
    }

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output_write.get();
    startup.hStdError = output_write.get();
    startup.hStdInput = nullptr;
    PROCESS_INFORMATION process {};
    if (!CreateProcessAsUserW(
          primary_token.get(),
          helper_path.c_str(),
          command_line.data(),
          nullptr,
          nullptr,
          FALSE,
          CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
          environment_block.get(),
          directory.c_str(),
          &startup,
          &process
        )) {
      return {GetLastError(), {}};
    }

    CloseHandle(process.hThread);
    output_write.reset();
    auto stop_event = duplicate_stop_event();
    HANDLE wait_handles[] {process.hProcess, stop_event.get()};
    const DWORD wait_result = stop_event.valid() ?
      WaitForMultipleObjects(2, wait_handles, FALSE, kHelperProcessTimeoutMs) :
      WaitForSingleObject(process.hProcess, kHelperProcessTimeoutMs);
    if (wait_result == WAIT_TIMEOUT || wait_result == WAIT_OBJECT_0 + 1) {
      TerminateProcess(process.hProcess, wait_result == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_CANCELLED);
      WaitForSingleObject(process.hProcess, 5000);
    } else if (wait_result == WAIT_FAILED) {
      const DWORD wait_error = GetLastError();
      CloseHandle(process.hProcess);
      return {wait_error, read_pipe_to_string(output_read.get())};
    }
    DWORD exit_code = ERROR_SUCCESS;
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
      exit_code = GetLastError();
    }
    const auto output = read_pipe_to_string(output_read.get());
    CloseHandle(process.hProcess);
    return {exit_code, output};
  }

  std::string handle_command(vdd::ControlClient &client, HANDLE pipe, const std::string_view command) {
    if (command == "protocol") {
      const auto result = client.query_protocol_version();
      if (!result.ok()) {
        return "error " + format_status(result) + "\n";
      }
      return "ok protocol=" + std::to_string(result.value.major) + "." +
             std::to_string(result.value.minor) + "." +
             std::to_string(result.value.patch) + "\n";
    }

    if (command == "query-state") {
      const auto result = client.query_display_state();
      if (!result.ok()) {
        return "error " + format_status(result) + "\n";
      }
      return "ok permanent=" + std::to_string(result.value.permanent_display_count) +
             " temporary=" + std::to_string(result.value.temporary_display_count) +
             " entries=" + std::to_string(result.value.entry_count) + "\n";
    }

    if (command == "display-query") {
      const auto result = client.query_display_state();
      if (!result.ok()) {
        return "error " + format_status(result) + "\n";
      }
      return "ok\n" + vdd::format_display_state(result.value);
    }

    if (command == "query-manifest") {
      const auto result = client.query_display_manifest();
      if (!result.ok()) {
        return "error " + format_status(result) + "\n";
      }
      return "ok\n" + vdd::format_display_manifest(result.value);
    }

    if (command == "permanent-query") {
      const auto result = client.query_permanent_display_count();
      if (!result.ok()) {
        return "error " + format_status(result) + "\n";
      }
      return "ok\n" + vdd::format_permanent_state(result.value);
    }

    if (command.starts_with("manifest-profile-set ")) {
      const auto profile = vdd::parse_manifest_profile_set_command(command);
      if (!profile) {
        return "error invalid_manifest_profile_set\n";
      }

      const auto current = client.query_permanent_display_count();
      if (!current.ok()) {
        return "error " + format_status(current) + "\n";
      }
      if (profile->connector_index >= current.value.max_display_count) {
        return "error invalid_connector_index\n";
      }

      auto manifest = client.query_display_manifest();
      if (!manifest.ok()) {
        return "error " + format_status(manifest) + "\n";
      }
      const auto original_manifest = manifest.value;
      manifest.value.max_profile_count = current.value.max_display_count;
      const auto bounded_profile_count =
        (std::min)(manifest.value.profile_count, vdd::kMaxPermanentDisplayProfiles);
      std::optional<std::uint32_t> profile_index;
      for (std::uint32_t index = 0; index < bounded_profile_count; ++index) {
        if (manifest.value.profiles[index].connector_index == profile->connector_index) {
          profile_index = index;
          break;
        }
      }
      if (!profile_index) {
        if (bounded_profile_count >= manifest.value.max_profile_count ||
            bounded_profile_count >= vdd::kMaxPermanentDisplayProfiles) {
          return "error manifest_full\n";
        }
        profile_index = bounded_profile_count;
        manifest.value.profile_count = bounded_profile_count + 1;
      }
      manifest.value.profiles[*profile_index] = *profile;

      if (!save_display_manifest(manifest.value, current.value.max_display_count)) {
        report_event_log(EVENTLOG_ERROR_TYPE, kEventServiceFailed, L"Persist display manifest profile failed");
        return "error persistence_failed\n";
      }
      const auto applied = client.set_display_manifest(manifest.value);
      if (!applied.ok()) {
        (void) save_display_manifest(original_manifest, current.value.max_display_count);
        return "error " + format_status(applied) + "\n";
      }
      return "ok\n" + vdd::format_display_manifest(applied.value);
    }

    if (command.starts_with("permanent-set ")) {
      const auto request = vdd::parse_permanent_set_command(command);
      if (!request) {
        return "error invalid_permanent_set\n";
      }

      const auto current = client.query_permanent_display_count();
      if (!current.ok()) {
        return "error " + format_status(current) + "\n";
      }

      const auto manifest = vdd::display_manifest_from_permanent_settings(*request, current.value.max_display_count);
      const auto applied = client.set_display_manifest(manifest);
      if (!applied.ok()) {
        return "error " + format_status(applied) + "\n";
      }
      if (!save_display_manifest(applied.value, current.value.max_display_count)) {
        report_event_log(EVENTLOG_ERROR_TYPE, kEventServiceFailed, L"Persist permanent display manifest failed");
        return "error persistence_failed\n";
      }

      const auto result = client.query_permanent_display_count();
      if (!result.ok()) {
        return "error " + format_status(result) + "\n";
      }
      return "ok\n" + vdd::format_permanent_state(result.value);
    }

    if (const auto helper_arguments = vdd::helper_arguments_for_broker_command(command)) {
      const auto session_id = pipe_client_session_id(pipe);
      if (!session_id) {
        return "error client_session_failed\n";
      }

      report_event_log(EVENTLOG_INFORMATION_TYPE, kEventHelperStarting, L"Starting client-session helper");
      auto helper_result = launch_session_helper(*session_id, *helper_arguments);
      if (helper_result.exit_code == ERROR_NO_TOKEN) {
        if (const auto console_session_id = active_console_session_id();
            console_session_id && *console_session_id != *session_id) {
          report_event_log(EVENTLOG_INFORMATION_TYPE, kEventHelperStarting, L"Retrying helper in active console session");
          helper_result = launch_session_helper(*console_session_id, *helper_arguments);
        }
      }
      if (helper_result.exit_code != ERROR_SUCCESS) {
        report_event_log(
          EVENTLOG_ERROR_TYPE,
          kEventHelperFinished,
          "Client-session helper failed result=" + std::to_string(helper_result.exit_code)
        );
        return "error helper_result=" + std::to_string(helper_result.exit_code) + "\n" + helper_result.output;
      }
      report_event_log(EVENTLOG_INFORMATION_TYPE, kEventHelperFinished, L"Client-session helper completed");
      return "ok helper_result=0\n" + helper_result.output;
    }

    return "error unknown_command\n";
  }

  bool wait_for_pipe_input(HANDLE pipe) {
    const auto deadline = GetTickCount64() + kPipeClientReadTimeoutMs;
    auto stop_event = duplicate_stop_event();
    while (!g_context.stop_requested.load(std::memory_order_acquire)) {
      DWORD available = 0;
      if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
        return false;
      }
      if (available > 0) {
        return true;
      }
      if (GetTickCount64() >= deadline) {
        SetLastError(ERROR_TIMEOUT);
        return false;
      }
      if (stop_event.valid()) {
        const DWORD wait_result = WaitForSingleObject(stop_event.get(), kPipeClientPollMs);
        if (wait_result == WAIT_OBJECT_0) {
          SetLastError(ERROR_CANCELLED);
          return false;
        }
      } else {
        Sleep(kPipeClientPollMs);
      }
    }
    SetLastError(ERROR_CANCELLED);
    return false;
  }

  void serve_pipe_client(HANDLE pipe, vdd::ControlClient &client) {
    char input[kPipeBufferBytes] {};
    DWORD bytes_read = 0;
    const BOOL read_ok =
      wait_for_pipe_input(pipe) &&
      ReadFile(pipe, input, sizeof(input) - 1, &bytes_read, nullptr);
    std::string response;
    if (!read_ok || bytes_read == 0) {
      response = "error read_failed\n";
    } else {
      input[(std::min<DWORD>)(bytes_read, sizeof(input) - 1)] = '\0';
      response = handle_command(client, pipe, vdd::trim_broker_command(input));
    }

    DWORD bytes_written = 0;
    (void) WriteFile(
      pipe,
      response.data(),
      static_cast<DWORD>((std::min<std::size_t>)(response.size(), (std::numeric_limits<DWORD>::max)())),
      &bytes_written,
      nullptr
    );
    FlushFileBuffers(pipe);
  }

  void restore_persisted_display_manifest(vdd::ControlClient &client) {
    const auto current = client.query_permanent_display_count();
    if (!current.ok()) {
      report_event_log(
        EVENTLOG_ERROR_TYPE,
        kEventServiceFailed,
        "Query permanent display state before restore failed " + format_status(current)
      );
      return;
    }

    const auto manifest = load_persisted_display_manifest(current.value.max_display_count);
    if (!manifest || manifest->profile_count == 0) {
      return;
    }

    constexpr DWORD kRestoreRetryDelayMs = 250;
    constexpr std::uint32_t kRestoreAttempts = 20;
    vdd::ControlResult<vdd::DisplayManifest> applied {};
    for (std::uint32_t attempt = 0; attempt < kRestoreAttempts; ++attempt) {
      applied = client.set_display_manifest(*manifest);
      if (applied.ok()) {
        report_event_log(EVENTLOG_INFORMATION_TYPE, kEventServiceRunning, L"Restored persisted display manifest");
        return;
      }
      if (g_context.stop_requested.load(std::memory_order_acquire)) {
        return;
      }
      Sleep(kRestoreRetryDelayMs);
    }

    report_event_log(
      EVENTLOG_ERROR_TYPE,
      kEventServiceFailed,
      "Restore persisted display manifest failed " + format_status(applied)
    );
  }

  int run_broker_loop() {
    auto opened = vdd::open_first_control_device();
    if (!opened.ok()) {
      report_event_log(
        EVENTLOG_ERROR_TYPE,
        kEventServiceFailed,
        "Open driver control device failed native_error=" + std::to_string(opened.native_error)
      );
      std::cerr << "open driver control device failed: "
                << vdd::to_string(opened.status)
                << " native_error=" << opened.native_error << '\n';
      return 1;
    }

    vdd::ControlClient client {*opened.transport};
    const auto protocol = client.query_protocol_version();
    if (!protocol.ok()) {
      report_event_log(EVENTLOG_ERROR_TYPE, kEventServiceFailed, "Driver protocol check failed " + format_status(protocol));
      std::cerr << "driver protocol check failed: " << format_status(protocol) << '\n';
      return 1;
    }

    restore_persisted_display_manifest(client);

    auto pipe_security = make_pipe_security();
    if (!pipe_security) {
      report_event_log(
        EVENTLOG_ERROR_TYPE,
        kEventServiceFailed,
        "Create pipe security failed native_error=" + std::to_string(GetLastError())
      );
      std::cerr << "create pipe security failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    while (!g_context.stop_requested.load(std::memory_order_acquire)) {
      const auto pipe_policy = vdd::broker_pipe_server_policy();
      const DWORD pipe_mode = static_cast<DWORD>(vdd::broker_pipe_mode(pipe_policy));
      HANDLE pipe = CreateNamedPipeW(
        kPipeName,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        pipe_mode,
        1,
        kPipeBufferBytes,
        kPipeBufferBytes,
        0,
        &pipe_security->attributes
      );
      if (pipe == INVALID_HANDLE_VALUE) {
        report_event_log(
          EVENTLOG_ERROR_TYPE,
          kEventServiceFailed,
          "Create broker pipe failed native_error=" + std::to_string(GetLastError())
        );
        std::cerr << "create broker pipe failed native_error=" << GetLastError() << '\n';
        return 1;
      }

      const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : GetLastError() == ERROR_PIPE_CONNECTED;
      if (connected && !g_context.stop_requested.load(std::memory_order_acquire)) {
        const bool client_authorized = !pipe_policy.require_client_authorization || authorize_pipe_client(pipe);
        if (vdd::broker_pipe_should_serve_client(pipe_policy, client_authorized)) {
          serve_pipe_client(pipe, client);
        } else {
          const auto response = vdd::broker_pipe_rejection_response(pipe_policy);
          DWORD bytes_written = 0;
          (void) WriteFile(
            pipe,
            response.data(),
            static_cast<DWORD>(response.size()),
            &bytes_written,
            nullptr
          );
          FlushFileBuffers(pipe);
        }
      }

      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
    }

    return 0;
  }

  void WINAPI service_control_handler(const DWORD control_code) {
    if (control_code != SERVICE_CONTROL_STOP) {
      return;
    }

    report_service_status(SERVICE_STOP_PENDING);
    report_event_log(EVENTLOG_INFORMATION_TYPE, kEventServiceStopping, L"Broker service stop requested");
    g_context.stop_requested.store(true, std::memory_order_release);
    signal_stop_event();

    HANDLE client = CreateFileW(
      kPipeName,
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
      nullptr
    );
    if (client != INVALID_HANDLE_VALUE) {
      CloseHandle(client);
    }
  }

  void WINAPI service_main(DWORD, LPWSTR *) {
    g_context.service_status_handle = RegisterServiceCtrlHandlerW(kServiceName, service_control_handler);
    if (!g_context.service_status_handle) {
      return;
    }

    set_stop_event_handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    report_service_status(SERVICE_START_PENDING);
    report_event_log(EVENTLOG_INFORMATION_TYPE, kEventServiceStarting, L"Broker service starting");
    report_service_status(SERVICE_RUNNING);
    report_event_log(EVENTLOG_INFORMATION_TYPE, kEventServiceRunning, L"Broker service running");
    const int result = run_broker_loop();
    close_stop_event_handle();
    report_event_log(
      result == 0 ? EVENTLOG_INFORMATION_TYPE : EVENTLOG_ERROR_TYPE,
      result == 0 ? kEventServiceStopped : kEventServiceFailed,
      result == 0 ? L"Broker service stopped" : L"Broker service stopped after failure"
    );
    report_service_status(SERVICE_STOPPED, result == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
  }

  int run_service() {
    SERVICE_TABLE_ENTRYW service_table[] {
      {const_cast<LPWSTR>(kServiceName), service_main},
      {nullptr, nullptr}
    };
    if (!StartServiceCtrlDispatcherW(service_table)) {
      std::cerr << "start service dispatcher failed native_error=" << GetLastError() << '\n';
      return 1;
    }
    return 0;
  }
#endif
}  // namespace

int main(const int argc, char **argv) {
#ifndef _WIN32
  (void) argc;
  (void) argv;
  std::cerr << "virtualdisplay_broker is only supported on Windows.\n";
  return 1;
#else
  if (argc < 2) {
    print_usage();
    return 2;
  }

  const std::string_view command {argv[1]};
  if (command == "--run-console") {
    return run_broker_loop();
  }
  if (command == "--service") {
    return run_service();
  }

  print_usage();
  return 2;
#endif
}
