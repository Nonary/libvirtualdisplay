#pragma once

#include "virtual_display/driver/display_identity.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace virtual_display::driver {
  enum class BackendError {
    None,
    Failed,
  };

  struct BackendDisplayResult {
    BackendError error {BackendError::None};
    AdapterLuid os_adapter_luid {};
    std::uint32_t target_id {};
  };

  struct DisplayDescriptor {
    std::uint64_t lease_id {};
    std::uint64_t display_id {};
    Guid container_id {};
    std::uint32_t connector_index {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t physical_width_mm {};
    std::uint32_t physical_height_mm {};
    std::uint32_t refresh_rate_millihz {};
    std::array<std::byte, kEdidSize> edid {};
    bool retain_identity {true};
  };

  class DisplayDriverBackend {
  public:
    virtual ~DisplayDriverBackend() = default;

    virtual BackendError reserve_temporary_display_identity(const DisplayDescriptor &descriptor);
    virtual BackendError unreserve_temporary_display_identity(std::uint64_t display_id);
    virtual BackendDisplayResult arrive_temporary_display(const DisplayDescriptor &descriptor) = 0;
    virtual BackendError depart_temporary_display(std::uint64_t display_id) = 0;
    virtual BackendError set_permanent_display_count(const PermanentDisplayCountRequest &request) = 0;
    virtual BackendError apply_display_manifest(const DisplayManifest &manifest);
    virtual BackendError set_render_adapter(const SetRenderAdapterRequest &request) = 0;
  };

  struct ControllerStatus {
    StoreError store_error {StoreError::None};
    ValidationError validation_error {ValidationError::None};
    BackendError backend_error {BackendError::None};

    [[nodiscard]] bool ok() const;
  };

  struct ControllerCreateResult {
    ControllerStatus status {};
    CreateTemporaryDisplayResult result {};
  };

  class DriverController {
  public:
    DriverController(DisplayStore store, DisplayDriverBackend &backend);

    ControllerCreateResult create_temporary_display(
      const CreateTemporaryDisplayRequest &request,
      std::chrono::steady_clock::time_point now,
      std::unique_lock<std::timed_mutex> *controller_lock = nullptr
    );
    ControllerStatus remove_temporary_display(const LeaseDisplayRequest &request);
    ControllerStatus remove_temporary_display(
      const LeaseDisplayRequest &request,
      std::unique_lock<std::timed_mutex> *controller_lock
    );
    ControllerStatus feed_lease(const LeaseRequest &request, std::chrono::steady_clock::time_point now);
    ControllerStatus release_lease(const LeaseRequest &request);
    ControllerStatus release_lease(
      const LeaseRequest &request,
      std::unique_lock<std::timed_mutex> *controller_lock
    );
    QueryLeaseResult query_lease(std::uint64_t lease_id, std::chrono::steady_clock::time_point now) const;
    ControllerStatus set_permanent_display_count(const PermanentDisplayCountRequest &request);
    ControllerStatus set_permanent_display_count(
      const PermanentDisplayCountRequest &request,
      std::unique_lock<std::timed_mutex> *controller_lock
    );
    ControllerStatus apply_display_manifest(const DisplayManifest &manifest);
    ControllerStatus apply_display_manifest(
      const DisplayManifest &manifest,
      std::unique_lock<std::timed_mutex> *controller_lock
    );
    ControllerStatus set_render_adapter(const SetRenderAdapterRequest &request);
    ControllerStatus set_render_adapter(
      const SetRenderAdapterRequest &request,
      std::unique_lock<std::timed_mutex> *controller_lock
    );
    const DisplayManifest &query_display_manifest() const;
    PermanentDisplayCountResult query_permanent_display_count() const;
    QueryDisplayStateResult query_display_state() const;
    std::uint32_t reap_expired(std::chrono::steady_clock::time_point now);
    std::uint32_t reap_expired(
      std::chrono::steady_clock::time_point now,
      std::unique_lock<std::timed_mutex> *controller_lock
    );

    [[nodiscard]] const DisplayStore &store() const;

  private:
    static ControllerStatus from_store_result(const StoreResult &result);
    bool temporary_display_generation_is_current(const TemporaryDisplayRecord &record) const;
    DisplayDescriptor descriptor_from_record(const TemporaryDisplayRecord &record) const;
    static DisplayStateEntry state_entry_from_record(const TemporaryDisplayRecord &record);

    DisplayStore store_;
    DisplayDriverBackend &backend_;
    std::timed_mutex backend_call_mutex_ {};
  };

  const char *to_string(BackendError error);
}  // namespace virtual_display::driver
