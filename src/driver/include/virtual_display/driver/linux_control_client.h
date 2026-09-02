#pragma once

#if defined(__linux__)

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace virtual_display::driver {
  inline constexpr std::string_view kDefaultLinuxControlSocketPath = "/run/vibeshine/vkms-control.sock";

  enum class LinuxControlStatus {
    Success,
    InvalidRequest,
    TransportFailed,
    TimedOut,
    ProtocolError,
  };

  const char *to_string(LinuxControlStatus status);

  struct LinuxControlResult {
    LinuxControlStatus status {LinuxControlStatus::Success};
    std::optional<bool> connected;
    int native_error {};
    std::string detail;

    [[nodiscard]] bool ok() const {
      return status == LinuxControlStatus::Success;
    }
  };

  /** Client for the root-owned Linux virtual-display connector broker. */
  class LinuxControlClient {
  public:
    explicit LinuxControlClient(
      std::string socket_path = std::string {kDefaultLinuxControlSocketPath},
      // The socket-activated mutator has a five-second runtime limit plus a
      // one-second cgroup stop limit. Keep the client deadline strictly later
      // so a timed-out request can never continue mutating after this call
      // returns and releases the caller's shutdown fence.
      std::chrono::milliseconds timeout = std::chrono::seconds {8}
    );

    [[nodiscard]] LinuxControlResult query_connector(std::string_view output_name) const;
    [[nodiscard]] LinuxControlResult set_connector(std::string_view output_name, bool connected) const;

  private:
    [[nodiscard]] LinuxControlResult request(std::string_view verb, std::string_view output_name) const;

    std::string socket_path_;
    std::chrono::milliseconds timeout_;
  };
}  // namespace virtual_display::driver

#endif
