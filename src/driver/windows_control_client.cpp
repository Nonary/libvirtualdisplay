#include "virtual_display/driver/windows_control_client.h"

#ifdef _WIN32

#include <SetupAPI.h>
#include <initguid.h>
#include <devpkey.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace virtual_display::driver {
  namespace {
    constexpr DWORD kControlIoTimeoutMs = 30'000;
    constexpr DWORD kControlIoCancelWaitMs = 1'000;

    struct DevInfoSet {
      HDEVINFO value {INVALID_HANDLE_VALUE};

      explicit DevInfoSet(HDEVINFO handle):
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

    struct UniqueHandle {
      HANDLE value {INVALID_HANDLE_VALUE};

      explicit UniqueHandle(HANDLE handle):
          value {handle} {
      }

      ~UniqueHandle() {
        if (value != INVALID_HANDLE_VALUE && value != nullptr) {
          CloseHandle(value);
        }
      }

      UniqueHandle(const UniqueHandle &) = delete;
      UniqueHandle &operator=(const UniqueHandle &) = delete;

      HANDLE release() {
        return std::exchange(value, INVALID_HANDLE_VALUE);
      }
    };

    HANDLE invalid_handle() {
      return INVALID_HANDLE_VALUE;
    }

    HANDLE invalid_event() {
      return nullptr;
    }

    struct ControlDevicePath {
      std::wstring path {};
      std::wstring instance_id {};
      std::optional<std::uint32_t> session_id {};
    };

    GUID control_interface_guid(const WindowsControlDeviceKind kind) {
      return to_windows_guid(
        kind == WindowsControlDeviceKind::RemoteSession ?
          kRemoteDeviceInterfaceGuid :
          kDeviceInterfaceGuid
      );
    }

    std::vector<ControlDevicePath> enumerate_control_device_paths(
      const WindowsControlDeviceKind kind,
      std::uint32_t &native_error
    ) {
      const GUID interface_guid = control_interface_guid(kind);
      DevInfoSet device_info_set {
        SetupDiGetClassDevsW(
          &interface_guid,
          nullptr,
          nullptr,
          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
        )
      };
      if (device_info_set.value == INVALID_HANDLE_VALUE) {
        native_error = GetLastError();
        return {};
      }

      std::vector<ControlDevicePath> paths;
      native_error = ERROR_FILE_NOT_FOUND;

      for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data {};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(device_info_set.value, nullptr, &interface_guid, index, &interface_data)) {
          const auto error = GetLastError();
          if (error == ERROR_NO_MORE_ITEMS) {
            native_error = paths.empty() ? ERROR_FILE_NOT_FOUND : ERROR_SUCCESS;
          } else {
            native_error = error;
          }
          break;
        }

        DWORD detail_size = 0;
        (void) SetupDiGetDeviceInterfaceDetailW(device_info_set.value, &interface_data, nullptr, 0, &detail_size, nullptr);
        if (detail_size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
          native_error = detail_size == 0 ? GetLastError() : ERROR_INVALID_DATA;
          continue;
        }

        std::vector<std::byte> detail_buffer(detail_size);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(detail_buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA device_info {};
        device_info.cbSize = sizeof(device_info);
        if (!SetupDiGetDeviceInterfaceDetailW(
              device_info_set.value,
              &interface_data,
              detail,
              detail_size,
              &detail_size,
              &device_info
            )) {
          native_error = GetLastError();
          continue;
        }

        ControlDevicePath device_path {};
        device_path.path = detail->DevicePath;

        DWORD instance_id_size = 0;
        (void) SetupDiGetDeviceInstanceIdW(
          device_info_set.value,
          &device_info,
          nullptr,
          0,
          &instance_id_size
        );
        if (instance_id_size > 0) {
          std::vector<wchar_t> instance_id(instance_id_size);
          if (SetupDiGetDeviceInstanceIdW(
                device_info_set.value,
                &device_info,
                instance_id.data(),
                instance_id_size,
                &instance_id_size
              )) {
            device_path.instance_id = instance_id.data();
          }
        }

        DEVPROPTYPE property_type = DEVPROP_TYPE_EMPTY;
        DWORD property_size = 0;
        std::uint32_t session_id = 0;
        if (SetupDiGetDevicePropertyW(
              device_info_set.value,
              &device_info,
              &DEVPKEY_Device_SessionId,
              &property_type,
              reinterpret_cast<PBYTE>(&session_id),
              sizeof(session_id),
              &property_size,
              0
            ) &&
            property_type == DEVPROP_TYPE_UINT32 &&
            property_size == sizeof(session_id)) {
          device_path.session_id = session_id;
        }

        paths.push_back(std::move(device_path));
        native_error = ERROR_SUCCESS;
      }

      return paths;
    }

    bool contains_case_insensitive(const std::wstring &value, const std::wstring &needle) {
      return std::search(
               value.begin(),
               value.end(),
               needle.begin(),
               needle.end(),
               [](const wchar_t left, const wchar_t right) {
                 return std::towupper(left) == std::towupper(right);
               }
             ) != value.end();
    }

    bool matches_remote_session(const ControlDevicePath &device, const std::uint32_t session_id) {
      if (device.session_id) {
        return *device.session_id == session_id;
      }

      // Older stacks may omit DEVPKEY_Device_SessionId. RemoteDisplayEnum also
      // embeds the session in the instance ID, so retain that as a fail-closed
      // compatibility fallback rather than opening an arbitrary seat.
      wchar_t marker[32] {};
      if (std::swprintf(marker, sizeof(marker) / sizeof(marker[0]), L"SESSIONID_%04X", session_id) < 0) {
        return false;
      }
      return contains_case_insensitive(device.instance_id, marker) ||
             contains_case_insensitive(device.path, marker);
    }

    WindowsControlOpenResult open_control_device_paths(
      const std::vector<ControlDevicePath> &devices,
      std::uint32_t last_error
    ) {
      for (const auto &device : devices) {
        UniqueHandle handle {
          CreateFileW(
            device.path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            nullptr
          )
        };
        if (handle.value != INVALID_HANDLE_VALUE) {
          auto transport = std::make_unique<WindowsControlTransport>(handle.release());
          return {
            ControlStatus::Success,
            std::move(transport),
            0,
            device.path
          };
        }

        last_error = GetLastError();
      }

      if (last_error == ERROR_SUCCESS || last_error == ERROR_NO_MORE_ITEMS) {
        last_error = ERROR_FILE_NOT_FOUND;
      }
      return {ControlStatus::TransportFailed, {}, last_error, {}};
    }
  }  // namespace

  WindowsControlTransport::WindowsControlTransport(HANDLE handle):
      handle_ {handle} {
  }

  WindowsControlTransport::~WindowsControlTransport() {
    if (valid()) {
      CloseHandle(handle_);
    }
  }

  WindowsControlTransport::WindowsControlTransport(WindowsControlTransport &&other) noexcept:
      handle_ {other.handle_} {
    other.handle_ = invalid_handle();
  }

  WindowsControlTransport &WindowsControlTransport::operator=(WindowsControlTransport &&other) noexcept {
    if (this != &other) {
      if (valid()) {
        CloseHandle(handle_);
      }
      handle_ = other.handle_;
      other.handle_ = invalid_handle();
    }
    return *this;
  }

  bool WindowsControlTransport::valid() const {
    return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
  }

  void WindowsControlTransport::cancel_pending_io() const {
    if (valid()) {
      (void) CancelIoEx(handle_, nullptr);
    }
  }

  bool WindowsControlTransport::ioctl(
    const std::uint32_t ioctl_code,
    const void *input,
    const std::size_t input_size,
    void *output,
    const std::size_t output_size,
    std::size_t &bytes_returned,
    std::uint32_t &native_error
  ) {
    bytes_returned = 0;
    native_error = 0;
    if (!valid() ||
        input_size > (std::numeric_limits<DWORD>::max)() ||
        output_size > (std::numeric_limits<DWORD>::max)()) {
      native_error = ERROR_INVALID_PARAMETER;
      return false;
    }

    UniqueHandle event {
      CreateEventW(nullptr, TRUE, FALSE, nullptr)
    };
    if (event.value == invalid_event()) {
      native_error = GetLastError();
      return false;
    }

    OVERLAPPED overlapped {};
    overlapped.hEvent = event.value;
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(
      handle_,
      static_cast<DWORD>(ioctl_code),
      const_cast<void *>(input),
      static_cast<DWORD>(input_size),
      output,
      static_cast<DWORD>(output_size),
      &returned,
      &overlapped
    );
    if (!ok) {
      const auto error = GetLastError();
      if (error != ERROR_IO_PENDING) {
        native_error = error;
        return false;
      }

      const auto wait_result = WaitForSingleObject(event.value, kControlIoTimeoutMs);
      if (wait_result == WAIT_TIMEOUT) {
        native_error = ERROR_TIMEOUT;
        CancelIoEx(handle_, &overlapped);
        (void) WaitForSingleObject(event.value, kControlIoCancelWaitMs);
        return false;
      }
      if (wait_result != WAIT_OBJECT_0) {
        native_error = GetLastError();
        CancelIoEx(handle_, &overlapped);
        return false;
      }
    }

    if (!GetOverlappedResult(handle_, &overlapped, &returned, FALSE)) {
      native_error = GetLastError();
      return false;
    }

    bytes_returned = returned;
    return true;
  }

  std::vector<WindowsControlDeviceInfo> enumerate_control_devices(std::uint32_t *native_error) {
    return enumerate_control_devices(WindowsControlDeviceKind::Console, native_error);
  }

  std::vector<WindowsControlDeviceInfo> enumerate_control_devices(
    const WindowsControlDeviceKind kind,
    std::uint32_t *native_error
  ) {
    std::uint32_t enumerate_error = ERROR_SUCCESS;
    auto paths = enumerate_control_device_paths(kind, enumerate_error);
    if (native_error) {
      *native_error = enumerate_error;
    }

    std::vector<WindowsControlDeviceInfo> devices;
    devices.reserve(paths.size());
    for (const auto &path : paths) {
      WindowsControlDeviceInfo info {};
      info.device_path = path.path;
      info.device_instance_id = path.instance_id;
      info.session_id = path.session_id;

      HANDLE handle = CreateFileW(
        path.path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
      );
      if (handle != INVALID_HANDLE_VALUE) {
        info.openable = true;
        CloseHandle(handle);
      } else {
        info.native_error = GetLastError();
      }
      devices.push_back(std::move(info));
    }

    return devices;
  }

  WindowsControlOpenResult open_first_control_device() {
    return open_first_control_device(WindowsControlDeviceKind::Console);
  }

  WindowsControlOpenResult open_first_control_device(const WindowsControlDeviceKind kind) {
    std::uint32_t last_error = ERROR_SUCCESS;
    auto devices = enumerate_control_device_paths(kind, last_error);
    return open_control_device_paths(devices, last_error);
  }

  WindowsControlOpenResult open_remote_control_device_for_session(const std::uint32_t session_id) {
    std::uint32_t last_error = ERROR_SUCCESS;
    auto devices = enumerate_control_device_paths(WindowsControlDeviceKind::RemoteSession, last_error);
    devices.erase(
      std::remove_if(
        devices.begin(),
        devices.end(),
        [session_id](const auto &device) {
          return !matches_remote_session(device, session_id);
        }
      ),
      devices.end()
    );
    // A terminal session is allowed exactly one control interface. Opening the
    // first matching path would let a stale/recreated device win a race and
    // would make the caller's session identity advisory. The broker/service
    // must re-enumerate and reject zero or multiple candidates instead.
    if (devices.size() != 1) {
      return {
        ControlStatus::TransportFailed,
        {},
        devices.empty() ? ERROR_FILE_NOT_FOUND : ERROR_INVALID_DATA,
        {}
      };
    }
    return open_control_device_paths(devices, last_error);
  }
}  // namespace virtual_display::driver

#endif
