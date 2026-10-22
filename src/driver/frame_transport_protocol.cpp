#include "virtual_display/driver/frame_transport_protocol.h"

namespace virtual_display::driver {
  bool valid_frame_transport_target_request(const FrameTransportTargetRequest &r) {
    return is_valid_api_namespace(r.api_namespace) && r.version == kFrameRingWireVersion &&
           r.size == sizeof(r) && r.lease_id && r.display_id &&
           std::any_of(r.owner_capability.bytes.begin(), r.owner_capability.bytes.end(), [](auto byte) { return byte != 0; });
  }

  bool valid_frame_transport_target(const FrameTransportTarget &target,
                                   const FrameTransportTargetRequest &request) {
    return is_valid_api_namespace(target.api_namespace) && target.version == kFrameRingWireVersion &&
           target.size == sizeof(target) && target.lease_id == request.lease_id &&
           target.display_id == request.display_id && target.display_generation != 0 &&
           target.width >= kMinWidth && target.width <= kMaxWidth &&
           target.height >= kMinHeight && target.height <= kMaxHeight &&
           (target.adapter_luid.low_part != 0 || target.adapter_luid.high_part != 0) &&
           ((target.format == 87 && target.color_space == 0) || (target.format == 10 && target.color_space == 1));
  }

  bool valid_frame_ring_request(const FrameRingRegisterRequest &r) {
    if (!is_valid_api_namespace(r.api_namespace) || r.version != kFrameRingWireVersion ||
        r.size != sizeof(r) || !r.lease_id || !r.display_id || !r.display_generation ||
        !r.client_cookie || r.width < kMinWidth || r.width > kMaxWidth ||
        r.height < kMinHeight || r.height > kMaxHeight ||
        r.slot_count != kFrameRingSlotCount || r.metadata_bytes != kFrameRingMetadataBytes ||
        !r.metadata_handle || !r.event_handle || r.reserved ||
        !((r.format == 87 && r.color_space == 0) || (r.format == 10 && r.color_space == 1))) {
      return false;
    }
    std::array<std::uint64_t, 6> handles {};
    std::copy(r.texture_handles.begin(), r.texture_handles.end(), handles.begin());
    handles[4] = r.metadata_handle;
    handles[5] = r.event_handle;
    std::sort(handles.begin(), handles.end());
    return handles.front() != 0 && handles.back() != (std::numeric_limits<std::uint64_t>::max)() &&
           std::adjacent_find(handles.begin(), handles.end()) == handles.end();
  }

  bool same_frame_ring_request(const FrameRingRegisterRequest &a, const FrameRingRegisterRequest &b) {
    return a.api_namespace == b.api_namespace && a.version == b.version && a.size == b.size &&
           a.lease_id == b.lease_id && a.display_id == b.display_id &&
           a.display_generation == b.display_generation && a.client_cookie == b.client_cookie &&
           a.owner_capability == b.owner_capability &&
           a.adapter_luid.low_part == b.adapter_luid.low_part && a.adapter_luid.high_part == b.adapter_luid.high_part &&
           a.width == b.width && a.height == b.height && a.format == b.format && a.color_space == b.color_space &&
           a.slot_count == b.slot_count && a.metadata_bytes == b.metadata_bytes &&
           a.texture_handles == b.texture_handles && a.metadata_handle == b.metadata_handle &&
           a.event_handle == b.event_handle && a.reserved == b.reserved;
  }

  bool valid_frame_ring_reply(const FrameRingReply &reply, const FrameRingRegisterRequest &request) {
    return is_valid_api_namespace(reply.api_namespace) && reply.version == kFrameRingWireVersion &&
           reply.size == sizeof(reply) && reply.client_cookie == request.client_cookie &&
           reply.display_generation == request.display_generation && reply.ring_generation != 0;
  }
}
