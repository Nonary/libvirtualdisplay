#pragma once

#include "virtual_display/driver/control_client.h"
#include "virtual_display/driver/windows_control_protocol.h"

#ifdef _WIN32

#include <Windows.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace virtual_display::driver {
  enum class WindowsControlDeviceKind {
    Console,
    RemoteSession
  };

  class WindowsControlTransport final: public ControlTransport {
  public:
    explicit WindowsControlTransport(HANDLE handle);
    ~WindowsControlTransport() override;

    WindowsControlTransport(const WindowsControlTransport &) = delete;
    WindowsControlTransport &operator=(const WindowsControlTransport &) = delete;
    WindowsControlTransport(WindowsControlTransport &&other) noexcept;
    WindowsControlTransport &operator=(WindowsControlTransport &&other) noexcept;

    [[nodiscard]] bool valid() const;
    void cancel_pending_io() const;

    bool ioctl(
      std::uint32_t ioctl_code,
      const void *input,
      std::size_t input_size,
      void *output,
      std::size_t output_size,
      std::size_t &bytes_returned,
      std::uint32_t &native_error
    ) override;

  private:
    HANDLE handle_ {INVALID_HANDLE_VALUE};
  };

  struct WindowsControlOpenResult {
    ControlStatus status {ControlStatus::Success};
    std::unique_ptr<WindowsControlTransport> transport {};
    std::uint32_t native_error {};
    std::wstring device_path {};

    [[nodiscard]] bool ok() const {
      return status == ControlStatus::Success && transport && transport->valid();
    }
  };

  struct WindowsControlDeviceInfo {
    std::wstring device_path {};
    std::wstring device_instance_id {};
    std::optional<std::uint32_t> session_id {};
    bool openable {};
    std::uint32_t native_error {};
  };

  std::vector<WindowsControlDeviceInfo> enumerate_control_devices(std::uint32_t *native_error = nullptr);
  std::vector<WindowsControlDeviceInfo> enumerate_control_devices(
    WindowsControlDeviceKind kind,
    std::uint32_t *native_error = nullptr
  );
  WindowsControlOpenResult open_first_control_device();
  WindowsControlOpenResult open_first_control_device(WindowsControlDeviceKind kind);
  WindowsControlOpenResult open_remote_control_device_for_session(std::uint32_t session_id);
}  // namespace virtual_display::driver

#endif
