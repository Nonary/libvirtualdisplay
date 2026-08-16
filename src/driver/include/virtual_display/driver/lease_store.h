#pragma once

#include "virtual_display/driver/control_protocol.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace virtual_display::driver {
  DisplayManifest display_manifest_from_permanent_settings(
    const PermanentDisplayCountRequest &request,
    std::uint32_t max_display_count
  );
  DisplayManifest apply_initial_remote_hdr_proof_intent(DisplayManifest manifest, bool enabled);
  PermanentDisplayCountRequest permanent_settings_from_display_manifest(const DisplayManifest &manifest);

  enum class StoreError {
    None,
    ValidationFailed,
    TemporaryDisplayLimitReached,
    DisplayAlreadyExists,
    DuplicateDisplayIdentity,
    LeaseNotFound,
    DisplayNotFound,
    PermanentDisplayCountTooHigh,
  };

  struct TemporaryDisplayRecord {
    std::uint64_t lease_id {};
    std::uint64_t display_id {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t physical_width_mm {};
    std::uint32_t physical_height_mm {};
    std::uint32_t refresh_rate_millihz {};
    std::uint32_t timeout_ms {};
    std::uint32_t connector_index {};
    std::string display_name {};
    std::chrono::steady_clock::time_point expires_at {};
    bool retain_identity {true};
    std::uint64_t identity_display_id {};
    std::uint64_t generation {};
    bool pending_departure {};
    bool initial_remote_hdr_requested {};
    std::uint32_t hdr_max_luminance_nits {kDefaultHdrMaxLuminanceNits};
  };

  struct StoreResult {
    StoreError error {StoreError::None};
    ValidationError validation_error {ValidationError::None};
  };

  struct CreateStoreResult {
    StoreResult status {};
    CreateTemporaryDisplayResult result {};
  };

  struct ReclaimStoreResult {
    StoreResult status {};
    ReclaimTemporaryDisplayResult result {};
  };

  enum class RemoveTemporaryDisplayMode {
    RetainConnectorReservation,
    ReleaseConnectorReservation,
  };

  class DisplayStore {
  public:
    explicit DisplayStore(
      std::uint32_t max_permanent_displays,
      std::uint32_t max_temporary_displays,
      std::map<std::uint64_t, std::uint32_t> connector_reservations_by_display_id = {},
      std::optional<DisplayManifest> initial_manifest = std::nullopt
    );

    [[nodiscard]] std::uint32_t max_permanent_displays() const;
    [[nodiscard]] std::uint32_t max_temporary_displays() const;
    [[nodiscard]] std::uint32_t permanent_display_count() const;
    [[nodiscard]] const PermanentDisplayCountRequest &permanent_display_settings() const;
    [[nodiscard]] const DisplayManifest &display_manifest() const;
    [[nodiscard]] std::uint32_t temporary_display_count() const;

    CreateStoreResult create_temporary_display(
      const CreateTemporaryDisplayRequest &request,
      std::chrono::steady_clock::time_point now
    );
    CreateStoreResult create_temporary_display_owned(
      const CreateTemporaryDisplayOwnedRequest &request,
      std::chrono::steady_clock::time_point now
    );
    ReclaimStoreResult reclaim_temporary_display(
      const ReclaimTemporaryDisplayRequest &request,
      std::chrono::steady_clock::time_point now
    );
    StoreResult remove_temporary_display(
      const LeaseDisplayRequest &request,
      RemoveTemporaryDisplayMode mode = RemoveTemporaryDisplayMode::RetainConnectorReservation
    );
    StoreResult mark_temporary_display_pending_departure(const LeaseDisplayRequest &request);
    // Evicts a condemned (pending-departure) record whose backend departure
    // keeps failing, on behalf of its proven owner. The record's connector is
    // quarantined until the driver restarts so the recreation cannot land on
    // the wedged connector; the reservation is dropped with the record.
    StoreResult abandon_pending_temporary_display(
      const LeaseDisplayRequest &request,
      std::uint64_t expected_generation,
      const OwnerCapability &owner_capability
    );
    StoreResult feed_lease(const LeaseRequest &request, std::chrono::steady_clock::time_point now);
    StoreResult release_lease(const LeaseRequest &request);
    QueryLeaseResult query_lease(std::uint64_t lease_id, std::chrono::steady_clock::time_point now) const;
    StoreResult set_permanent_display_count(const PermanentDisplayCountRequest &request);
    StoreResult apply_display_manifest(const DisplayManifest &manifest);
    StoreResult set_display_hdr_state(const SetDisplayHdrStateRequest &request);
    StoreResult set_display_mode(const SetDisplayModeRequest &request);
    PermanentDisplayCountResult query_permanent_display_count() const;
    std::uint32_t reap_expired(std::chrono::steady_clock::time_point now);

    [[nodiscard]] std::optional<TemporaryDisplayRecord> find_temporary_display(std::uint64_t display_id) const;
    [[nodiscard]] std::vector<TemporaryDisplayRecord> temporary_displays() const;
    [[nodiscard]] std::vector<TemporaryDisplayRecord> temporary_displays_for_lease(std::uint64_t lease_id) const;
    [[nodiscard]] std::vector<TemporaryDisplayRecord> pending_departure_temporary_displays() const;
    [[nodiscard]] std::vector<TemporaryDisplayRecord> expired_temporary_displays(std::chrono::steady_clock::time_point now) const;

  private:
    struct LeaseRecord {
      std::uint32_t timeout_ms {};
      std::chrono::steady_clock::time_point expires_at {};
      std::optional<OwnerCapability> owner_capability {};
    };

    CreateStoreResult create_temporary_display_impl(
      const CreateTemporaryDisplayRequest &request,
      const OwnerCapability *owner_capability,
      std::chrono::steady_clock::time_point now
    );

    bool is_temporary_connector_index(std::uint32_t connector_index) const;
    std::uint32_t temporary_connector_limit() const;
    bool connector_index_is_active(std::uint32_t connector_index) const;
    bool connector_index_is_reserved(std::uint32_t connector_index) const;
    bool connector_index_is_reserved_for_other_display(std::uint32_t connector_index, std::uint64_t display_id) const;
    bool edid_identity_is_active(std::uint64_t identity_display_id) const;
    void remove_connector_reservation(std::uint32_t connector_index, std::uint64_t except_display_id);
    std::uint32_t connector_index_for_display(std::uint64_t display_id, bool retain_identity);
    bool lease_has_displays(std::uint64_t lease_id) const;
    bool lease_has_pending_departure(std::uint64_t lease_id) const;
    bool lease_has_expired_pending_departure(
      std::uint64_t lease_id,
      std::chrono::steady_clock::time_point now
    ) const;
    void remove_lease_if_empty(std::uint64_t lease_id);

    std::uint32_t max_permanent_displays_ {};
    std::uint32_t max_temporary_displays_ {};
    std::uint32_t permanent_display_count_ {};
    PermanentDisplayCountRequest permanent_display_settings_ {};
    DisplayManifest display_manifest_ {};
    std::map<std::uint64_t, TemporaryDisplayRecord> displays_by_id_ {};
    std::map<std::uint64_t, LeaseRecord> leases_by_id_ {};
    std::map<std::uint64_t, std::uint32_t> connector_reservations_by_display_id_ {};
    std::set<std::uint32_t> quarantined_connectors_ {};
    std::uint64_t next_ephemeral_identity_ {1};
    std::uint64_t next_temporary_display_generation_ {1};
  };

  const char *to_string(StoreError error);
}  // namespace virtual_display::driver
