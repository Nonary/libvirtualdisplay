#include <gtest/gtest.h>

#include "virtual_display/driver/frame_ring_server.h"

#include <cstring>

namespace vdd = virtual_display::driver;

namespace {
  class UnusedImporter final: public vdd::FrameResourceImporter {
  public:
    std::shared_ptr<vdd::FrameRingResources> import(
      const vdd::FrameRequestPeer &, const vdd::FrameRingRegisterRequest &
    ) override {
      return {};
    }
  };

  class FrameRingServerTest: public testing::Test {
  protected:
    std::chrono::steady_clock::time_point now {std::chrono::seconds {200}};
    vdd::DisplayStore store {2, 2};
    vdd::FrameRingLifetime rings {store};
    UnusedImporter importer {};
    vdd::FrameRingServer server {store, rings, importer};
    vdd::FrameRequestPeer peer {1234, 987654};

    vdd::CreateTemporaryDisplayOwnedRequest create_request() {
      vdd::CreateTemporaryDisplayOwnedRequest request {};
      request.display.lease_id = vdd::kMinOpaqueLeaseId | 19;
      request.display.display_id = 619;
      request.display.width = 2560;
      request.display.height = 1440;
      request.display.physical_width_mm = 530;
      request.display.physical_height_mm = 300;
      request.display.refresh_rate_millihz = 144'000;
      request.display.requested_timeout_ms = 30'000;
      std::strcpy(request.display.display_name, "Frame target");
      request.owner_capability.bytes.fill(71);
      return request;
    }

    vdd::FrameTransportTargetRequest target_request(
      const vdd::CreateTemporaryDisplayOwnedRequest &created
    ) {
      vdd::FrameTransportTargetRequest request {};
      request.lease_id = created.display.lease_id;
      request.display_id = created.display.display_id;
      request.owner_capability = created.owner_capability;
      return request;
    }
  };
}

TEST_F(FrameRingServerTest, OwnerQueryReturnsTheCurrentLifetimeTokenAndModeBounds) {
  const auto created = create_request();
  ASSERT_EQ(store.create_temporary_display_owned(created, now).status.error, vdd::StoreError::None);

  const auto request = target_request(created);
  ASSERT_TRUE(vdd::valid_frame_transport_target_request(request));
  const auto target = server.query_target(peer, request, now);
  ASSERT_TRUE(target);
  // The portable lifetime service supplies the owner-authenticated display
  // token and requested bounds. The Windows IddCx backend must enrich this
  // with the active surface adapter/format before the target is usable.
  EXPECT_FALSE(vdd::valid_frame_transport_target(*target, request));
  EXPECT_EQ(target->lease_id, request.lease_id);
  EXPECT_EQ(target->display_id, request.display_id);
  EXPECT_NE(target->display_generation, 0u);
  EXPECT_EQ(target->width, created.display.width);
  EXPECT_EQ(target->height, created.display.height);
  auto qualified_target = *target;
  qualified_target.adapter_luid = {1, 2};
  qualified_target.format = 87;
  qualified_target.color_space = 0;
  EXPECT_TRUE(vdd::valid_frame_transport_target(qualified_target, request));
  qualified_target.format = 10;
  qualified_target.color_space = 1;
  EXPECT_TRUE(vdd::valid_frame_transport_target(qualified_target, request));
  qualified_target.color_space = 0;
  EXPECT_FALSE(vdd::valid_frame_transport_target(qualified_target, request));
}

TEST_F(FrameRingServerTest, QueryDoesNotDiscloseTargetsToGuessedOwnersOrInvalidPeers) {
  const auto created = create_request();
  ASSERT_EQ(store.create_temporary_display_owned(created, now).status.error, vdd::StoreError::None);
  const auto request = target_request(created);

  auto wrong_capability = request;
  wrong_capability.owner_capability.bytes[0] ^= 1;
  EXPECT_FALSE(server.query_target(peer, wrong_capability, now));

  auto wrong_lease = request;
  ++wrong_lease.lease_id;
  EXPECT_FALSE(server.query_target(peer, wrong_lease, now));

  auto wrong_display = request;
  ++wrong_display.display_id;
  EXPECT_FALSE(server.query_target(peer, wrong_display, now));

  EXPECT_FALSE(server.query_target({}, request, now));
}

TEST_F(FrameRingServerTest, QueryStopsAfterExpiryOrPendingDeparture) {
  const auto created = create_request();
  ASSERT_EQ(store.create_temporary_display_owned(created, now).status.error, vdd::StoreError::None);
  const auto request = target_request(created);
  ASSERT_TRUE(server.query_target(peer, request, now));
  EXPECT_FALSE(server.query_target(peer, request, now + std::chrono::seconds {31}));

  vdd::LeaseDisplayRequest display {};
  display.lease_id = request.lease_id;
  display.display_id = request.display_id;
  ASSERT_EQ(store.mark_temporary_display_pending_departure(display).error, vdd::StoreError::None);
  EXPECT_FALSE(server.query_target(peer, request, now));
}

TEST(FrameTransportProtocol, RingRequestRejectsUnboundedTextureDimensions) {
  vdd::FrameRingRegisterRequest request {};
  request.lease_id = 1;
  request.display_id = 2;
  request.display_generation = 3;
  request.client_cookie = 4;
  request.width = vdd::kMaxWidth + 1;
  request.height = vdd::kMaxHeight;
  request.texture_handles = {1, 2, 3, 4};
  request.metadata_handle = 5;
  request.event_handle = 6;
  request.format = 87;
  EXPECT_FALSE(vdd::valid_frame_ring_request(request));
  request.width = vdd::kMaxWidth;
  request.height = vdd::kMaxHeight + 1;
  EXPECT_FALSE(vdd::valid_frame_ring_request(request));
}
