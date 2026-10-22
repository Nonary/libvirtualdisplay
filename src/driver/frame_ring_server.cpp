#include "virtual_display/driver/frame_ring_server.h"

namespace virtual_display::driver {
  namespace {
    FrameRingOwner owner_for(const FrameRingRegisterRequest &r) {
      FrameRingOwner o {};
      o.display.lease_id = r.lease_id;
      o.display.display_id = r.display_id;
      o.display_generation = r.display_generation;
      o.capability = r.owner_capability;
      return o;
    }
  }
  FrameRingServer::FrameRingServer(const DisplayStore &store, FrameRingLifetime &rings, FrameResourceImporter &importer):
      store_(store), rings_(rings), importer_(importer) {}
  FrameRingServer::~FrameRingServer() { reset(); }

  FrameWireResult FrameRingServer::register_ring(const FrameRequestPeer &peer, const FrameRingRegisterRequest &r,
                                               std::chrono::steady_clock::time_point now) {
    return register_ring(peer, r, now, importer_);
  }

  FrameWireResult FrameRingServer::register_ring(const FrameRequestPeer &peer, const FrameRingRegisterRequest &r,
                                               std::chrono::steady_clock::time_point now,
                                               FrameResourceImporter &importer) {
    if (!peer.valid() || !valid_frame_ring_request(r)) return {};
    const auto owner = owner_for(r);
    const auto display = store_.authorize_temporary_display_owner(owner.display, owner.capability, now);
    if (!display || display->generation != r.display_generation) return {FrameWireStatus::Unauthorized, {}};
    reap(now);
    const auto old = entries_.find(r.display_id);
    if (old != entries_.end()) {
      // A new process must reclaim a fresh lease before binding. Otherwise a
      // delayed request from the previous process could replace its successor.
      if (old->second.peer != peer) return {FrameWireStatus::Unauthorized, {}};
      if (r.client_cookie <= old->second.request.client_cookie) {
        if (r.client_cookie == old->second.request.client_cookie && old->second.reply.ring_generation &&
            same_frame_ring_request(old->second.request, r)) {
          return {FrameWireStatus::Success, old->second.reply};
        }
        return {FrameWireStatus::StaleGeneration, {}};
      }
    }
    // Reserve the bounded high-water entry before importing/registering. Failed
    // imports also consume the cookie, so a late duplicate can never resurrect.
    auto entry = entries_.try_emplace(r.display_id, Entry {peer, r, {}}).first;
    const auto previous = entry->second;
    entry->second = {peer, r, {}};
    std::shared_ptr<FrameRingResources> resources;
    try { resources = importer.import(peer, r); }
    catch (...) { /* native partial imports use RAII; fail closed */ }
    if (!resources) {
      // A failed replacement retires the old ring, rather than leaving its old
      // identity active behind the newer tombstone.
      rings_.retire_ring(r.display_id, previous.request.display_generation, previous.reply.ring_generation);
      return {FrameWireStatus::ImportFailed, {}};
    }
    const auto registered = rings_.register_ring(owner, resources, now);
    if (registered.status != FrameRingRegistrationStatus::Success) {
      rings_.retire_ring(r.display_id, previous.request.display_generation, previous.reply.ring_generation);
      return {FrameWireStatus::Unauthorized, {}};
    }
    FrameRingReply reply {};
    reply.client_cookie = r.client_cookie;
    reply.display_generation = r.display_generation;
    reply.ring_generation = registered.ring_generation;
    entry->second.reply = reply;
    return {FrameWireStatus::Success, reply};
  }

  std::optional<FrameTransportTarget> FrameRingServer::query_target(
    const FrameRequestPeer &peer, const FrameTransportTargetRequest &request,
    const std::chrono::steady_clock::time_point now
  ) const {
    if (!peer.valid() || !valid_frame_transport_target_request(request)) return {};
    LeaseDisplayRequest owner {};
    owner.lease_id = request.lease_id;
    owner.display_id = request.display_id;
    const auto display = store_.authorize_temporary_display_owner(owner, request.owner_capability, now);
    if (!display || display->pending_departure || !display->generation) return {};
    FrameTransportTarget target {};
    target.lease_id = display->lease_id;
    target.display_id = display->display_id;
    target.display_generation = display->generation;
    target.width = display->width;
    target.height = display->height;
    return target;
  }

