#include "virtual_display/driver/driver_controller.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>

namespace virtual_display::driver {
  namespace {
    constexpr auto kBackendCallLockTimeout = std::chrono::milliseconds(500);

    template<class Result, class Call>
    auto call_backend_without_controller_lock(
      std::timed_mutex &backend_call_mutex,
      std::unique_lock<std::timed_mutex> *controller_lock,
      Result busy_result,
      Call &&call
    ) -> Result {
      const bool relock_controller = controller_lock && controller_lock->owns_lock();
      if (relock_controller) {
        controller_lock->unlock();
      }

      std::unique_lock backend_lock {backend_call_mutex, std::defer_lock};
      if (!backend_lock.try_lock_for(kBackendCallLockTimeout)) {
        if (relock_controller) {
          controller_lock->lock();
        }
        return busy_result;
      }

      auto result = call();
      if (relock_controller) {
        controller_lock->lock();
      }
      return result;
    }
  }  // namespace

  bool ControllerStatus::ok() const {
    return store_error == StoreError::None &&
           validation_error == ValidationError::None &&
           backend_error == BackendError::None;
  }

  bool DriverController::temporary_display_generation_is_current(
    const TemporaryDisplayRecord &record
  ) const {
    const auto current = store_.find_temporary_display(record.display_id);
    return current &&
           current->lease_id == record.lease_id &&
           current->generation == record.generation;
  }

  DriverController::DriverController(DisplayStore store, DisplayDriverBackend &backend):
      store_ {std::move(store)},
      backend_ {backend} {
  }

  ControllerCreateResult DriverController::create_temporary_display(
    const CreateTemporaryDisplayRequest &request,
    const std::chrono::steady_clock::time_point now,
    std::unique_lock<std::timed_mutex> *controller_lock
  ) {
    return create_temporary_display_impl(request, nullptr, now, controller_lock);
  }

  ControllerCreateResult DriverController::create_temporary_display_owned(
    const CreateTemporaryDisplayOwnedRequest &request,
    const std::chrono::steady_clock::time_point now,
    std::unique_lock<std::timed_mutex> *controller_lock
  ) {
    if (const auto validation = validate_create_temporary_display_owned(request);
        validation != ValidationError::None) {
      return {
        {StoreError::ValidationFailed, validation, BackendError::None},
        {}
      };
    }
    return create_temporary_display_impl(
      request.display,
      &request.owner_capability,
      now,
      controller_lock
    );
  }

