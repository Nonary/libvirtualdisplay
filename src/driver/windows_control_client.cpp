#include "virtual_display/driver/windows_control_client.h"

#ifdef _WIN32

#include <SetupAPI.h>

#include <algorithm>
#include <limits>
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

    std::vector<std::wstring> enumerate_control_device_paths(std::uint32_t &native_error) {
      const GUID interface_guid = to_windows_guid(kDeviceInterfaceGuid);
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

      std::vector<std::wstring> paths;
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
        if (!SetupDiGetDeviceInterfaceDetailW(device_info_set.value, &interface_data, detail, detail_size, &detail_size, nullptr)) {
          native_error = GetLastError();
          continue;
        }

        paths.emplace_back(detail->DevicePath);
        native_error = ERROR_SUCCESS;
      }

      return paths;
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
    std::uint32_t enumerate_error = ERROR_SUCCESS;
    auto paths = enumerate_control_device_paths(enumerate_error);
    if (native_error) {
      *native_error = enumerate_error;
    }

    std::vector<WindowsControlDeviceInfo> devices;
    devices.reserve(paths.size());
    for (const auto &path : paths) {
      WindowsControlDeviceInfo info {};
      info.device_path = path;

      HANDLE handle = CreateFileW(
        path.c_str(),
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
    std::uint32_t last_error = ERROR_SUCCESS;
    const auto paths = enumerate_control_device_paths(last_error);

    for (const auto &path : paths) {
      UniqueHandle handle {
        CreateFileW(
          path.c_str(),
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
          path
        };
      }

      last_error = GetLastError();
    }

    if (last_error == ERROR_SUCCESS || last_error == ERROR_NO_MORE_ITEMS) {
      last_error = ERROR_FILE_NOT_FOUND;
    }
    return {ControlStatus::TransportFailed, {}, last_error, {}};
  }
}  // namespace virtual_display::driver

#endif
