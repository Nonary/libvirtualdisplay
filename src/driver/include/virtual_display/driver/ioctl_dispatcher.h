#pragma once

#include "virtual_display/driver/driver_controller.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace virtual_display::driver {
  enum class IoctlStatus {
    Success,
    InvalidIoctl,
    InvalidInputBuffer,
    InvalidOutputBuffer,
    InvalidRequest,
    AlreadyExists,
    LimitReached,
    NotFound,
    BackendFailed,
  };

  struct IoctlDispatchResult {
    IoctlStatus status {IoctlStatus::Success};
    std::size_t bytes_returned {};
    ControllerStatus controller_status {};
  };

  class IoctlDispatcher {
  public:
    explicit IoctlDispatcher(DriverController &controller);

    IoctlDispatchResult dispatch(
      std::uint32_t ioctl_code,
      const void *input,
      std::size_t input_size,
      void *output,
      std::size_t output_size,
      std::chrono::steady_clock::time_point now,
      std::unique_lock<std::timed_mutex> *controller_lock = nullptr
    );

  private:
    DriverController &controller_;
  };

  const char *to_string(IoctlStatus status);
}  // namespace virtual_display::driver