  ControllerCreateResult DriverController::create_temporary_display_impl(
    const CreateTemporaryDisplayRequest &request,
    const OwnerCapability *owner_capability,
    const std::chrono::steady_clock::time_point now,
    std::unique_lock<std::timed_mutex> *controller_lock
  ) {
    if (const auto validation = validate_create_temporary_display(request);
        validation != ValidationError::None) {
      return {
        {StoreError::ValidationFailed, validation, BackendError::None},
        {}
      };
    }

    if (const auto existing = store_.find_temporary_display(request.display_id);
        existing && existing->pending_departure) {
      const auto pending_record = *existing;
      // Depart unfenced (generation 0): the store record is pending departure,
      // so any backend monitor still parked under this display_id belongs to an
      // abandoned lineage and must be cleared before the fresh create.
      if (const auto backend_error = call_backend_without_controller_lock(
            backend_call_mutex_,
            controller_lock,
            BackendError::Busy,
            [&]() {
              return backend_.depart_temporary_display(existing->display_id, 0);
            });
          backend_error != BackendError::None) {
        if (backend_error == BackendError::Failed &&
            temporary_display_generation_is_current(pending_record)) {
          LeaseDisplayRequest pending {};
          pending.lease_id = pending_record.lease_id;
          pending.display_id = pending_record.display_id;
          (void) store_.mark_temporary_display_pending_departure(pending);
        }
        return {
          {StoreError::None, ValidationError::None, backend_error},
          {}
        };
      }

      if (temporary_display_generation_is_current(pending_record)) {
        LeaseDisplayRequest remove {};
        remove.lease_id = pending_record.lease_id;
        remove.display_id = pending_record.display_id;
        (void) store_.remove_temporary_display(remove);
      }
    }

    auto created = [&]() {
      if (!owner_capability) {
        return store_.create_temporary_display(request, now);
      }
      CreateTemporaryDisplayOwnedRequest owned {};
      owned.display = request;
      owned.owner_capability = *owner_capability;
      return store_.create_temporary_display_owned(owned, now);
    }();
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
    const auto created_record = *record;
    bool identity_reserved = false;
    if (descriptor.retain_identity) {
      if (const auto backend_error = call_backend_without_controller_lock(
            backend_call_mutex_,
            controller_lock,
            BackendError::Busy,
            [&]() {
              return backend_.reserve_temporary_display_identity(descriptor);
            });
          backend_error != BackendError::None) {
        // Roll back only if our just-created record is still current. A concurrent
        // remove+recreate of the same display_id during the unlocked backend call
        // could otherwise make us delete the replacement record and free its
        // connector reservation (store/backend divergence).
        if (temporary_display_generation_is_current(created_record)) {
          LeaseDisplayRequest rollback {};
          rollback.lease_id = request.lease_id;
          rollback.display_id = request.display_id;
          (void) store_.remove_temporary_display(
            rollback,
            RemoveTemporaryDisplayMode::ReleaseConnectorReservation
          );
        }
        return {
          {StoreError::None, ValidationError::None, backend_error},
          {}
        };
      }
      identity_reserved = true;

      if (!temporary_display_generation_is_current(created_record)) {
        (void) call_backend_without_controller_lock(
          backend_call_mutex_,
          controller_lock,
          BackendError::Busy,
          [&]() {
            return backend_.unreserve_temporary_display_identity(request.display_id);
          }
        );
        return {
          {StoreError::DisplayNotFound, ValidationError::None, BackendError::None},
          {}
        };
      }
    }

    const auto backend_result = call_backend_without_controller_lock(
      backend_call_mutex_,
      controller_lock,
      BackendDisplayResult {BackendError::Busy, {}, 0},
      [&]() {
        return backend_.arrive_temporary_display(descriptor);
      }
    );
    if (backend_result.error != BackendError::None) {
      auto rollback_mode = RemoveTemporaryDisplayMode::ReleaseConnectorReservation;
      if (identity_reserved) {
        if (call_backend_without_controller_lock(
              backend_call_mutex_,
              controller_lock,
              BackendError::Busy,
              [&]() {
                return backend_.unreserve_temporary_display_identity(request.display_id);
              }) != BackendError::None) {
          rollback_mode = RemoveTemporaryDisplayMode::RetainConnectorReservation;
        }
      }
      LeaseDisplayRequest rollback {};
      rollback.lease_id = request.lease_id;
      rollback.display_id = request.display_id;
      // Same ABA guard as the reserve-failure rollback: don't delete a replacement
      // record created while the backend arrive/unreserve call held no lock.
      if (temporary_display_generation_is_current(created_record)) {
        (void) store_.remove_temporary_display(
          rollback,
          rollback_mode
        );
      }
      return {
        {StoreError::None, ValidationError::None, backend_result.error},
        {}
      };
    }

    const auto arrived_record = store_.find_temporary_display(request.display_id);
    if (!arrived_record ||
        arrived_record->lease_id != request.lease_id ||
        arrived_record->generation != created_record.generation) {
      (void) call_backend_without_controller_lock(
        backend_call_mutex_,
        controller_lock,
        BackendError::Busy,
        [&]() {
          return backend_.depart_temporary_display(request.display_id, created_record.generation);
        }
      );
      return {
        {StoreError::DisplayNotFound, ValidationError::None, BackendError::None},
        {}
      };
    }
    if (arrived_record->pending_departure) {
      const auto cleanup_error = call_backend_without_controller_lock(
        backend_call_mutex_,
        controller_lock,
        BackendError::Busy,
        [&]() {
          return backend_.depart_temporary_display(request.display_id, created_record.generation);
        }
      );
      if (cleanup_error == BackendError::None &&
          temporary_display_generation_is_current(*arrived_record)) {
        LeaseDisplayRequest remove {};
        remove.lease_id = arrived_record->lease_id;
        remove.display_id = arrived_record->display_id;
        (void) store_.remove_temporary_display(remove);
      }
      return {
        {cleanup_error == BackendError::None ? StoreError::DisplayNotFound : StoreError::None, ValidationError::None, cleanup_error},
        {}
      };
    }

    created.result.os_adapter_luid = backend_result.os_adapter_luid;
    created.result.target_id = backend_result.target_id;
    return {{}, created.result};
  }

