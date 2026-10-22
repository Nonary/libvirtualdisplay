#pragma once

#include "virtual_display/driver/lease_store.h"

#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <limits>
#include <utility>

namespace virtual_display::driver {
  // Internal C++ contract, not an IOCTL payload. Raw handles must never reach
  // this layer. A future Windows importer must own validated driver-local handle
  // duplicates and opened GPU objects before constructing a resource bundle.
  class FrameRingResources {
  public:
    virtual ~FrameRingResources() = default;
  };

  // Publication and retirement share this small per-ring gate. A producer
  // holds mutex only while publishing metadata, releasing the keyed mutex and
  // signaling the event; it never holds it during GPU copy/wait operations.
  // Retirement takes the same mutex before disabling the gate, so no frame can
  // be published after retirement returns. Sequence state belongs to the ring
  // lifetime, not to an IddCx swapchain worker that may be reassigned.
  struct FrameRingPublication {
    std::mutex mutex {};
    std::atomic<bool> enabled {true};
    std::atomic<std::uint64_t> sequence {};

    void disable() {
      std::lock_guard lock {mutex};
      enabled.store(false, std::memory_order_release);
    }

    bool next_sequence(std::uint64_t &out) {
      auto current = sequence.load(std::memory_order_relaxed);
      constexpr auto last_valid = (std::numeric_limits<std::uint64_t>::max)() - 1;
      while (current < last_valid) {
        if (sequence.compare_exchange_weak(current, current + 1,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {
          out = current + 1;
          return true;
        }
      }
      return false;
    }
  };

  struct FrameRingOwner {
    LeaseDisplayRequest display {};
    std::uint64_t display_generation {};
    OwnerCapability capability {};
  };

  enum class FrameRingRegistrationStatus {
    Success,
    OwnerUnavailable,
    InvalidResources,
    GenerationExhausted,
  };

  struct FrameRingRegistrationResult {
    FrameRingRegistrationStatus status {FrameRingRegistrationStatus::Success};
    std::uint64_t ring_generation {};
  };

  struct FrameRingSnapshot {
    std::uint64_t display_generation {};
    std::uint64_t ring_generation {};
    // In-flight native work retains this snapshot until its final GPU access
    // completes, even when the registration is cleared or replaced meanwhile.
    std::shared_ptr<FrameRingResources> resources {};
    // A producer can check this on its hot path without calling DisplayStore
    // or acquiring the control-plane lock. Already queued GPU work retains its
    // resources; the native publication protocol must also fence generations.
    std::shared_ptr<FrameRingPublication> publication {};

    [[nodiscard]] bool can_publish() const {
      return publication && publication->enabled.load(std::memory_order_acquire);
    }

    bool next_sequence(std::uint64_t &out) const {
      return can_publish() && publication->next_sequence(out);
    }

    void retire_publication() const {
      if (publication) publication->disable();
    }
  };

  // Release an acquired slot when publication is canceled or fails. The short
  // callback stays inside the retirement gate: a live ring restores its prior
  // key, while a retired ring returns the slot to producer key 0 and cannot
  // expose it to a consumer after clear/replacement has returned.
  template<typename Release>
  bool release_unpublished_slot(const FrameRingSnapshot &snapshot, const std::uint64_t acquired_key,
                                Release &&release) {
    if (!snapshot.publication) return false;
    std::lock_guard lock {snapshot.publication->mutex};
    const auto release_key = snapshot.can_publish() ? acquired_key : 0;
    return std::forward<Release>(release)(release_key);
  }

  // Called under the same external serialization as DisplayStore. This class
  // performs no GPU waits or per-frame queueing. Retirement may synchronize
  // with the bounded publication/release gate; it cannot invalidate snapshots.
  // Native bundle destructors must close owned resources without waiting for the
  // consumer. A producer snapshot must not outlive its GPU work's resource hold.
  class FrameRingLifetime {
  public:
    explicit FrameRingLifetime(const DisplayStore &store);
    ~FrameRingLifetime();
    FrameRingLifetime(const FrameRingLifetime &) = delete;
    FrameRingLifetime &operator=(const FrameRingLifetime &) = delete;

    FrameRingRegistrationResult register_ring(
      const FrameRingOwner &owner,
      const std::shared_ptr<FrameRingResources> &resources,
      std::chrono::steady_clock::time_point now
    );
    bool clear_ring(
      const FrameRingOwner &owner,
      std::uint64_t ring_generation,
      std::chrono::steady_clock::time_point now
    );
    FrameRingSnapshot snapshot(
      std::uint64_t display_id,
      std::uint64_t display_generation,
      std::chrono::steady_clock::time_point now
    );
    // A completion/error from an old producer must not retire a replacement.
    bool retire_ring(std::uint64_t display_id, std::uint64_t display_generation, std::uint64_t ring_generation);
    bool retire_display(std::uint64_t display_id, std::uint64_t display_generation);
    // The native reaper must call this even if no producer asks for a snapshot.
    std::size_t retire_unavailable(std::chrono::steady_clock::time_point now);
    void reset();

  private:
    bool authorized(const FrameRingOwner &owner, std::chrono::steady_clock::time_point now) const;

    struct Entry {
      FrameRingOwner owner {};
      FrameRingSnapshot snapshot {};
    };
    const DisplayStore &store_;
    std::map<std::uint64_t, Entry> rings_ {};
    // Deliberately not reset by reset(): a late clear/error cannot match a later
    // registration during the same device lifetime. Zero is never issued.
    std::uint64_t next_ring_generation_ {1};
  };
}  // namespace virtual_display::driver
