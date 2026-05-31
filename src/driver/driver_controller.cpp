#include "virtual_display/driver/driver_controller.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace virtual_display::driver {
  bool ControllerStatus::ok() const {
    return store_error == StoreError::None &&
           validation_error == ValidationError::None &&
           backend_error == BackendError::None;
  }

  DriverController::DriverController(DisplayStore store, DisplayDriverBackend &backend):
      store_ {std::move(store)},
      backend_ {backend} {
  }

  ControllerCreateResult DriverController::create_temporary_display(
    const CreateTemporaryDisplayRequest &request,
    const std::chrono::steady_clock::time_point now
  ) {
    auto created = store_.create_temporary_display(request, now);
    if (created.status.error != StoreError::None) {
      return {from_store_result(created.status), {}};
    }

    const auto record = store_.find_temporary_display(request.display_id);
    if (!record) {
      return {
        {StoreError::DisplayNotFound, ValidationError::None, BackendError::None},
        {}
      };
    }

    const auto descriptor = descriptor_from_record(*record);
    bool identity_reserved = false;
    if (descriptor.retain_identity) {
      if (const auto backend_error = backend_.reserve_temporary_display_identity(descriptor);
          backend_error != BackendError::None) {
        LeaseDisplayRequest rollback {};
        rollback.lease_id = request.lease_id;
        rollback.display_id = request.display_id;
        (void) store_.remove_temporary_display(
          rollback,
          RemoveTemporaryDisplayMode::ReleaseConnectorReservation
        );
        return {
          {StoreError::None, ValidationError::None, backend_error},
          {}
        };
      }
      identity_reserved = true;
    }

    const auto backend_result = backend_.arrive_temporary_display(descriptor);
    if (backend_result.error != BackendError::None) {
      auto rollback_mode = RemoveTemporaryDisplayMode::ReleaseConnectorReservation;
      if (identity_reserved) {
        if (backend_.unreserve_temporary_display_identity(request.display_id) != BackendError::None) {
          rollback_mode = RemoveTemporaryDisplayMode::RetainConnectorReservation;
        }
      }
      LeaseDisplayRequest rollback {};
      rollback.lease_id = request.lease_id;
      rollback.display_id = request.display_id;
      (void) store_.remove_temporary_display(
        rollback,
        rollback_mode
      );
      return {
        {StoreError::None, ValidationError::None, backend_result.error},
        {}
      };
    }

    created.result.os_adapter_luid = backend_result.os_adapter_luid;
    created.result.target_id = backend_result.target_id;
    return {{}, created.result};
  }

  ControllerStatus DriverController::remove_temporary_display(const LeaseDisplayRequest &request) {
    if (const auto validation = validate_lease_display_request(request);
        validation != ValidationError::None) {
      return {StoreError::ValidationFailed, validation, BackendError::None};
    }

    const auto record = store_.find_temporary_display(request.display_id);
    if (!record || record->lease_id != request.lease_id) {
      return {StoreError::DisplayNotFound, ValidationError::None, BackendError::None};
    }

    if (const auto backend_error = backend_.depart_temporary_display(request.display_id);
        backend_error != BackendError::None) {
      (void) store_.mark_temporary_display_pending_departure(request);
      return {StoreError::None, ValidationError::None, backend_error};
    }

    return from_store_result(store_.remove_temporary_display(request));
  }

  ControllerStatus DriverController::feed_lease(
    const LeaseRequest &request,
    const std::chrono::steady_clock::time_point now
  ) {
    return from_store_result(store_.feed_lease(request, now));
  }

  ControllerStatus DriverController::release_lease(const LeaseRequest &request) {
    if (const auto validation = validate_lease_request(request);
        validation != ValidationError::None) {
      return {StoreError::ValidationFailed, validation, BackendError::None};
    }

    const auto displays = store_.temporary_displays_for_lease(request.lease_id);
    if (displays.empty()) {
      return from_store_result(store_.release_lease(request));
    }

    BackendError backend_error = BackendError::None;
    for (const auto &display: displays) {
      if (backend_.depart_temporary_display(display.display_id) == BackendError::None) {
        LeaseDisplayRequest remove {};
        remove.lease_id = display.lease_id;
        remove.display_id = display.display_id;
        (void) store_.remove_temporary_display(remove);
      } else {
        LeaseDisplayRequest pending {};
        pending.lease_id = display.lease_id;
        pending.display_id = display.display_id;
        (void) store_.mark_temporary_display_pending_departure(pending);
        backend_error = BackendError::Failed;
      }
    }

    return {StoreError::None, ValidationError::None, backend_error};
  }

  QueryLeaseResult DriverController::query_lease(
    const std::uint64_t lease_id,
    const std::chrono::steady_clock::time_point now
  ) const {
    return store_.query_lease(lease_id, now);
  }

  ControllerStatus DriverController::set_permanent_display_count(const PermanentDisplayCountRequest &request) {
    auto normalized = request;
    set_default_permanent_display_settings(normalized);
    if (const auto validation = validate_permanent_display_count(normalized, store_.max_permanent_displays());
        validation != ValidationError::None) {
      return {StoreError::ValidationFailed, validation, BackendError::None};
    }

    return apply_display_manifest(
      display_manifest_from_permanent_settings(normalized, store_.max_permanent_displays())
    );
  }

  ControllerStatus DriverController::apply_display_manifest(const DisplayManifest &manifest) {
    if (const auto validation = validate_display_manifest(manifest, store_.max_permanent_displays());
        validation != ValidationError::None) {
      return {StoreError::ValidationFailed, validation, BackendError::None};
    }

    auto canonical = manifest;
    for (std::uint32_t index = 0; index < canonical.profile_count; ++index) {
      char display_name[kDisplayNameChars] {};
      if (!canonicalize_display_name(canonical.profiles[index].display_name, display_name)) {
        return {StoreError::ValidationFailed, ValidationError::InvalidDisplayName, BackendError::None};
      }
      std::copy(
        std::begin(display_name),
        std::end(display_name),
        std::begin(canonical.profiles[index].display_name)
      );
    }

    if (const auto backend_error = backend_.apply_display_manifest(canonical);
        backend_error != BackendError::None) {
      return {StoreError::None, ValidationError::None, backend_error};
    }

    return from_store_result(store_.apply_display_manifest(canonical));
  }

  ControllerStatus DriverController::set_render_adapter(const SetRenderAdapterRequest &request) {
    if (!is_valid_api_namespace(request.api_namespace)) {
      return {StoreError::ValidationFailed, ValidationError::WrongApiNamespace, BackendError::None};
    }
    if (request.flags != 0) {
      return {StoreError::ValidationFailed, ValidationError::InvalidFlags, BackendError::None};
    }

    if (const auto backend_error = backend_.set_render_adapter(request);
        backend_error != BackendError::None) {
      return {StoreError::None, ValidationError::None, backend_error};
    }

    return {};
  }

  const DisplayManifest &DriverController::query_display_manifest() const {
    return store_.display_manifest();
  }

  PermanentDisplayCountResult DriverController::query_permanent_display_count() const {
    return store_.query_permanent_display_count();
  }

  QueryDisplayStateResult DriverController::query_display_state() const {
    QueryDisplayStateResult result {};
    const auto &manifest = store_.display_manifest();
    result.permanent_display_count = store_.permanent_display_count();
    result.temporary_display_count = store_.temporary_display_count();

    for (std::uint32_t index = 0;
         index < manifest.profile_count && result.entry_count < kMaxDisplayStateEntries;
         ++index) {
      const auto &profile = manifest.profiles[index];
      const auto &mode = profile.allowed_modes[profile.native_mode_index];
      auto &entry = result.entries[result.entry_count++];
      entry.kind = kDisplayStateKindPermanent;
      if ((profile.flags & kDisplayManifestProfileFlagHdrSupported) != 0) {
        entry.flags |= kDisplayStateFlagHdrSupported;
      }
      if ((profile.flags & kDisplayManifestProfileFlagRetainIdentity) != 0) {
        entry.flags |= kDisplayStateFlagRetainIdentity;
      }
      entry.display_id = profile.display_id;
      entry.container_id = profile.container_id;
      entry.connector_index = profile.connector_index;
      entry.product_code = profile.product_code;
      entry.serial_number = profile.serial_number;
      entry.width = mode.width;
      entry.height = mode.height;
      entry.physical_width_mm = profile.physical_width_mm;
      entry.physical_height_mm = profile.physical_height_mm;
      entry.refresh_rate_millihz = mode.refresh_rate_millihz;
      std::copy(
        std::begin(profile.display_name),
        std::end(profile.display_name),
        std::begin(entry.display_name)
      );
    }

    for (const auto &display: store_.temporary_displays()) {
      if (result.entry_count >= kMaxDisplayStateEntries) {
        break;
      }
      result.entries[result.entry_count++] = state_entry_from_record(display);
    }

    return result;
  }

  std::uint32_t DriverController::reap_expired(const std::chrono::steady_clock::time_point now) {
    const auto expired = store_.expired_temporary_displays(now);
    std::uint32_t removed = 0;

    for (const auto &display: expired) {
      if (backend_.depart_temporary_display(display.display_id) == BackendError::None) {
        LeaseDisplayRequest remove {};
        remove.lease_id = display.lease_id;
        remove.display_id = display.display_id;
        if (store_.remove_temporary_display(remove).error == StoreError::None) {
          ++removed;
        }
      } else {
        LeaseDisplayRequest pending {};
        pending.lease_id = display.lease_id;
        pending.display_id = display.display_id;
        (void) store_.mark_temporary_display_pending_departure(pending);
      }
    }

    return removed;
  }

  const DisplayStore &DriverController::store() const {
    return store_;
  }

  BackendError DisplayDriverBackend::reserve_temporary_display_identity(const DisplayDescriptor &) {
    return BackendError::None;
  }

  BackendError DisplayDriverBackend::unreserve_temporary_display_identity(const std::uint64_t) {
    return BackendError::None;
  }

  BackendError DisplayDriverBackend::apply_display_manifest(const DisplayManifest &) {
    return BackendError::Failed;
  }

  ControllerStatus DriverController::from_store_result(const StoreResult &result) {
    return {result.error, result.validation_error, BackendError::None};
  }

  DisplayDescriptor DriverController::descriptor_from_record(const TemporaryDisplayRecord &record) const {
    DisplayDescriptor descriptor {};
    descriptor.lease_id = record.lease_id;
    descriptor.display_id = record.display_id;
    const auto identity_display_id = record.identity_display_id == 0 ?
      record.display_id :
      record.identity_display_id;
    descriptor.container_id = container_guid_from_display_id(identity_display_id);
    descriptor.connector_index = record.connector_index;
    descriptor.width = record.width;
    descriptor.height = record.height;
    descriptor.physical_width_mm = record.physical_width_mm;
    descriptor.physical_height_mm = record.physical_height_mm;
    descriptor.refresh_rate_millihz = record.refresh_rate_millihz;
    descriptor.edid = create_edid(edid_options_for_temporary_display(record));
    descriptor.retain_identity = record.retain_identity;
    return descriptor;
  }

  DisplayStateEntry DriverController::state_entry_from_record(const TemporaryDisplayRecord &record) {
    DisplayStateEntry entry {};
    entry.kind = kDisplayStateKindTemporary;
    entry.flags = kDisplayStateFlagHdrSupported;
    if (record.retain_identity) {
      entry.flags |= kDisplayStateFlagRetainIdentity;
    }
    entry.display_id = record.display_id;
    const auto identity_display_id = record.identity_display_id == 0 ?
      record.display_id :
      record.identity_display_id;
    entry.container_id = container_guid_from_display_id(identity_display_id);
    entry.connector_index = record.connector_index;
    entry.product_code = product_code_from_display_id(identity_display_id);
    entry.serial_number = serial_number_from_display_id(identity_display_id);
    entry.width = record.width;
    entry.height = record.height;
    entry.physical_width_mm = record.physical_width_mm;
    entry.physical_height_mm = record.physical_height_mm;
    entry.refresh_rate_millihz = record.refresh_rate_millihz;
    const auto copy_size = (std::min)(record.display_name.size(), static_cast<std::size_t>(kDisplayNameChars - 1));
    std::memcpy(entry.display_name, record.display_name.data(), copy_size);
    return entry;
  }

  const char *to_string(const BackendError error) {
    switch (error) {
      case BackendError::None:
        return "none";
      case BackendError::Failed:
        return "failed";
    }

    return "unknown";
  }
}  // namespace virtual_display::driver
