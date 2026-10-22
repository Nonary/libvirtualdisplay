#include "virtual_display/driver/frame_ring_lifetime.h"

namespace virtual_display::driver {
  FrameRingLifetime::FrameRingLifetime(const DisplayStore &store):
      store_ {store} {
  }

  FrameRingLifetime::~FrameRingLifetime() {
    reset();
  }

  bool FrameRingLifetime::authorized(
    const FrameRingOwner &owner,
    const std::chrono::steady_clock::time_point now
  ) const {
    const auto display = store_.authorize_temporary_display_owner(owner.display, owner.capability, now);
    return display && owner.display_generation != 0 && display->generation == owner.display_generation;
  }

  FrameRingRegistrationResult FrameRingLifetime::register_ring(
    const FrameRingOwner &owner,
    const std::shared_ptr<FrameRingResources> &resources,
    const std::chrono::steady_clock::time_point now
  ) {
    if (!authorized(owner, now)) {
      return {FrameRingRegistrationStatus::OwnerUnavailable, 0};
    }
    if (!resources) {
      return {FrameRingRegistrationStatus::InvalidResources, 0};
    }
    if (next_ring_generation_ == 0) {
      return {FrameRingRegistrationStatus::GenerationExhausted, 0};
    }
    // Expired/removed displays cannot accumulate registrations as display IDs
    // churn. Active registrations remain bounded by the store's display limit.
    retire_unavailable(now);
    const auto generation = next_ring_generation_;
    const auto previous = rings_.find(owner.display.display_id);
    const auto previous_publication = previous == rings_.end() ? nullptr : previous->second.snapshot.publication;
    if (previous_publication) {
      previous_publication->disable();
    }
    const auto publication = std::make_shared<FrameRingPublication>();
    rings_.insert_or_assign(owner.display.display_id, Entry {
      owner, {owner.display_generation, generation, resources, publication}
    });
    ++next_ring_generation_;
    return {FrameRingRegistrationStatus::Success, generation};
  }

  bool FrameRingLifetime::clear_ring(
    const FrameRingOwner &owner,
    const std::uint64_t ring_generation,
    const std::chrono::steady_clock::time_point now
  ) {
    return authorized(owner, now) &&
           retire_ring(owner.display.display_id, owner.display_generation, ring_generation);
  }

  FrameRingSnapshot FrameRingLifetime::snapshot(
    const std::uint64_t display_id,
    const std::uint64_t display_generation,
    const std::chrono::steady_clock::time_point now
  ) {
    const auto entry = rings_.find(display_id);
    if (entry == rings_.end() || entry->second.owner.display_generation != display_generation) {
      return {};
    }
    if (!authorized(entry->second.owner, now)) {
      entry->second.snapshot.retire_publication();
      rings_.erase(entry);
      return {};
    }
    return entry->second.snapshot;
  }

  bool FrameRingLifetime::retire_ring(
    const std::uint64_t display_id,
    const std::uint64_t display_generation,
    const std::uint64_t ring_generation
  ) {
    const auto entry = rings_.find(display_id);
    if (entry == rings_.end() || entry->second.snapshot.display_generation != display_generation ||
        entry->second.snapshot.ring_generation != ring_generation) {
      return false;
    }
    entry->second.snapshot.retire_publication();
    rings_.erase(entry);
    return true;
  }

  bool FrameRingLifetime::retire_display(const std::uint64_t display_id, const std::uint64_t display_generation) {
    const auto entry = rings_.find(display_id);
    if (entry == rings_.end() || entry->second.snapshot.display_generation != display_generation) {
      return false;
    }
    entry->second.snapshot.retire_publication();
    rings_.erase(entry);
    return true;
  }

  void FrameRingLifetime::reset() {
    for (const auto &[_, entry]: rings_) {
      entry.snapshot.retire_publication();
    }
    rings_.clear();
  }

  std::size_t FrameRingLifetime::retire_unavailable(const std::chrono::steady_clock::time_point now) {
    std::size_t retired = 0;
    for (auto entry = rings_.begin(); entry != rings_.end();) {
      if (!authorized(entry->second.owner, now)) {
        entry->second.snapshot.retire_publication();
        entry = rings_.erase(entry);
        ++retired;
      } else {
        ++entry;
      }
    }
    return retired;
  }
}  // namespace virtual_display::driver
