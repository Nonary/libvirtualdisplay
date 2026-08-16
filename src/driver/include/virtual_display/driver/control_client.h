#pragma once

#include "virtual_display/driver/control_protocol.h"

#include <cstddef>
#include <cstdint>

namespace virtual_display::driver {
  enum class ControlStatus {
    Success,
    TransportFailed,
    InvalidOutput,
    ProtocolIncompatible,
  };

  const char *to_string(ControlStatus status);

  struct ControlOperationResult {
    ControlStatus status {ControlStatus::Success};
    std::uint32_t native_error {};

    [[nodiscard]] bool ok() const {
      return status == ControlStatus::Success;
    }
  };

  template<class T>
  struct ControlResult {
    ControlStatus status {ControlStatus::Success};
    T value {};
    std::uint32_t native_error {};

    [[nodiscard]] bool ok() const {
      return status == ControlStatus::Success;
    }
  };

  class ControlTransport {
  public:
    virtual ~ControlTransport() = default;

    virtual bool ioctl(
      std::uint32_t ioctl_code,
      const void *input,
      std::size_t input_size,
      void *output,
      std::size_t output_size,
      std::size_t &bytes_returned,
      std::uint32_t &native_error
    ) = 0;
  };

  class ControlClient {
  public:
    explicit ControlClient(ControlTransport &transport);

    ControlResult<ProtocolVersion> query_protocol_version();
    ControlOperationResult check_protocol_compatible();
    ControlResult<CreateTemporaryDisplayResult> create_temporary_display(const CreateTemporaryDisplayRequest &request);
    ControlResult<CreateTemporaryDisplayResult> create_temporary_display_owned(
      const CreateTemporaryDisplayOwnedRequest &request
    );
    ControlResult<ReclaimTemporaryDisplayResult> reclaim_temporary_display(
      const ReclaimTemporaryDisplayRequest &request
    );
    ControlOperationResult remove_temporary_display(const LeaseDisplayRequest &request);
    ControlOperationResult feed_lease(const LeaseRequest &request);
    ControlOperationResult release_lease(const LeaseRequest &request);
    ControlResult<QueryLeaseResult> query_lease(const LeaseRequest &request);
    ControlResult<PermanentDisplayCountResult> set_permanent_display_count(const PermanentDisplayCountRequest &request);
    ControlResult<PermanentDisplayCountResult> query_permanent_display_count();
    ControlResult<QueryDisplayStateResult> query_display_state();
    ControlResult<DisplayManifest> set_display_manifest(const DisplayManifest &manifest);
    ControlResult<DisplayManifest> query_display_manifest();
    ControlOperationResult set_render_adapter(const SetRenderAdapterRequest &request);
    ControlOperationResult set_display_hdr_state(const SetDisplayHdrStateRequest &request);
    ControlOperationResult set_display_mode(const SetDisplayModeRequest &request);

  private:
    template<class T>
    ControlResult<T> ioctl_out(std::uint32_t ioctl_code, const void *input, std::size_t input_size) {
      T output {};
      output.api_namespace = Guid {};
      std::size_t bytes_returned = 0;
      std::uint32_t native_error = 0;
      if (!transport_.ioctl(ioctl_code, input, input_size, &output, sizeof(output), bytes_returned, native_error)) {
        return {ControlStatus::TransportFailed, {}, native_error};
      }
      if (bytes_returned != sizeof(output)) {
        return {ControlStatus::InvalidOutput, {}, native_error};
      }
      if (!is_valid_api_namespace(output.api_namespace)) {
        return {ControlStatus::ProtocolIncompatible, output, native_error};
      }
      return {ControlStatus::Success, output, native_error};
    }

    ControlOperationResult ioctl_no_out(std::uint32_t ioctl_code, const void *input, std::size_t input_size);

    ControlTransport &transport_;
  };
}  // namespace virtual_display::driver
