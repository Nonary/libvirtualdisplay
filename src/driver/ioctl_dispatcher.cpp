#include "virtual_display/driver/ioctl_dispatcher.h"

#include <cstring>

namespace virtual_display::driver {
  namespace {
    template<class T>
    bool read_input(const void *input, const std::size_t input_size, T &value) {
      if (!input || input_size < sizeof(T)) {
        return false;
      }

      std::memcpy(&value, input, sizeof(T));
      return true;
    }

    template<class T>
    bool write_output(void *output, const std::size_t output_size, const T &value, std::size_t &bytes_returned) {
      if (!output || output_size < sizeof(T)) {
        return false;
      }

      std::memcpy(output, &value, sizeof(T));
      bytes_returned = sizeof(T);
      return true;
    }

    IoctlStatus status_from_controller(const ControllerStatus &status) {
      if (status.backend_error != BackendError::None) {
        return IoctlStatus::BackendFailed;
      }

      if (status.validation_error == ValidationError::PermanentDisplayCountTooHigh) {
        return IoctlStatus::LimitReached;
      }

      if (status.validation_error != ValidationError::None) {
        return IoctlStatus::InvalidRequest;
      }

      switch (status.store_error) {
        case StoreError::None:
          return IoctlStatus::Success;
        case StoreError::ValidationFailed:
          return IoctlStatus::InvalidRequest;
        case StoreError::TemporaryDisplayLimitReached:
        case StoreError::PermanentDisplayCountTooHigh:
          return IoctlStatus::LimitReached;
        case StoreError::DisplayAlreadyExists:
        case StoreError::DuplicateDisplayIdentity:
          return IoctlStatus::AlreadyExists;
        case StoreError::LeaseNotFound:
        case StoreError::DisplayNotFound:
          return IoctlStatus::NotFound;
      }

      return IoctlStatus::InvalidRequest;
    }

    IoctlDispatchResult result_from_controller(const ControllerStatus &status) {
      return {status_from_controller(status), 0, status};
    }
  }  // namespace

  IoctlDispatcher::IoctlDispatcher(DriverController &controller):
      controller_ {controller} {
  }

