#pragma once

#include "virtual_display/driver/control_protocol.h"

namespace virtual_display::driver {
  // Experimental wire contract. The capability bit reports opt-in build
  // support; it does not imply live target readiness or Windows/WDK/GPU qualification.
  inline constexpr std::uint32_t kFrameRingWireVersion = 1;
  inline constexpr std::uint32_t kFrameRingMetadataBytes = 4096;
  inline constexpr std::uint32_t kIoctlRegisterFrameRing = ioctl_code(static_cast<IoctlFunction>(0x90f), IoctlAccess::ReadWrite);
  inline constexpr std::uint32_t kIoctlClearFrameRing = ioctl_code(static_cast<IoctlFunction>(0x910), IoctlAccess::ReadWrite);

  // Owner-authenticated snapshot of one temporary display lifetime. The host
  // must query this before allocating/registering a ring and pass the returned
  // generation back unchanged. Global display-state enumeration deliberately
  // does not disclose this token.
  struct FrameTransportTargetRequest {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint32_t version {kFrameRingWireVersion};
    std::uint32_t size {sizeof(FrameTransportTargetRequest)};
    std::uint64_t lease_id {};
    std::uint64_t display_id {};
    OwnerCapability owner_capability {};
  };

  struct FrameTransportTarget {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint32_t version {kFrameRingWireVersion};
    std::uint32_t size {sizeof(FrameTransportTarget)};
    std::uint64_t lease_id {};
    std::uint64_t display_id {};
    std::uint64_t display_generation {};
    std::uint32_t width {};
    std::uint32_t height {};
    AdapterLuid adapter_luid {};
    std::uint32_t format {};
    std::uint32_t color_space {};
  };

  struct FrameRingRegisterRequest {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint32_t version {kFrameRingWireVersion};
    std::uint32_t size {sizeof(FrameRingRegisterRequest)};
    std::uint64_t lease_id {};
    std::uint64_t display_id {};
    std::uint64_t display_generation {};
    std::uint64_t client_cookie {};
    OwnerCapability owner_capability {};
    AdapterLuid adapter_luid {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t format {}; // DXGI_FORMAT_B8G8R8A8_UNORM or R16G16B16A16_FLOAT
    std::uint32_t color_space {}; // P709 gamma 2.2 or linear scRGB, respectively
    std::uint32_t slot_count {kFrameRingSlotCount};
    std::uint32_t metadata_bytes {kFrameRingMetadataBytes};
    // Values belong to the authenticated requestor's handle table. Never close
    // these values locally, accept a payload PID, or use DUPLICATE_CLOSE_SOURCE.
    std::array<std::uint64_t, kFrameRingSlotCount> texture_handles {};
    std::uint64_t metadata_handle {};
    std::uint64_t event_handle {};
    std::uint64_t reserved {};
  };

  struct FrameRingReply {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint32_t version {kFrameRingWireVersion};
    std::uint32_t size {sizeof(FrameRingReply)};
    std::uint64_t client_cookie {};
    std::uint64_t display_generation {};
    std::uint64_t ring_generation {};
  };

  struct FrameRingClearRequest {
    FrameRingReply identity {};
    std::uint64_t lease_id {};
    std::uint64_t display_id {};
    OwnerCapability owner_capability {};
    std::uint32_t flags {}; // 1: cancel an unacknowledged client_cookie, ring_generation must be 0
    std::uint32_t reserved {};
  };

  // No std::atomic, pointers, bools or compiler-specific C++ objects cross the
  // process boundary. Every field of a slot is read/written only while holding
  // that texture's keyed mutex. The header is immutable after registration.
  // Each registration uses a fresh section; no section is reused across rings.
  struct FrameSlotMetadata {
    std::uint64_t client_cookie {};
    std::uint64_t display_generation {};
    std::uint64_t ring_generation {};
    std::uint64_t sequence {}; // starts at 1; retire before uint64 overflow
    std::uint64_t acquired_qpc {}; // acquisition timestamp, never scan-out QPC
    std::uint64_t reserved {};
  };
  struct alignas(8) FrameRingMetadata {
    Guid api_namespace {kApiNamespaceGuid};
    std::uint32_t version {kFrameRingWireVersion};
    std::uint32_t size {sizeof(FrameRingMetadata)};
    std::uint64_t client_cookie {};
    std::array<FrameSlotMetadata, kFrameRingSlotCount> slots {};
    std::array<std::uint8_t, 3872> reserved {};
  };

  static_assert(std::is_standard_layout_v<FrameRingRegisterRequest>);
  static_assert(sizeof(FrameTransportTargetRequest) == 72);
  static_assert(sizeof(FrameTransportTarget) == 72);
  static_assert(sizeof(FrameRingRegisterRequest) == 176);
  static_assert(offsetof(FrameRingRegisterRequest, texture_handles) == 120);
  static_assert(sizeof(FrameRingReply) == 48);
  static_assert(sizeof(FrameRingClearRequest) == 104);
  static_assert(sizeof(FrameSlotMetadata) == 48);
  static_assert(sizeof(FrameRingMetadata) == kFrameRingMetadataBytes);
  static_assert(offsetof(FrameRingMetadata, slots) == 32);

  bool valid_frame_ring_request(const FrameRingRegisterRequest &request);
  bool valid_frame_transport_target_request(const FrameTransportTargetRequest &request);
  bool valid_frame_transport_target(const FrameTransportTarget &target,
                                    const FrameTransportTargetRequest &request);
  bool same_frame_ring_request(const FrameRingRegisterRequest &a, const FrameRingRegisterRequest &b);
  bool valid_frame_ring_reply(const FrameRingReply &reply, const FrameRingRegisterRequest &request);
} // namespace virtual_display::driver