  ControllerReclaimResult DriverController::reclaim_temporary_display(
    const ReclaimTemporaryDisplayRequest &request,
    const std::chrono::steady_clock::time_point now,
    std::unique_lock<std::timed_mutex> *controller_lock
  ) {
    if (const auto validation = validate_reclaim_temporary_display(request);
        validation != ValidationError::None) {
      return {{StoreError::ValidationFailed, validation, BackendError::None}, {}};
    }

    // Reclaim changes the lease fencing token used by lifecycle ABA checks. Wait
    // until no backend arrival/departure is in flight, then hold both locks while
    // rotating it so a completed departure cannot leave a live store record behind.
    const bool relock_controller = controller_lock && controller_lock->owns_lock();
    if (relock_controller) {
      controller_lock->unlock();
    }
    std::unique_lock backend_lock {backend_call_mutex_, std::defer_lock};
    if (!backend_lock.try_lock_for(kBackendCallLockTimeout)) {
      if (relock_controller) {
        controller_lock->lock();
      }
      return {{StoreError::None, ValidationError::None, BackendError::Busy}, {}};
    }
    if (relock_controller) {
      controller_lock->lock();
    }

    const auto reclaimed = store_.reclaim_temporary_display(request, now);
    return {from_store_result(reclaimed.status), reclaimed.result};
  }

  ControllerStatus DriverController::remove_temporary_display(const LeaseDisplayRequest &request) {
    return remove_temporary_display(request, nullptr);
  }