  IoctlDispatchResult IoctlDispatcher::dispatch(
    const std::uint32_t ioctl_code,
    const void *input,
    const std::size_t input_size,
    void *output,
    const std::size_t output_size,
    const std::chrono::steady_clock::time_point now
  ) {
    std::size_t bytes_returned = 0;

    switch (ioctl_code) {
      case kIoctlGetProtocolVersion: {
        ProtocolVersion version {};
        if (!write_output(output, output_size, version, bytes_returned)) {
          return {IoctlStatus::InvalidOutputBuffer, 0, {}};
        }

        return {IoctlStatus::Success, bytes_returned, {}};
      }

      case kIoctlCreateTemporaryDisplay: {
        CreateTemporaryDisplayRequest request {};
        if (!read_input(input, input_size, request)) {
          return {IoctlStatus::InvalidInputBuffer, 0, {}};
        }

        if (!output || output_size < sizeof(CreateTemporaryDisplayResult)) {
          return {IoctlStatus::InvalidOutputBuffer, 0, {}};
        }

        auto created = controller_.create_temporary_display(request, now);
        if (!created.status.ok()) {
          return result_from_controller(created.status);
        }

        (void) write_output(output, output_size, created.result, bytes_returned);

        return {IoctlStatus::Success, bytes_returned, {}};
      }

      case kIoctlRemoveTemporaryDisplay: {
        LeaseDisplayRequest request {};
        if (!read_input(input, input_size, request)) {
          return {IoctlStatus::InvalidInputBuffer, 0, {}};
        }

        return result_from_controller(controller_.remove_temporary_display(request));
      }

      case kIoctlFeedLease: {
        LeaseRequest request {};
        if (!read_input(input, input_size, request)) {
          return {IoctlStatus::InvalidInputBuffer, 0, {}};
        }

        return result_from_controller(controller_.feed_lease(request, now));
      }

      case kIoctlReleaseLease: {
        LeaseRequest request {};
        if (!read_input(input, input_size, request)) {
          return {IoctlStatus::InvalidInputBuffer, 0, {}};
        }

        return result_from_controller(controller_.release_lease(request));
      }

      case kIoctlQueryLease: {
        LeaseRequest request {};
        if (!read_input(input, input_size, request)) {
          return {IoctlStatus::InvalidInputBuffer, 0, {}};
        }

        if (const auto validation = validate_lease_request(request);
            validation != ValidationError::None) {
          return {IoctlStatus::InvalidRequest, 0, {StoreError::ValidationFailed, validation, BackendError::None}};
        }

        const auto query = controller_.query_lease(request.lease_id, now);
        if (!write_output(output, output_size, query, bytes_returned)) {
          return {IoctlStatus::InvalidOutputBuffer, 0, {}};
        }

        return {IoctlStatus::Success, bytes_returned, {}};
      }

      case kIoctlSetPermanentDisplayCount: {
        PermanentDisplayCountRequest request {};
        if (!read_input(input, input_size, request)) {
          return {IoctlStatus::InvalidInputBuffer, 0, {}};
        }

        if (!output || output_size < sizeof(PermanentDisplayCountResult)) {
          return {IoctlStatus::InvalidOutputBuffer, 0, {}};
        }

        const auto status = controller_.set_permanent_display_count(request);
        if (!status.ok()) {
          return result_from_controller(status);
        }

        const auto result = controller_.query_permanent_display_count();
        (void) write_output(output, output_size, result, bytes_returned);

        return {IoctlStatus::Success, bytes_returned, {}};
      }

      case kIoctlQueryPermanentDisplayCount: {
        const auto result = controller_.query_permanent_display_count();
        if (!write_output(output, output_size, result, bytes_returned)) {
          return {IoctlStatus::InvalidOutputBuffer, 0, {}};
        }

        return {IoctlStatus::Success, bytes_returned, {}};
      }

      case kIoctlQueryDisplayState: {
        const auto result = controller_.query_display_state();
        if (!write_output(output, output_size, result, bytes_returned)) {
          return {IoctlStatus::InvalidOutputBuffer, 0, {}};
        }

        return {IoctlStatus::Success, bytes_returned, {}};
      }

      case kIoctlSetDisplayManifest: {
        DisplayManifest manifest {};
        if (!read_input(input, input_size, manifest)) {
          return {IoctlStatus::InvalidInputBuffer, 0, {}};
        }

        if (!output || output_size < sizeof(DisplayManifest)) {
          return {IoctlStatus::InvalidOutputBuffer, 0, {}};
        }

        const auto status = controller_.apply_display_manifest(manifest);
        if (!status.ok()) {
          return result_from_controller(status);
        }

        (void) write_output(output, output_size, controller_.query_display_manifest(), bytes_returned);

        return {IoctlStatus::Success, bytes_returned, {}};
      }

      case kIoctlQueryDisplayManifest: {
        if (!write_output(output, output_size, controller_.query_display_manifest(), bytes_returned)) {
          return {IoctlStatus::InvalidOutputBuffer, 0, {}};
        }

        return {IoctlStatus::Success, bytes_returned, {}};
      }

      case kIoctlSetRenderAdapter: {
        SetRenderAdapterRequest request {};
        if (!read_input(input, input_size, request)) {
          return {IoctlStatus::InvalidInputBuffer, 0, {}};
        }

        return result_from_controller(controller_.set_render_adapter(request));
      }

      default:
        return {IoctlStatus::InvalidIoctl, 0, {}};
    }
  }

  const char *to_string(const IoctlStatus status) {
    switch (status) {
      case IoctlStatus::Success:
        return "success";
      case IoctlStatus::InvalidIoctl:
        return "invalid_ioctl";
      case IoctlStatus::InvalidInputBuffer:
        return "invalid_input_buffer";
      case IoctlStatus::InvalidOutputBuffer:
        return "invalid_output_buffer";
      case IoctlStatus::InvalidRequest:
        return "invalid_request";
      case IoctlStatus::AlreadyExists:
        return "already_exists";
      case IoctlStatus::LimitReached:
        return "limit_reached";
      case IoctlStatus::NotFound:
        return "not_found";
      case IoctlStatus::BackendFailed:
        return "backend_failed";
    }

    return "unknown";
  }
}  // namespace virtual_display::driver
