#pragma once
#include "virtual_display/driver/frame_ring_lifetime.h"
#include "virtual_display/driver/frame_transport_protocol.h"

namespace virtual_display::driver {
  // Obtained from the OS request context, never decoded from an IOCTL or pipe
  // payload. process_start distinguishes PID reuse. Native import must retain
  // the opened process handle used to check these values through duplication.
  struct FrameRequestPeer {
    std::uint64_t process_id {};
    std::uint64_t process_start {};
    friend bool operator==(const FrameRequestPeer &, const FrameRequestPeer &) = default;
    bool valid() const { return process_id != 0 && process_start != 0; }
  };

  class FrameResourceImporter {
  public:
    virtual ~FrameResourceImporter() = default;
    // Must duplicate from this exact authenticated process, validate all six
    // objects and their device/geometry/header, and return only owned resources.
    // Partial failure destroys local duplicates; source handles stay caller-owned.
    virtual std::shared_ptr<FrameRingResources> import(
      const FrameRequestPeer &peer, const FrameRingRegisterRequest &request) = 0;
  };

  enum class FrameWireStatus { Success, InvalidInput, Unauthorized, ImportFailed, StaleGeneration };
  struct FrameWireResult {
    FrameWireStatus status {FrameWireStatus::InvalidInput};
    FrameRingReply reply {};
  };

  // Control-plane prerequisite, serialized with DisplayStore and FrameRingLifetime.
  // Intentionally not connected to the native dispatcher; capabilities stay zero.
  class FrameRingServer {
  public:
    FrameRingServer(const DisplayStore &store, FrameRingLifetime &rings, FrameResourceImporter &importer);
    ~FrameRingServer();
    FrameWireResult register_ring(const FrameRequestPeer &peer, const FrameRingRegisterRequest &request,
                                  std::chrono::steady_clock::time_point now);
    FrameWireResult register_ring(const FrameRequestPeer &peer, const FrameRingRegisterRequest &request,
                                  std::chrono::steady_clock::time_point now, FrameResourceImporter &importer);
    std::optional<FrameTransportTarget> query_target(
      const FrameRequestPeer &peer, const FrameTransportTargetRequest &request,
      std::chrono::steady_clock::time_point now
    ) const;
    FrameWireStatus clear_ring(const FrameRequestPeer &peer, const FrameRingClearRequest &request,
                               std::chrono::steady_clock::time_point now);
    std::size_t reap(std::chrono::steady_clock::time_point now);
    // Retires resources and preserves live-lease cancellation tombstones.
    void reset();
    std::size_t size() const { return entries_.size(); }
  private:
    struct Entry { FrameRequestPeer peer; FrameRingRegisterRequest request; FrameRingReply reply; };
    // Entry survives clear as a high-water tombstone until lease/display expiry.
    // client_cookie must increase within one authenticated process and lease.
    const DisplayStore &store_;
    FrameRingLifetime &rings_;
    FrameResourceImporter &importer_;
    std::map<std::uint64_t, Entry> entries_;
  };
}