  FrameWireStatus FrameRingServer::clear_ring(const FrameRequestPeer &peer, const FrameRingClearRequest &r,
                                            std::chrono::steady_clock::time_point now) {
    if (!peer.valid() || r.reserved || r.flags > 1 ||
        !is_valid_api_namespace(r.identity.api_namespace) || r.identity.version != kFrameRingWireVersion ||
        r.identity.size != sizeof(FrameRingReply) || !r.identity.client_cookie || !r.identity.display_generation ||
        (r.flags == 1 ? r.identity.ring_generation != 0 : r.identity.ring_generation == 0)) {
      return FrameWireStatus::InvalidInput;
    }
    FrameRingRegisterRequest cancel {};
    cancel.lease_id = r.lease_id; cancel.display_id = r.display_id;
    cancel.display_generation = r.identity.display_generation;
    cancel.client_cookie = r.identity.client_cookie; cancel.owner_capability = r.owner_capability;
    const auto owner = owner_for(cancel);
    const auto display = store_.authorize_temporary_display_owner(owner.display, owner.capability, now);
    if (!display || display->generation != cancel.display_generation) return FrameWireStatus::Unauthorized;
    reap(now);
    auto i = entries_.find(r.display_id);
    if (i == entries_.end()) {
      if (r.flags != 1) return FrameWireStatus::StaleGeneration;
      // Cancellation may win the race against registration. The tombstone
      // consumes the cookie even when no handles were ever imported.
      entries_.emplace(r.display_id, Entry {peer, cancel, {}});
      return FrameWireStatus::Success;
    }
    auto &entry = i->second;
    if (entry.peer != peer || entry.request.owner_capability != r.owner_capability ||
        entry.request.lease_id != r.lease_id) return FrameWireStatus::Unauthorized;
    if (r.identity.client_cookie < entry.request.client_cookie ||
        r.identity.display_generation != entry.request.display_generation) return FrameWireStatus::StaleGeneration;
    if (r.flags == 0 && (r.identity.client_cookie != entry.request.client_cookie ||
        r.identity.ring_generation != entry.reply.ring_generation)) return FrameWireStatus::StaleGeneration;
    if (entry.reply.ring_generation) {
      rings_.retire_ring(r.display_id, entry.request.display_generation, entry.reply.ring_generation);
    }
    // A cancellation for a newer cookie must also fence an older active ring.
    entry.request.client_cookie = r.identity.client_cookie;
    entry.reply = {};
    return FrameWireStatus::Success;
  }

  std::size_t FrameRingServer::reap(std::chrono::steady_clock::time_point now) {
    std::size_t count = 0;
    for (auto i = entries_.begin(); i != entries_.end();) {
      const auto owner = owner_for(i->second.request);
      const auto display = store_.authorize_temporary_display_owner(owner.display, owner.capability, now);
      if (!display || display->generation != i->second.request.display_generation) {
        rings_.retire_ring(i->first, i->second.request.display_generation, i->second.reply.ring_generation);
        i = entries_.erase(i); ++count;
      } else {
        if (i->second.reply.ring_generation) {
          const auto snapshot = rings_.snapshot(i->first, i->second.request.display_generation, now);
          if (!snapshot.can_publish() || snapshot.ring_generation != i->second.reply.ring_generation) {
            i->second.reply = {};
          }
        }
        ++i;
      }
    }
    return count;
  }
  void FrameRingServer::reset() {
    for (const auto &[id, entry]: entries_) {
      rings_.retire_ring(id, entry.request.display_generation, entry.reply.ring_generation);
    }
    // Retain high-water tombstones while the same display/lease remains live.
    // Delayed requests after a reset cannot resurrect a retired generation.
    for (auto &[id, entry]: entries_) entry.reply = {};
  }
}
