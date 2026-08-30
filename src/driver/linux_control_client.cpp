#include "virtual_display/driver/linux_control_client.h"

#if defined(__linux__)

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace virtual_display::driver {
  namespace {
    bool valid_output_name(const std::string_view output_name) {
      return output_name == "Virtual-1" || output_name == "Virtual-2" ||
             output_name == "Virtual-3" || output_name == "Virtual-4";
    }

    enum class WaitResult {
      Ready,
      TimedOut,
      Failed,
    };

    WaitResult wait_for_fd(
      const int fd,
      const short events,
      const std::chrono::steady_clock::time_point deadline,
      int &native_error
    ) {
      while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()
        );
        if (remaining <= std::chrono::milliseconds::zero()) {
          return WaitResult::TimedOut;
        }

        pollfd descriptor {.fd = fd, .events = events};
        const auto result = poll(&descriptor, 1, static_cast<int>(remaining.count()));
        if (result > 0) {
          if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            native_error = EIO;
            return WaitResult::Failed;
          }
          // A socket-activated one-shot broker can close immediately after
          // writing its reply. Consume queued data when POLLIN and POLLHUP
          // arrive together.
          if ((descriptor.revents & events) != 0 ||
              (events == POLLIN && (descriptor.revents & POLLHUP) != 0)) {
            return WaitResult::Ready;
          }
          native_error = EIO;
          return WaitResult::Failed;
        }
        if (result == 0) {
          return WaitResult::TimedOut;
        }
        if (errno == EINTR) {
          continue;
        }
        native_error = errno;
        return WaitResult::Failed;
      }
    }

    LinuxControlResult wait_failure(const WaitResult result, const int native_error, std::string detail) {
      return {
        result == WaitResult::TimedOut ? LinuxControlStatus::TimedOut : LinuxControlStatus::TransportFailed,
        std::nullopt,
        native_error,
        std::move(detail),
      };
    }
  }  // namespace

  LinuxControlClient::LinuxControlClient(std::string socket_path, const std::chrono::milliseconds timeout):
      socket_path_ {std::move(socket_path)},
      timeout_ {timeout} {
  }

  LinuxControlResult LinuxControlClient::request(
    const std::string_view verb,
    const std::string_view output_name
  ) const {
    if ((verb != "connect" && verb != "disconnect" && verb != "status") ||
        !valid_output_name(output_name) || socket_path_.empty()) {
      return {LinuxControlStatus::InvalidRequest, std::nullopt, EINVAL, "invalid broker request"};
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
      return {LinuxControlStatus::TransportFailed, std::nullopt, errno, std::strerror(errno)};
    }
    struct CloseFd {
      int value;
      ~CloseFd() {
        close(value);
      }
    } close_fd {fd};

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(address.sun_path)) {
      return {LinuxControlStatus::InvalidRequest, std::nullopt, ENAMETOOLONG, "socket path is too long"};
    }
    std::memcpy(address.sun_path, socket_path_.data(), socket_path_.size());
    address.sun_path[socket_path_.size()] = '\0';

    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    if (connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
      if (errno != EINPROGRESS) {
        return {LinuxControlStatus::TransportFailed, std::nullopt, errno, std::strerror(errno)};
      }
      int native_error = 0;
      const auto waited = wait_for_fd(fd, POLLOUT, deadline, native_error);
      if (waited != WaitResult::Ready) {
        return wait_failure(waited, native_error, "broker connection did not become writable");
      }
      socklen_t native_error_size = sizeof(native_error);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &native_error, &native_error_size) != 0) {
        return {LinuxControlStatus::TransportFailed, std::nullopt, errno, std::strerror(errno)};
      }
      if (native_error != 0) {
        return {LinuxControlStatus::TransportFailed, std::nullopt, native_error, std::strerror(native_error)};
      }
    }

    const std::string request_text = std::string {verb} + " " + std::string {output_name} + "\n";
    std::size_t sent = 0;
    while (sent < request_text.size()) {
      int native_error = 0;
      const auto waited = wait_for_fd(fd, POLLOUT, deadline, native_error);
      if (waited != WaitResult::Ready) {
        return wait_failure(waited, native_error, "broker request could not be written");
      }
      const auto count = send(fd, request_text.data() + sent, request_text.size() - sent, MSG_NOSIGNAL);
      if (count > 0) {
        sent += static_cast<std::size_t>(count);
      } else if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      } else {
        return {LinuxControlStatus::TransportFailed, std::nullopt, errno, std::strerror(errno)};
      }
    }

    std::string response;
    response.reserve(96);
    while (response.size() <= 128) {
      int native_error = 0;
      const auto waited = wait_for_fd(fd, POLLIN, deadline, native_error);
      if (waited != WaitResult::Ready) {
        return wait_failure(waited, native_error, "broker response was not received");
      }
      char buffer[64];
      const auto count = recv(fd, buffer, sizeof(buffer), 0);
      if (count > 0) {
        response.append(buffer, static_cast<std::size_t>(count));
      } else if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      } else {
        return {LinuxControlStatus::TransportFailed, std::nullopt, count < 0 ? errno : ECONNRESET,
                count < 0 ? std::strerror(errno) : "broker closed without a complete response"};
      }

      const auto newline = response.find('\n');
      if (newline == std::string::npos) {
        continue;
      }
      if (newline + 1 != response.size() || response.find('\r') != std::string::npos) {
        return {LinuxControlStatus::ProtocolError, std::nullopt, EPROTO, "malformed broker response"};
      }
      response.resize(newline);

      const std::string connected_reply = "STATUS connected " + std::string {output_name};
      const std::string disconnected_reply = "STATUS disconnected " + std::string {output_name};
      const std::string connected_ok = "OK connected " + std::string {output_name};
      const std::string disconnected_ok = "OK disconnected " + std::string {output_name};
      if (response == connected_reply || response == connected_ok) {
        return {LinuxControlStatus::Success, true, 0, {}};
      }
      if (response == disconnected_reply || response == disconnected_ok) {
        return {LinuxControlStatus::Success, false, 0, {}};
      }
      return {LinuxControlStatus::ProtocolError, std::nullopt, EPROTO, std::move(response)};
    }

    return {LinuxControlStatus::ProtocolError, std::nullopt, EMSGSIZE, "broker response is too large"};
  }

  LinuxControlResult LinuxControlClient::query_connector(const std::string_view output_name) const {
    return request("status", output_name);
  }

  LinuxControlResult LinuxControlClient::set_connector(
    const std::string_view output_name,
    const bool connected
  ) const {
    // Mutations must always reach the broker. A status-only idempotence
    // shortcut cannot prove that this UID owns an already-connected output.
    const auto result = request(connected ? "connect" : "disconnect", output_name);
    if (!result.ok() || result.connected != connected) {
      return result.ok()
               ? LinuxControlResult {LinuxControlStatus::ProtocolError, result.connected, EPROTO,
                                     "broker acknowledged the wrong connector state"}
               : result;
    }
    return result;
  }

  const char *to_string(const LinuxControlStatus status) {
    switch (status) {
      case LinuxControlStatus::Success:
        return "success";
      case LinuxControlStatus::InvalidRequest:
        return "invalid_request";
      case LinuxControlStatus::TransportFailed:
        return "transport_failed";
      case LinuxControlStatus::TimedOut:
        return "timed_out";
      case LinuxControlStatus::ProtocolError:
        return "protocol_error";
    }
    return "unknown";
  }
}  // namespace virtual_display::driver

#endif
