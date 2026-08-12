#include "virtual_display/driver/control_client.h"

namespace virtual_display::driver {
  ControlClient::ControlClient(ControlTransport &transport):
      transport_ {transport} {
  }

  ControlResult<ProtocolVersion> ControlClient::query_protocol_version() {
    auto result = ioctl_out<ProtocolVersion>(kIoctlGetProtocolVersion, nullptr, 0);
    if (!result.ok()) {
      return result;
    }
    if (!is_valid_api_namespace(result.value.api_namespace) ||
        result.value.major != kProtocolVersionMajor ||
        result.value.minor < kMinimumCompatibleProtocolVersionMinor) {
      result.status = ControlStatus::ProtocolIncompatible;
    }
    return result;
  }

  ControlOperationResult ControlClient::check_protocol_compatible() {
    const auto version = query_protocol_version();
    return {version.status, version.native_error};
  }

  ControlResult<CreateTemporaryDisplayResult> ControlClient::create_temporary_display(
    const CreateTemporaryDisplayRequest &request
  ) {
    return ioctl_out<CreateTemporaryDisplayResult>(kIoctlCreateTemporaryDisplay, &request, sizeof(request));
  }

  ControlOperationResult ControlClient::remove_temporary_display(const LeaseDisplayRequest &request) {
    return ioctl_no_out(kIoctlRemoveTemporaryDisplay, &request, sizeof(request));
  }

  ControlOperationResult ControlClient::feed_lease(const LeaseRequest &request) {
    return ioctl_no_out(kIoctlFeedLease, &request, sizeof(request));
  }

  ControlOperationResult ControlClient::release_lease(const LeaseRequest &request) {
    return ioctl_no_out(kIoctlReleaseLease, &request, sizeof(request));
  }

  ControlResult<QueryLeaseResult> ControlClient::query_lease(const LeaseRequest &request) {
    return ioctl_out<QueryLeaseResult>(kIoctlQueryLease, &request, sizeof(request));
  }

  ControlResult<PermanentDisplayCountResult> ControlClient::set_permanent_display_count(
    const PermanentDisplayCountRequest &request
  ) {
    return ioctl_out<PermanentDisplayCountResult>(kIoctlSetPermanentDisplayCount, &request, sizeof(request));
  }

  ControlResult<PermanentDisplayCountResult> ControlClient::query_permanent_display_count() {
    return ioctl_out<PermanentDisplayCountResult>(kIoctlQueryPermanentDisplayCount, nullptr, 0);
  }

  ControlResult<QueryDisplayStateResult> ControlClient::query_display_state() {
    return ioctl_out<QueryDisplayStateResult>(kIoctlQueryDisplayState, nullptr, 0);
  }

  ControlResult<DisplayManifest> ControlClient::set_display_manifest(const DisplayManifest &manifest) {
    return ioctl_out<DisplayManifest>(kIoctlSetDisplayManifest, &manifest, sizeof(manifest));
  }

  ControlResult<DisplayManifest> ControlClient::query_display_manifest() {
    return ioctl_out<DisplayManifest>(kIoctlQueryDisplayManifest, nullptr, 0);
  }

  ControlResult<CreateTemporaryDisplayResult> ControlClient::create_temporary_display_owned(
    const CreateTemporaryDisplayOwnedRequest &request
  ) {
    return ioctl_out<CreateTemporaryDisplayResult>(
      kIoctlCreateTemporaryDisplayOwned,
      &request,
      sizeof(request)
    );
  }

  ControlResult<ReclaimTemporaryDisplayResult> ControlClient::reclaim_temporary_display(
    const ReclaimTemporaryDisplayRequest &request
  ) {
    return ioctl_out<ReclaimTemporaryDisplayResult>(
      kIoctlReclaimTemporaryDisplay,
      &request,
      sizeof(request)
    );
  }

  ControlOperationResult ControlClient::set_render_adapter(const SetRenderAdapterRequest &request) {
    return ioctl_no_out(kIoctlSetRenderAdapter, &request, sizeof(request));
  }

  ControlOperationResult ControlClient::set_display_hdr_state(const SetDisplayHdrStateRequest &request) {
    return ioctl_no_out(kIoctlSetDisplayHdrState, &request, sizeof(request));
  }

  ControlOperationResult ControlClient::ioctl_no_out(
    const std::uint32_t ioctl_code,
    const void *input,
    const std::size_t input_size
  ) {
    std::size_t bytes_returned = 0;
    std::uint32_t native_error = 0;
    if (!transport_.ioctl(ioctl_code, input, input_size, nullptr, 0, bytes_returned, native_error)) {
      return {ControlStatus::TransportFailed, native_error};
    }
    if (bytes_returned != 0) {
      return {ControlStatus::InvalidOutput, native_error};
    }
    return {ControlStatus::Success, native_error};
  }

  const char *to_string(const ControlStatus status) {
    switch (status) {
      case ControlStatus::Success:
        return "success";
      case ControlStatus::TransportFailed:
        return "transport_failed";
      case ControlStatus::InvalidOutput:
        return "invalid_output";
      case ControlStatus::ProtocolIncompatible:
        return "protocol_incompatible";
    }

    return "unknown";
  }
}  // namespace virtual_display::driver