  ControllerStatus DriverController::remove_temporary_display(
    const LeaseDisplayRequest &request,
    std::unique_lock<std::timed_mutex> *controller_lock
  ) {
    if (const auto validation = validate_lease_display_request(request);
        validation != ValidationError::None) {
      return {StoreError::ValidationFailed, validation, BackendError::None};
    }

    const auto record = store_.find_temporary_display(request.display_id);
    if (!record || record->lease_id != request.lease_id) {
      return {StoreError::DisplayNotFound, ValidationError::None, BackendError::None};
    }

    if (const auto backend_error = call_backend_without_controller_lock(
          backend_call_mutex_,
          controller_lock,
          BackendError::Busy,
          [&]() {
            return backend_.depart_temporary_display(request.display_id, record->generation);
          });
        backend_error != BackendError::None) {
      // Only mark OUR record pending-departure, and only when the backend
      // actually failed the departure. The backend call released the controller
      // lock, so a concurrent remove+recreate of the same display_id could have
      // produced a newer record; marking that would let the reaper depart a
      // healthy, in-use virtual display (ABA). A Busy result means the backend
      // was never invoked - leave the record untouched so a mere lock timeout
      // cannot poison a live display. Matches release_lease.
      if (backend_error == BackendError::Failed &&
          temporary_display_generation_is_current(*record)) {
        (void) store_.mark_temporary_display_pending_departure(request);
      }
      return {StoreError::None, ValidationError::None, backend_error};
    }

    if (!temporary_display_generation_is_current(*record)) {
      return {StoreError::DisplayNotFound, ValidationError::None, BackendError::None};
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
    return release_lease(request, nullptr);
  }

  ControllerStatus DriverController::release_lease(
    const LeaseRequest &request,
    std::unique_lock<std::timed_mutex> *controller_lock
  ) {
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
      const auto depart_error = call_backend_without_controller_lock(
        backend_call_mutex_,
        controller_lock,
        BackendError::Busy,
        [&]() {
          return backend_.depart_temporary_display(display.display_id, display.generation);
        }
      );
      if (depart_error == BackendError::None) {
        if (temporary_display_generation_is_current(display)) {
          LeaseDisplayRequest remove {};
          remove.lease_id = display.lease_id;
          remove.display_id = display.display_id;
          (void) store_.remove_temporary_display(remove);
        }
      } else {
        // Busy means the backend was never invoked; do not poison the record.
        if (depart_error == BackendError::Failed &&
            temporary_display_generation_is_current(display)) {
          LeaseDisplayRequest pending {};
          pending.lease_id = display.lease_id;
          pending.display_id = display.display_id;
          (void) store_.mark_temporary_display_pending_departure(pending);
        }
        if (backend_error != BackendError::Failed) {
          backend_error = depart_error;
        }
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
    return set_permanent_display_count(request, nullptr);
  }

  ControllerStatus DriverController::set_permanent_display_count(
    const PermanentDisplayCountRequest &request,
    std::unique_lock<std::timed_mutex> *controller_lock
  ) {
    auto normalized = request;
    set_default_permanent_display_settings(normalized);
    if (const auto validation = validate_permanent_display_count(normalized, store_.max_permanent_displays());
        validation != ValidationError::None) {
      return {StoreError::ValidationFailed, validation, BackendError::None};
    }

    return apply_display_manifest(
      display_manifest_from_permanent_settings(normalized, store_.max_permanent_displays()),
      controller_lock
    );
  }

  ControllerStatus DriverController::apply_display_manifest(const DisplayManifest &manifest) {
    return apply_display_manifest(manifest, nullptr);
  }

  ControllerStatus DriverController::apply_display_manifest(
    const DisplayManifest &manifest,
    std::unique_lock<std::timed_mutex> *controller_lock
  ) {
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

    if (const auto backend_error = call_backend_without_controller_lock(
          backend_call_mutex_,
          controller_lock,
          BackendError::Busy,
          [&]() {
            return backend_.apply_display_manifest(canonical);
          });
        backend_error != BackendError::None) {
      return {StoreError::None, ValidationError::None, backend_error};
    }

    return from_store_result(store_.apply_display_manifest(canonical));
  }

  ControllerStatus DriverController::set_render_adapter(const SetRenderAdapterRequest &request) {
    return set_render_adapter(request, nullptr);
  }

  ControllerStatus DriverController::set_render_adapter(
    const SetRenderAdapterRequest &request,
    std::unique_lock<std::timed_mutex> *controller_lock
  ) {
    if (!is_valid_api_namespace(request.api_namespace)) {
      return {StoreError::ValidationFailed, ValidationError::WrongApiNamespace, BackendError::None};
    }
    if (request.flags != 0) {
      return {StoreError::ValidationFailed, ValidationError::InvalidFlags, BackendError::None};
    }

    if (const auto backend_error = call_backend_without_controller_lock(
          backend_call_mutex_,
          controller_lock,
          BackendError::Busy,
          [&]() {
            return backend_.set_render_adapter(request);
          });
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
    return reap_expired(now, nullptr);
  }

  std::uint32_t DriverController::reap_expired(
    const std::chrono::steady_clock::time_point now,
    std::unique_lock<std::timed_mutex> *controller_lock
  ) {
    auto expired = store_.expired_temporary_displays(now);
    auto reap_candidates = store_.pending_departure_temporary_displays();
    for (const auto &display: expired) {
      if (std::none_of(
            reap_candidates.begin(),
            reap_candidates.end(),
            [&](const auto &candidate) {
              return candidate.display_id == display.display_id;
            })) {
        reap_candidates.push_back(display);
      }
    }
    std::uint32_t removed = 0;

    for (const auto &display: reap_candidates) {
      // Fence the backend departure on the snapshot's generation. The snapshot
      // was taken before the controller lock was released for the backend call,
      // so the same display_id may since have been removed and recreated; an
      // unfenced departure here would silently retract the replacement monitor
      // while its healthy store record survives (the field "created but never
      // enumerated" wedge).
      const auto depart_error = call_backend_without_controller_lock(
        backend_call_mutex_,
        controller_lock,
        BackendError::Busy,
        [&]() {
          return backend_.depart_temporary_display(display.display_id, display.generation);
        }
      );
      if (depart_error == BackendError::None) {
        LeaseDisplayRequest remove {};
        remove.lease_id = display.lease_id;
        remove.display_id = display.display_id;
        if (temporary_display_generation_is_current(display) &&
            store_.remove_temporary_display(remove).error == StoreError::None) {
          ++removed;
        }
      } else if (depart_error == BackendError::Failed) {
        // Busy means the backend was never invoked; the candidate stays
        // expired/pending and the next reaper tick retries naturally.
        if (temporary_display_generation_is_current(display)) {
          LeaseDisplayRequest pending {};
          pending.lease_id = display.lease_id;
          pending.display_id = display.display_id;
          (void) store_.mark_temporary_display_pending_departure(pending);
        }
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
    descriptor.generation = record.generation;
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
      case BackendError::Busy:
        return "busy";
    }

    return "unknown";
  }
}  // namespace virtual_display::driver
