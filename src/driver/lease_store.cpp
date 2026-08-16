#include "virtual_display/driver/lease_store.h"

#include "virtual_display/driver/display_identity.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace virtual_display::driver {
  namespace {
    StoreResult validation_failure(const ValidationError error) {
      return StoreResult {
        StoreError::ValidationFailed,
        error
      };
    }

    bool owner_capabilities_equal(const OwnerCapability &lhs, const OwnerCapability &rhs) {
      std::uint8_t difference = 0;
      for (std::size_t index = 0; index < lhs.bytes.size(); ++index) {
        difference |= static_cast<std::uint8_t>(lhs.bytes[index] ^ rhs.bytes[index]);
      }
      return difference == 0;
    }
  }  // namespace

  DisplayManifest display_manifest_from_permanent_settings(
    const PermanentDisplayCountRequest &request,
    const std::uint32_t max_display_count
  ) {
    auto normalized = request;
    set_default_permanent_display_settings(normalized);

    DisplayManifest manifest {};
    manifest.profile_count = normalized.display_count;
    manifest.max_profile_count = std::min(max_display_count, kMaxPermanentDisplayProfiles);
    for (std::uint32_t index = 0;
         index < manifest.profile_count && index < kMaxPermanentDisplayProfiles;
         ++index) {
      auto &profile = manifest.profiles[index];
      profile.flags = kDisplayManifestProfileFlagHdrSupported |
        kDisplayManifestProfileFlagRetainIdentity |
        kDisplayManifestProfileFlagPermanentIdentity;
      profile.connector_index = index;
      profile.display_id = permanent_display_id(index);
      profile.container_id = container_guid_from_display_id(profile.display_id);
      std::copy(
        std::begin(kSunshineDriverManufacturerId),
        std::end(kSunshineDriverManufacturerId),
        std::begin(profile.manufacturer_id)
      );
      profile.product_code = permanent_product_code(index);
      profile.serial_number = serial_number_from_display_id(profile.display_id);
      profile.physical_width_mm = normalized.physical_width_mm;
      profile.physical_height_mm = normalized.physical_height_mm;
      profile.native_mode_index = 0;
      profile.allowed_mode_count = 1;
      profile.layout_policy = kDisplayManifestLayoutPolicyNone;
      profile.orientation = kDisplayManifestOrientationDefault;
      profile.allowed_modes[0] = DisplayMode {
        normalized.width,
        normalized.height,
        normalized.refresh_rate_millihz
      };
      std::copy(
        std::begin(normalized.display_name),
        std::end(normalized.display_name),
        std::begin(profile.display_name)
      );
    }

    return manifest;
  }

  DisplayManifest apply_initial_remote_hdr_proof_intent(DisplayManifest manifest, const bool enabled) {
    if (enabled && manifest.profile_count != 0) {
      manifest.profiles[0].flags |= kDisplayManifestProfileFlagInitialRemoteHdr;
    }
    return manifest;
  }

  PermanentDisplayCountRequest permanent_settings_from_display_manifest(const DisplayManifest &manifest) {
    PermanentDisplayCountRequest request {};
    request.display_count = manifest.profile_count;
    if (manifest.profile_count == 0) {
      set_default_permanent_display_settings(request);
      return request;
    }

    const auto &profile = manifest.profiles[0];
    const auto &mode = profile.allowed_modes[profile.native_mode_index];
    request.width = mode.width;
    request.height = mode.height;
    request.physical_width_mm = profile.physical_width_mm;
    request.physical_height_mm = profile.physical_height_mm;
    request.refresh_rate_millihz = mode.refresh_rate_millihz;
    std::copy(
      std::begin(profile.display_name),
      std::end(profile.display_name),
      std::begin(request.display_name)
    );
    set_default_permanent_display_settings(request);
    return request;
  }

  DisplayStore::DisplayStore(
    const std::uint32_t max_permanent_displays,
    const std::uint32_t max_temporary_displays,
    std::map<std::uint64_t, std::uint32_t> connector_reservations_by_display_id,
    std::optional<DisplayManifest> initial_manifest
  ):
      max_permanent_displays_ {max_permanent_displays},
      max_temporary_displays_ {max_temporary_displays},
      connector_reservations_by_display_id_ {std::move(connector_reservations_by_display_id)} {
    set_default_permanent_display_settings(permanent_display_settings_);
    display_manifest_ = display_manifest_from_permanent_settings(
      permanent_display_settings_,
      max_permanent_displays_
    );
    if (initial_manifest &&
        validate_display_manifest(*initial_manifest, max_permanent_displays_) == ValidationError::None) {
      (void) apply_display_manifest(*initial_manifest);
    }
  }

  std::uint32_t DisplayStore::max_permanent_displays() const {
    return max_permanent_displays_;
  }

  std::uint32_t DisplayStore::max_temporary_displays() const {
    return max_temporary_displays_;
  }

  std::uint32_t DisplayStore::permanent_display_count() const {
    return permanent_display_count_;
  }

  const PermanentDisplayCountRequest &DisplayStore::permanent_display_settings() const {
    return permanent_display_settings_;
  }

  const DisplayManifest &DisplayStore::display_manifest() const {
    return display_manifest_;
  }

  std::uint32_t DisplayStore::temporary_display_count() const {
    return static_cast<std::uint32_t>(displays_by_id_.size());
  }

  CreateStoreResult DisplayStore::create_temporary_display(
    const CreateTemporaryDisplayRequest &request,
    const std::chrono::steady_clock::time_point now
  ) {
    return create_temporary_display_impl(request, nullptr, now);
  }

  CreateStoreResult DisplayStore::create_temporary_display_owned(
    const CreateTemporaryDisplayOwnedRequest &request,
    const std::chrono::steady_clock::time_point now
  ) {
    if (const auto validation = validate_create_temporary_display_owned(request);
        validation != ValidationError::None) {
      return CreateStoreResult {validation_failure(validation), {}};
    }
    return create_temporary_display_impl(request.display, &request.owner_capability, now);
  }

  CreateStoreResult DisplayStore::create_temporary_display_impl(
    const CreateTemporaryDisplayRequest &request,
    const OwnerCapability *owner_capability,
    const std::chrono::steady_clock::time_point now
  ) {
    ValidatedCreateTemporaryDisplay validated {};
    if (const auto validation = validate_create_temporary_display(request, &validated);
        validation != ValidationError::None) {
      return CreateStoreResult {validation_failure(validation), {}};
    }

    if (displays_by_id_.contains(request.display_id)) {
      return CreateStoreResult {{StoreError::DisplayAlreadyExists, ValidationError::None}, {}};
    }

    if (displays_by_id_.size() >= max_temporary_displays_) {
      return CreateStoreResult {{StoreError::TemporaryDisplayLimitReached, ValidationError::None}, {}};
    }

    const auto lease = leases_by_id_.find(request.lease_id);
    if (lease != leases_by_id_.end() &&
        (lease->second.expires_at <= now || lease_has_pending_departure(request.lease_id))) {
      return CreateStoreResult {{StoreError::LeaseNotFound, ValidationError::None}, {}};
    }
    if (lease != leases_by_id_.end() && owner_capability &&
        lease->second.owner_capability &&
        !owner_capabilities_equal(*lease->second.owner_capability, *owner_capability)) {
      return CreateStoreResult {{StoreError::LeaseNotFound, ValidationError::None}, {}};
    }

    const bool retain_identity = (validated.request.flags & kCreateTemporaryDisplayFlagEphemeralIdentity) == 0;
    const auto connector_index = connector_index_for_display(request.display_id, retain_identity);
    if (!is_temporary_connector_index(connector_index)) {
      return CreateStoreResult {{StoreError::TemporaryDisplayLimitReached, ValidationError::None}, {}};
    }

    constexpr std::uint64_t kEphemeralDisplayIdBase = 0x6000000000000000ull;
    auto identity_display_id = request.display_id;
    if (retain_identity) {
      if (edid_identity_is_active(identity_display_id)) {
        return CreateStoreResult {{StoreError::DuplicateDisplayIdentity, ValidationError::None}, {}};
      }
    } else {
      do {
        identity_display_id = kEphemeralDisplayIdBase | next_ephemeral_identity_++;
      } while (edid_identity_is_active(identity_display_id));
    }

    if (retain_identity) {
      remove_connector_reservation(connector_index, request.display_id);
      connector_reservations_by_display_id_[request.display_id] = connector_index;
    }
    auto effective_timeout_ms = validated.effective_timeout_ms;
    auto expires_at = now + std::chrono::milliseconds(effective_timeout_ms);
    if (lease != leases_by_id_.end()) {
      effective_timeout_ms = lease->second.timeout_ms;
      expires_at = lease->second.expires_at;
    }

    displays_by_id_.emplace(
      request.display_id,
      TemporaryDisplayRecord {
        request.lease_id,
        request.display_id,
        validated.request.width,
        validated.request.height,
        validated.request.physical_width_mm,
        validated.request.physical_height_mm,
        validated.request.refresh_rate_millihz,
        effective_timeout_ms,
        connector_index,
        std::string {validated.display_name},
        expires_at,
        retain_identity,
        identity_display_id,
        next_temporary_display_generation_++,
        false,
        (validated.request.flags & kCreateTemporaryDisplayFlagInitialRemoteHdr) != 0,
        validated.request.hdr_max_luminance_nits
      }
    );

    if (lease == leases_by_id_.end()) {
      leases_by_id_.emplace(
        request.lease_id,
        LeaseRecord {
          effective_timeout_ms,
          expires_at,
          owner_capability ? std::optional<OwnerCapability> {*owner_capability} : std::nullopt
        }
      );
    } else if (owner_capability && !lease->second.owner_capability) {
      lease->second.owner_capability = *owner_capability;
    }

    CreateTemporaryDisplayResult result {};
    result.lease_id = request.lease_id;
    result.display_id = request.display_id;
    result.connector_index = connector_index;
    result.effective_timeout_ms = effective_timeout_ms;

    return CreateStoreResult {{}, result};
  }

  ReclaimStoreResult DisplayStore::reclaim_temporary_display(
    const ReclaimTemporaryDisplayRequest &request,
    const std::chrono::steady_clock::time_point now
  ) {
    if (const auto validation = validate_reclaim_temporary_display(request);
        validation != ValidationError::None) {
      return ReclaimStoreResult {validation_failure(validation), {}};
    }

    const auto display = displays_by_id_.find(request.display_id);
    if (display == displays_by_id_.end() || display->second.pending_departure) {
      return ReclaimStoreResult {{StoreError::LeaseNotFound, ValidationError::None}, {}};
    }

    const auto old_lease_id = display->second.lease_id;
    auto lease = leases_by_id_.find(old_lease_id);
    if (lease == leases_by_id_.end() ||
        lease->second.expires_at <= now ||
        lease_has_pending_departure(old_lease_id) ||
        !lease->second.owner_capability ||
        !owner_capabilities_equal(*lease->second.owner_capability, request.owner_capability)) {
      return ReclaimStoreResult {{StoreError::LeaseNotFound, ValidationError::None}, {}};
    }

    // Reclaim is a fencing operation. Reusing the old lease would leave a stale
    // process authorized to feed or remove the recovered display.
    if (request.new_lease_id == old_lease_id || leases_by_id_.contains(request.new_lease_id)) {
      return ReclaimStoreResult {{StoreError::LeaseNotFound, ValidationError::None}, {}};
    }

    const auto timeout_ms = normalize_timeout_ms(request.requested_timeout_ms);
    const auto expires_at = now + std::chrono::milliseconds(timeout_ms);
    const auto capability = *lease->second.owner_capability;

    std::uint32_t display_count = 0;
    for (auto &[_, record]: displays_by_id_) {
      if (record.lease_id != old_lease_id) {
        continue;
      }
      record.lease_id = request.new_lease_id;
      record.timeout_ms = timeout_ms;
      record.expires_at = expires_at;
      ++display_count;
    }

    leases_by_id_.erase(lease);
    leases_by_id_.emplace(
      request.new_lease_id,
      LeaseRecord {
        timeout_ms,
        expires_at,
        capability
      }
    );

    ReclaimTemporaryDisplayResult result {};
    result.lease_id = request.new_lease_id;
    result.display_id = request.display_id;
    result.connector_index = display->second.connector_index;
    result.effective_timeout_ms = timeout_ms;
    result.temporary_display_count = display_count;
    return ReclaimStoreResult {{}, result};
  }

  StoreResult DisplayStore::remove_temporary_display(
    const LeaseDisplayRequest &request,
    const RemoveTemporaryDisplayMode mode
  ) {
    if (const auto validation = validate_lease_display_request(request);
        validation != ValidationError::None) {
      return validation_failure(validation);
    }

    auto display = displays_by_id_.find(request.display_id);
    if (display == displays_by_id_.end() || display->second.lease_id != request.lease_id) {
      return {StoreError::DisplayNotFound, ValidationError::None};
    }

    if (mode == RemoveTemporaryDisplayMode::ReleaseConnectorReservation) {
      connector_reservations_by_display_id_.erase(request.display_id);
    }

    displays_by_id_.erase(display);
    remove_lease_if_empty(request.lease_id);
    return {};
  }

  StoreResult DisplayStore::abandon_pending_temporary_display(
    const LeaseDisplayRequest &request,
    const std::uint64_t expected_generation,
    const OwnerCapability &owner_capability
  ) {
    if (const auto validation = validate_lease_display_request(request);
        validation != ValidationError::None) {
      return validation_failure(validation);
    }

    const auto display = displays_by_id_.find(request.display_id);
    if (display == displays_by_id_.end() ||
        display->second.lease_id != request.lease_id ||
        display->second.generation != expected_generation ||
        !display->second.pending_departure) {
      return {StoreError::DisplayNotFound, ValidationError::None};
    }

    // Only the proven owner of the condemned record's lease may evict it. A
    // lease without a stored capability (plain create) cannot be
    // owner-verified and stays fail-closed.
    const auto lease = leases_by_id_.find(request.lease_id);
    if (lease == leases_by_id_.end() ||
        !lease->second.owner_capability ||
        !owner_capabilities_equal(*lease->second.owner_capability, owner_capability)) {
      return {StoreError::LeaseNotFound, ValidationError::None};
    }

    quarantined_connectors_.insert(display->second.connector_index);
    connector_reservations_by_display_id_.erase(request.display_id);
    displays_by_id_.erase(display);
    remove_lease_if_empty(request.lease_id);
    return {};
  }

  StoreResult DisplayStore::mark_temporary_display_pending_departure(const LeaseDisplayRequest &request) {
    if (const auto validation = validate_lease_display_request(request);
        validation != ValidationError::None) {
      return validation_failure(validation);
    }

    auto display = displays_by_id_.find(request.display_id);
    if (display == displays_by_id_.end() || display->second.lease_id != request.lease_id) {
      return {StoreError::DisplayNotFound, ValidationError::None};
    }

    display->second.pending_departure = true;
    return {};
  }

  StoreResult DisplayStore::feed_lease(
    const LeaseRequest &request,
    const std::chrono::steady_clock::time_point now
  ) {
    if (const auto validation = validate_lease_request(request);
        validation != ValidationError::None) {
      return validation_failure(validation);
    }

    auto lease = leases_by_id_.find(request.lease_id);
    if (lease == leases_by_id_.end()) {
      return {StoreError::LeaseNotFound, ValidationError::None};
    }

    if (lease->second.expires_at <= now || lease_has_expired_pending_departure(request.lease_id, now)) {
      return {StoreError::LeaseNotFound, ValidationError::None};
    }

    const auto timeout_ms = normalize_timeout_ms(request.requested_timeout_ms);
    const auto expires_at = now + std::chrono::milliseconds(timeout_ms);
    lease->second.timeout_ms = timeout_ms;
    lease->second.expires_at = expires_at;

    for (auto &[_, display]: displays_by_id_) {
      if (display.lease_id == request.lease_id && !display.pending_departure) {
        display.timeout_ms = timeout_ms;
        display.expires_at = expires_at;
      }
    }

    return {};
  }

  StoreResult DisplayStore::release_lease(const LeaseRequest &request) {
    if (const auto validation = validate_lease_request(request);
        validation != ValidationError::None) {
      return validation_failure(validation);
    }

    if (!leases_by_id_.contains(request.lease_id)) {
      return {StoreError::LeaseNotFound, ValidationError::None};
    }

    for (auto it = displays_by_id_.begin(); it != displays_by_id_.end();) {
      if (it->second.lease_id == request.lease_id) {
        it = displays_by_id_.erase(it);
      } else {
        ++it;
      }
    }

    leases_by_id_.erase(request.lease_id);
    return {};
  }

  QueryLeaseResult DisplayStore::query_lease(
    const std::uint64_t lease_id,
    const std::chrono::steady_clock::time_point now
  ) const {
    QueryLeaseResult result {};
    result.lease_id = lease_id;

    const auto lease = leases_by_id_.find(lease_id);
    if (lease == leases_by_id_.end()) {
      return result;
    }

    result.lease_exists = 1;
    result.effective_timeout_ms = lease->second.timeout_ms;
    result.remaining_ms = static_cast<std::uint32_t>(
      std::max<std::int64_t>(
        0,
        std::chrono::duration_cast<std::chrono::milliseconds>(lease->second.expires_at - now).count()
      )
    );

    result.temporary_display_count = static_cast<std::uint32_t>(
      std::count_if(displays_by_id_.begin(), displays_by_id_.end(), [lease_id](const auto &entry) {
        return entry.second.lease_id == lease_id;
      })
    );
    result.pending_departure_count = static_cast<std::uint32_t>(
      std::count_if(displays_by_id_.begin(), displays_by_id_.end(), [lease_id](const auto &entry) {
        return entry.second.lease_id == lease_id && entry.second.pending_departure;
      })
    );

    return result;
  }

  StoreResult DisplayStore::set_permanent_display_count(const PermanentDisplayCountRequest &request) {
    auto normalized = request;
    set_default_permanent_display_settings(normalized);
    if (const auto validation = validate_permanent_display_count(normalized, max_permanent_displays_);
        validation != ValidationError::None) {
      return validation_failure(validation);
    }

    return apply_display_manifest(display_manifest_from_permanent_settings(normalized, max_permanent_displays_));
  }

  StoreResult DisplayStore::apply_display_manifest(const DisplayManifest &manifest) {
    if (const auto validation = validate_display_manifest(manifest, max_permanent_displays_);
        validation != ValidationError::None) {
      return validation_failure(validation);
    }

    auto canonical = manifest;
    for (std::uint32_t index = 0; index < canonical.profile_count; ++index) {
      char display_name[kDisplayNameChars] {};
      if (!canonicalize_display_name(canonical.profiles[index].display_name, display_name)) {
        return validation_failure(ValidationError::InvalidDisplayName);
      }
      std::copy(
        std::begin(display_name),
        std::end(display_name),
        std::begin(canonical.profiles[index].display_name)
      );
    }

    permanent_display_count_ = canonical.profile_count;
    display_manifest_ = canonical;
    permanent_display_settings_ = permanent_settings_from_display_manifest(canonical);
    return {};
  }

  StoreResult DisplayStore::set_display_mode(const SetDisplayModeRequest &request) {
    if (const auto validation = validate_set_display_mode(request);
        validation != ValidationError::None) {
      return validation_failure(validation);
    }

    for (std::uint32_t index = 0; index < display_manifest_.profile_count; ++index) {
      auto &profile = display_manifest_.profiles[index];
      if (profile.display_id != request.display_id) {
        continue;
      }

      profile.native_mode_index = 0;
      profile.allowed_mode_count = 1;
      profile.allowed_modes[0] = {request.width, request.height, request.refresh_rate_millihz};
      permanent_display_settings_ = permanent_settings_from_display_manifest(display_manifest_);
      return {};
    }

    if (auto display = displays_by_id_.find(request.display_id);
        display != displays_by_id_.end() && !display->second.pending_departure) {
      display->second.width = request.width;
      display->second.height = request.height;
      display->second.refresh_rate_millihz = request.refresh_rate_millihz;
      return {};
    }

    return {StoreError::DisplayNotFound, ValidationError::None};
  }

  StoreResult DisplayStore::set_display_hdr_state(const SetDisplayHdrStateRequest &request) {
    if (const auto validation = validate_set_display_hdr_state(request);
        validation != ValidationError::None) {
      return validation_failure(validation);
    }
    if (auto display = displays_by_id_.find(request.display_id);
        display != displays_by_id_.end() && !display->second.pending_departure) {
      display->second.initial_remote_hdr_requested = request.enabled != 0;
      return {};
    }
    return {StoreError::DisplayNotFound, ValidationError::None};
  }

  PermanentDisplayCountResult DisplayStore::query_permanent_display_count() const {
    PermanentDisplayCountResult result {};
    result.current_display_count = permanent_display_count_;
    result.max_display_count = max_permanent_displays_;
    result.temporary_display_count = temporary_display_count();
    result.width = permanent_display_settings_.width;
    result.height = permanent_display_settings_.height;
    result.physical_width_mm = permanent_display_settings_.physical_width_mm;
    result.physical_height_mm = permanent_display_settings_.physical_height_mm;
    result.refresh_rate_millihz = permanent_display_settings_.refresh_rate_millihz;
    std::copy(
      std::begin(permanent_display_settings_.display_name),
      std::end(permanent_display_settings_.display_name),
      std::begin(result.display_name)
    );
    return result;
  }

  std::uint32_t DisplayStore::reap_expired(const std::chrono::steady_clock::time_point now) {
    std::uint32_t removed_count = 0;

    for (auto it = displays_by_id_.begin(); it != displays_by_id_.end();) {
      if (it->second.expires_at <= now) {
        const auto lease_id = it->second.lease_id;
        it = displays_by_id_.erase(it);
        ++removed_count;
        remove_lease_if_empty(lease_id);
      } else {
        ++it;
      }
    }

    return removed_count;
  }

  std::optional<TemporaryDisplayRecord> DisplayStore::find_temporary_display(const std::uint64_t display_id) const {
    const auto display = displays_by_id_.find(display_id);
    if (display == displays_by_id_.end()) {
      return std::nullopt;
    }

    return display->second;
  }

  std::vector<TemporaryDisplayRecord> DisplayStore::temporary_displays() const {
    std::vector<TemporaryDisplayRecord> displays;
    displays.reserve(displays_by_id_.size());
    for (const auto &[_, display]: displays_by_id_) {
      displays.push_back(display);
    }

    return displays;
  }

  std::vector<TemporaryDisplayRecord> DisplayStore::temporary_displays_for_lease(const std::uint64_t lease_id) const {
    std::vector<TemporaryDisplayRecord> displays;
    for (const auto &[_, display]: displays_by_id_) {
      if (display.lease_id == lease_id) {
        displays.push_back(display);
      }
    }

    return displays;
  }

  std::vector<TemporaryDisplayRecord> DisplayStore::pending_departure_temporary_displays() const {
    std::vector<TemporaryDisplayRecord> displays;
    for (const auto &[_, display]: displays_by_id_) {
      if (display.pending_departure) {
        displays.push_back(display);
      }
    }

    return displays;
  }

  std::vector<TemporaryDisplayRecord> DisplayStore::expired_temporary_displays(
    const std::chrono::steady_clock::time_point now
  ) const {
    std::vector<TemporaryDisplayRecord> displays;
    for (const auto &[_, display]: displays_by_id_) {
      if (display.expires_at <= now) {
        displays.push_back(display);
      }
    }

    return displays;
  }

  bool DisplayStore::is_temporary_connector_index(const std::uint32_t connector_index) const {
    const auto limit = temporary_connector_limit();
    return connector_index >= max_permanent_displays_ &&
           connector_index < limit;
  }

  std::uint32_t DisplayStore::temporary_connector_limit() const {
    if (max_temporary_displays_ > (std::numeric_limits<std::uint32_t>::max)() - max_permanent_displays_) {
      return (std::numeric_limits<std::uint32_t>::max)();
    }
    return max_permanent_displays_ + max_temporary_displays_;
  }

  bool DisplayStore::connector_index_is_active(const std::uint32_t connector_index) const {
    return std::any_of(displays_by_id_.begin(), displays_by_id_.end(), [connector_index](const auto &entry) {
      return entry.second.connector_index == connector_index;
    });
  }

  bool DisplayStore::connector_index_is_reserved(const std::uint32_t connector_index) const {
    return std::any_of(
      connector_reservations_by_display_id_.begin(),
      connector_reservations_by_display_id_.end(),
      [connector_index](const auto &entry) {
        return entry.second == connector_index;
      }
    );
  }

  bool DisplayStore::connector_index_is_reserved_for_other_display(
    const std::uint32_t connector_index,
    const std::uint64_t display_id
  ) const {
    return std::any_of(
      connector_reservations_by_display_id_.begin(),
      connector_reservations_by_display_id_.end(),
      [connector_index, display_id](const auto &entry) {
        return entry.first != display_id && entry.second == connector_index;
      }
    );
  }

  bool DisplayStore::edid_identity_is_active(const std::uint64_t identity_display_id) const {
    const auto candidate_product_code = product_code_from_display_id(identity_display_id);
    const auto candidate_serial_number = serial_number_from_display_id(identity_display_id);
    return std::any_of(
      displays_by_id_.begin(),
      displays_by_id_.end(),
      [&](const auto &entry) {
        const auto active_identity_display_id = entry.second.identity_display_id == 0 ?
          entry.second.display_id :
          entry.second.identity_display_id;
        return product_code_from_display_id(active_identity_display_id) == candidate_product_code &&
               serial_number_from_display_id(active_identity_display_id) == candidate_serial_number;
      }
    );
  }

  void DisplayStore::remove_connector_reservation(
    const std::uint32_t connector_index,
    const std::uint64_t except_display_id
  ) {
    for (auto it = connector_reservations_by_display_id_.begin();
         it != connector_reservations_by_display_id_.end();) {
      if (it->first != except_display_id && it->second == connector_index) {
        it = connector_reservations_by_display_id_.erase(it);
      } else {
        ++it;
      }
    }
  }

  std::uint32_t DisplayStore::connector_index_for_display(
    const std::uint64_t display_id,
    const bool retain_identity
  ) {
    if (retain_identity) {
      if (const auto reservation = connector_reservations_by_display_id_.find(display_id);
          reservation != connector_reservations_by_display_id_.end() &&
          is_temporary_connector_index(reservation->second) &&
          !connector_index_is_active(reservation->second)) {
        return reservation->second;
      }
    }

    const auto connector_limit = temporary_connector_limit();
    for (std::uint32_t connector_index = max_permanent_displays_;
         connector_index < connector_limit;
         ++connector_index) {
      if (!connector_index_is_active(connector_index) &&
          !quarantined_connectors_.contains(connector_index) &&
          (retain_identity ?
             !connector_index_is_reserved_for_other_display(connector_index, display_id) :
             !connector_index_is_reserved(connector_index))) {
        return connector_index;
      }
    }

    for (std::uint32_t connector_index = max_permanent_displays_;
         connector_index < connector_limit;
         ++connector_index) {
      if (!connector_index_is_active(connector_index) &&
          !quarantined_connectors_.contains(connector_index)) {
        remove_connector_reservation(connector_index, display_id);
        return connector_index;
      }
    }

    return connector_limit;
  }

  bool DisplayStore::lease_has_displays(const std::uint64_t lease_id) const {
    return std::any_of(displays_by_id_.begin(), displays_by_id_.end(), [lease_id](const auto &entry) {
      return entry.second.lease_id == lease_id;
    });
  }

  bool DisplayStore::lease_has_pending_departure(const std::uint64_t lease_id) const {
    return std::any_of(displays_by_id_.begin(), displays_by_id_.end(), [lease_id](const auto &entry) {
      return entry.second.lease_id == lease_id && entry.second.pending_departure;
    });
  }

  bool DisplayStore::lease_has_expired_pending_departure(
    const std::uint64_t lease_id,
    const std::chrono::steady_clock::time_point now
  ) const {
    return std::any_of(displays_by_id_.begin(), displays_by_id_.end(), [lease_id, now](const auto &entry) {
      return entry.second.lease_id == lease_id && entry.second.pending_departure && entry.second.expires_at <= now;
    });
  }

  void DisplayStore::remove_lease_if_empty(const std::uint64_t lease_id) {
    if (!lease_has_displays(lease_id)) {
      leases_by_id_.erase(lease_id);
    }
  }

  const char *to_string(const StoreError error) {
    switch (error) {
      case StoreError::None:
        return "none";
      case StoreError::ValidationFailed:
        return "validation_failed";
      case StoreError::TemporaryDisplayLimitReached:
        return "temporary_display_limit_reached";
      case StoreError::DisplayAlreadyExists:
        return "display_already_exists";
      case StoreError::DuplicateDisplayIdentity:
        return "duplicate_display_identity";
      case StoreError::LeaseNotFound:
        return "lease_not_found";
      case StoreError::DisplayNotFound:
        return "display_not_found";
      case StoreError::PermanentDisplayCountTooHigh:
        return "permanent_display_count_too_high";
    }

    return "unknown";
  }
}  // namespace virtual_display::driver
