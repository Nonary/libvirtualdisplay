#include <gtest/gtest.h>

#include "virtual_display/driver/frame_ring_lifetime.h"

#include <cstring>
#include <future>
#include <limits>

namespace vdd = virtual_display::driver;

namespace {
  class Resources final: public vdd::FrameRingResources {
  public:
    explicit Resources(std::shared_ptr<int> destroyed): destroyed_ {std::move(destroyed)} {}
    ~Resources() override { ++*destroyed_; }
  private:
    std::shared_ptr<int> destroyed_;
  };

  class FrameRingLifetimeTest: public testing::Test {
  protected:
    std::chrono::steady_clock::time_point now {std::chrono::seconds {100}};
    vdd::DisplayStore store {2, 2};
    vdd::FrameRingLifetime rings {store};

    vdd::CreateTemporaryDisplayOwnedRequest request(std::uint64_t display_id = 200) {
      vdd::CreateTemporaryDisplayOwnedRequest request {};
      request.display.lease_id = vdd::kMinOpaqueLeaseId | 10;
      request.display.display_id = display_id;
      request.display.width = 1920;
      request.display.height = 1080;
      request.display.physical_width_mm = 530;
      request.display.physical_height_mm = 300;
      request.display.refresh_rate_millihz = 60'000;
      request.display.requested_timeout_ms = 30'000;
      std::strcpy(request.display.display_name, "Owned display");
      request.owner_capability.bytes.fill(42);
      return request;
    }

    vdd::FrameRingOwner create(std::uint64_t display_id = 200) {
      const auto req = request(display_id);
      const auto created = store.create_temporary_display_owned(req, now);
      EXPECT_EQ(created.status.error, vdd::StoreError::None);
      const auto display = store.find_temporary_display(display_id);
      EXPECT_TRUE(display);
      vdd::FrameRingOwner owner {};
      owner.display.lease_id = req.display.lease_id;
      owner.display.display_id = display_id;
      owner.display_generation = display ? display->generation : 0;
      owner.capability = req.owner_capability;
      return owner;
    }
  };
}

TEST_F(FrameRingLifetimeTest, RetainsResourcesOnlyForTheCurrentLiveOwner) {
  auto destroyed = std::make_shared<int>(0);
  auto resources = std::make_shared<Resources>(destroyed);
  const auto owner = create();
  const auto registered = rings.register_ring(owner, resources, now);
  ASSERT_EQ(registered.status, vdd::FrameRingRegistrationStatus::Success);
  EXPECT_NE(registered.ring_generation, 0u);
  auto snapshot = rings.snapshot(owner.display.display_id, owner.display_generation, now);
  EXPECT_EQ(snapshot.resources, resources);
  EXPECT_EQ(snapshot.ring_generation, registered.ring_generation);
  EXPECT_TRUE(snapshot.can_publish());
  EXPECT_TRUE(rings.clear_ring(owner, registered.ring_generation, now));
  EXPECT_FALSE(snapshot.can_publish());
  EXPECT_FALSE(rings.snapshot(owner.display.display_id, owner.display_generation, now).resources);
  resources.reset();
  EXPECT_EQ(*destroyed, 0); // in-flight GPU work still owns its snapshot
  snapshot = {};
  EXPECT_EQ(*destroyed, 1);
}

TEST_F(FrameRingLifetimeTest, RejectsGuessedIdentityCapabilityAndGenerationWithoutAdoptingResources) {
  auto destroyed = std::make_shared<int>(0);
  auto resources = std::make_shared<Resources>(destroyed);
  const auto owner = create();
  for (int mutation = 0; mutation < 6; ++mutation) {
    auto wrong = owner;
    switch (mutation) {
      case 0: ++wrong.display.lease_id; break;
      case 1: ++wrong.display.display_id; break;
      case 2: wrong.capability.bytes[31] ^= 1; break;
      case 3: wrong.display_generation = 0; break;
      case 4: ++wrong.display_generation; break;
      case 5: wrong.display.api_namespace = {}; break;
    }
    EXPECT_EQ(rings.register_ring(wrong, resources, now).status, vdd::FrameRingRegistrationStatus::OwnerUnavailable);
    EXPECT_EQ(resources.use_count(), 1);
  }
  EXPECT_FALSE(rings.snapshot(owner.display.display_id, owner.display_generation, now).resources);
}

TEST_F(FrameRingLifetimeTest, EnumerationAndUnownedCreationDoNotGrantPixelAccess) {
  const auto req = request();
  ASSERT_EQ(store.create_temporary_display(req.display, now).status.error, vdd::StoreError::None);
  const auto record = store.find_temporary_display(req.display.display_id);
  ASSERT_TRUE(record);
  vdd::LeaseDisplayRequest display {};
  display.lease_id = record->lease_id;
  display.display_id = record->display_id;
  EXPECT_FALSE(store.authorize_temporary_display_owner(display, req.owner_capability, now));
  EXPECT_FALSE(store.authorize_temporary_display_owner(display, {}, now));
}

TEST_F(FrameRingLifetimeTest, PendingDepartureAndExpiryStopNewPublication) {
  auto destroyed = std::make_shared<int>(0);
  auto resources = std::make_shared<Resources>(destroyed);
  const auto owner = create();
  ASSERT_EQ(rings.register_ring(owner, resources, now).status, vdd::FrameRingRegistrationStatus::Success);
  auto inflight = rings.snapshot(200, owner.display_generation, now);
  ASSERT_TRUE(inflight.resources);
  EXPECT_FALSE(rings.snapshot(200, owner.display_generation, now + std::chrono::seconds {30}).resources);
  EXPECT_EQ(rings.register_ring(owner, resources, now + std::chrono::seconds {30}).status,
            vdd::FrameRingRegistrationStatus::OwnerUnavailable);
  EXPECT_EQ(resources.use_count(), 2); // caller + existing in-flight work
  ASSERT_EQ(store.mark_temporary_display_pending_departure(owner.display).error, vdd::StoreError::None);
  EXPECT_EQ(rings.register_ring(owner, resources, now).status, vdd::FrameRingRegistrationStatus::OwnerUnavailable);
}

TEST_F(FrameRingLifetimeTest, ReplacementAndLateClearKeepTheNewGeneration) {
  auto old_destroyed = std::make_shared<int>(0);
  auto new_destroyed = std::make_shared<int>(0);
  auto first = std::make_shared<Resources>(old_destroyed);
  auto second = std::make_shared<Resources>(new_destroyed);
  const auto owner = create();
  const auto old = rings.register_ring(owner, first, now);
  auto inflight = rings.snapshot(200, owner.display_generation, now);
  const auto replacement = rings.register_ring(owner, second, now);
  ASSERT_NE(old.ring_generation, replacement.ring_generation);
  EXPECT_FALSE(inflight.can_publish());
  EXPECT_FALSE(rings.clear_ring(owner, old.ring_generation, now));
  EXPECT_FALSE(rings.retire_ring(200, owner.display_generation, old.ring_generation));
  EXPECT_EQ(rings.snapshot(200, owner.display_generation, now).resources, second);
  first.reset();
  EXPECT_EQ(*old_destroyed, 0);
  inflight = {};
  EXPECT_EQ(*old_destroyed, 1);
  rings.reset();
}

TEST_F(FrameRingLifetimeTest, SequencePersistsAcrossProducerSnapshotsForOneRing) {
  auto resources = std::make_shared<Resources>(std::make_shared<int>(0));
  const auto owner = create();
  ASSERT_EQ(rings.register_ring(owner, resources, now).status, vdd::FrameRingRegistrationStatus::Success);
  const auto first_snapshot = rings.snapshot(200, owner.display_generation, now);
  const auto reassigned_snapshot = rings.snapshot(200, owner.display_generation, now);
  std::uint64_t first {}, second {};
  ASSERT_TRUE(first_snapshot.next_sequence(first));
  ASSERT_TRUE(reassigned_snapshot.next_sequence(second));
  EXPECT_EQ(first, 1u);
  EXPECT_EQ(second, 2u);
  rings.reset();
}

TEST_F(FrameRingLifetimeTest, RetirementWaitsForAnInFlightPublicationCriticalSection) {
  auto resources = std::make_shared<Resources>(std::make_shared<int>(0));
  const auto owner = create();
  const auto registered = rings.register_ring(owner, resources, now);
  ASSERT_EQ(registered.status, vdd::FrameRingRegistrationStatus::Success);
  const auto snapshot = rings.snapshot(200, owner.display_generation, now);
  ASSERT_TRUE(snapshot.publication);

  std::unique_lock publication_lock {snapshot.publication->mutex};
  auto retiring = std::async(std::launch::async, [&]() {
    return rings.retire_ring(200, owner.display_generation, registered.ring_generation);
  });
  EXPECT_EQ(retiring.wait_for(std::chrono::milliseconds {20}), std::future_status::timeout);
  publication_lock.unlock();

  EXPECT_TRUE(retiring.get());
  EXPECT_FALSE(snapshot.can_publish());
  rings.reset();
}

TEST_F(FrameRingLifetimeTest, AbortedSlotCleanupCannotRestoreConsumerKeyAfterRetirement) {
  auto resources = std::make_shared<Resources>(std::make_shared<int>(0));
  const auto owner = create();
  const auto registered = rings.register_ring(owner, resources, now);
  ASSERT_EQ(registered.status, vdd::FrameRingRegistrationStatus::Success);
  const auto snapshot = rings.snapshot(200, owner.display_generation, now);

  std::uint64_t released_key = (std::numeric_limits<std::uint64_t>::max)();
  EXPECT_TRUE(vdd::release_unpublished_slot(snapshot, 1, [&](const std::uint64_t key) {
    released_key = key;
    return true;
  }));
  EXPECT_EQ(released_key, 1u); // live ring keeps the acquired key

  ASSERT_TRUE(rings.retire_ring(200, owner.display_generation, registered.ring_generation));
  EXPECT_TRUE(vdd::release_unpublished_slot(snapshot, 1, [&](const std::uint64_t key) {
    released_key = key;
    return true;
  }));
  EXPECT_EQ(released_key, 0u); // retired ring returns the slot to producer ownership
  EXPECT_FALSE(snapshot.can_publish());
}

TEST_F(FrameRingLifetimeTest, WrongOwnerCannotClearAnExistingRegistration) {
  auto destroyed = std::make_shared<int>(0);
  auto resources = std::make_shared<Resources>(destroyed);
  const auto owner = create();
  const auto registered = rings.register_ring(owner, resources, now);
  auto wrong = owner;
  wrong.capability.bytes[0] ^= 1;
  EXPECT_FALSE(rings.clear_ring(wrong, registered.ring_generation, now));
  EXPECT_EQ(rings.snapshot(200, owner.display_generation, now).resources, resources);
  rings.reset();
}

TEST_F(FrameRingLifetimeTest, PendingDepartureRetiresPublicationWhileRetainingInflightWork) {
  auto destroyed = std::make_shared<int>(0);
  auto resources = std::make_shared<Resources>(destroyed);
  const auto owner = create();
  ASSERT_EQ(rings.register_ring(owner, resources, now).status, vdd::FrameRingRegistrationStatus::Success);
  auto inflight = rings.snapshot(200, owner.display_generation, now);
  ASSERT_EQ(store.mark_temporary_display_pending_departure(owner.display).error, vdd::StoreError::None);
  EXPECT_FALSE(rings.snapshot(200, owner.display_generation, now).resources);
  EXPECT_EQ(resources.use_count(), 2);
  EXPECT_FALSE(inflight.can_publish());
  resources.reset();
  EXPECT_EQ(*destroyed, 0);
  inflight = {};
  EXPECT_EQ(*destroyed, 1);
}

TEST_F(FrameRingLifetimeTest, LeaseReclaimRevokesTheOldProducerAndRegistrationOwner) {
  auto destroyed = std::make_shared<int>(0);
  auto resources = std::make_shared<Resources>(destroyed);
  const auto owner = create();
  ASSERT_EQ(rings.register_ring(owner, resources, now).status, vdd::FrameRingRegistrationStatus::Success);
  vdd::ReclaimTemporaryDisplayRequest reclaim {};
  reclaim.display_id = owner.display.display_id;
  reclaim.new_lease_id = owner.display.lease_id + 1;
  reclaim.requested_timeout_ms = 30'000;
  reclaim.owner_capability = owner.capability;
  ASSERT_EQ(store.reclaim_temporary_display(reclaim, now).status.error, vdd::StoreError::None);
  EXPECT_FALSE(rings.snapshot(200, owner.display_generation, now).resources);
  EXPECT_EQ(rings.register_ring(owner, resources, now).status, vdd::FrameRingRegistrationStatus::OwnerUnavailable);
  auto next_owner = owner;
  next_owner.display.lease_id = reclaim.new_lease_id;
  ASSERT_EQ(rings.register_ring(next_owner, resources, now).status, vdd::FrameRingRegistrationStatus::Success);
  EXPECT_TRUE(rings.snapshot(200, owner.display_generation, now).resources);
  rings.reset();
}

TEST_F(FrameRingLifetimeTest, FailedImportDoesNotReplaceTheLiveRegistration) {
  auto destroyed = std::make_shared<int>(0);
  auto resources = std::make_shared<Resources>(destroyed);
  const auto owner = create();
  const auto registered = rings.register_ring(owner, resources, now);
  EXPECT_EQ(rings.register_ring(owner, {}, now).status, vdd::FrameRingRegistrationStatus::InvalidResources);
  EXPECT_EQ(rings.snapshot(200, owner.display_generation, now).ring_generation, registered.ring_generation);
  rings.reset();
}

TEST_F(FrameRingLifetimeTest, ResetAndDeviceLossCannotReuseGenerationOrInvalidateInflightWork) {
  auto destroyed = std::make_shared<int>(0);
  auto resources = std::make_shared<Resources>(destroyed);
  const auto owner = create();
  const auto first = rings.register_ring(owner, resources, now);
  auto inflight = rings.snapshot(200, owner.display_generation, now);
  rings.reset();
  EXPECT_FALSE(inflight.can_publish());
  const auto next = rings.register_ring(owner, resources, now);
  EXPECT_GT(next.ring_generation, first.ring_generation);
  EXPECT_FALSE(rings.retire_ring(200, owner.display_generation, first.ring_generation));
  EXPECT_TRUE(rings.retire_ring(200, owner.display_generation, next.ring_generation));
  resources.reset();
  EXPECT_EQ(*destroyed, 0);
  inflight = {};
  EXPECT_EQ(*destroyed, 1);
}

TEST_F(FrameRingLifetimeTest, DisplayRecreationRejectsOldDepartureAndProducerTokens) {
  auto destroyed = std::make_shared<int>(0);
  auto resources = std::make_shared<Resources>(destroyed);
  const auto old_owner = create();
  ASSERT_EQ(rings.register_ring(old_owner, resources, now).status, vdd::FrameRingRegistrationStatus::Success);
  ASSERT_EQ(store.remove_temporary_display(old_owner.display).error, vdd::StoreError::None);
  const auto next_owner = create();
  ASSERT_NE(old_owner.display_generation, next_owner.display_generation);
  ASSERT_EQ(rings.register_ring(next_owner, resources, now).status, vdd::FrameRingRegistrationStatus::Success);
  EXPECT_FALSE(rings.retire_display(200, old_owner.display_generation));
  EXPECT_FALSE(rings.snapshot(200, old_owner.display_generation, now).resources);
  EXPECT_TRUE(rings.snapshot(200, next_owner.display_generation, now).resources);
  rings.reset();
}

TEST_F(FrameRingLifetimeTest, RemovedDisplayResourcesDoNotAccumulateWhenDisplayIdsChurn) {
  auto destroyed = std::make_shared<int>(0);
  auto resources = std::make_shared<Resources>(destroyed);
  const auto first_owner = create();
  ASSERT_EQ(rings.register_ring(first_owner, resources, now).status, vdd::FrameRingRegistrationStatus::Success);
  ASSERT_EQ(store.remove_temporary_display(first_owner.display).error, vdd::StoreError::None);
  const auto next_owner = create(201);
  auto next_destroyed = std::make_shared<int>(0);
  auto next_resources = std::make_shared<Resources>(next_destroyed);
  ASSERT_EQ(rings.register_ring(next_owner, next_resources, now).status, vdd::FrameRingRegistrationStatus::Success);
  EXPECT_EQ(resources.use_count(), 1);
  rings.reset();
}

TEST_F(FrameRingLifetimeTest, ReaperRevokesAnIdleRegistrationWithoutWaitingForANewProducer) {
  auto destroyed = std::make_shared<int>(0);
  auto resources = std::make_shared<Resources>(destroyed);
  const auto owner = create();
  ASSERT_EQ(rings.register_ring(owner, resources, now).status, vdd::FrameRingRegistrationStatus::Success);
  auto producer = rings.snapshot(200, owner.display_generation, now);
  EXPECT_EQ(rings.retire_unavailable(now + std::chrono::seconds {30}), 1u);
  EXPECT_FALSE(producer.can_publish());
  EXPECT_EQ(rings.retire_unavailable(now + std::chrono::seconds {30}), 0u);
  resources.reset();
  EXPECT_EQ(*destroyed, 0);
  producer = {};
  EXPECT_EQ(*destroyed, 1);
}

TEST_F(FrameRingLifetimeTest, RegistryDestructionRevokesSnapshotsBeforeDroppingPublicationOwnership) {
  auto destroyed = std::make_shared<int>(0);
  auto resources = std::make_shared<Resources>(destroyed);
  const auto owner = create();
  vdd::FrameRingSnapshot producer {};
  {
    vdd::FrameRingLifetime temporary {store};
    ASSERT_EQ(temporary.register_ring(owner, resources, now).status, vdd::FrameRingRegistrationStatus::Success);
    producer = temporary.snapshot(200, owner.display_generation, now);
    ASSERT_TRUE(producer.can_publish());
  }
  EXPECT_FALSE(producer.can_publish());
  resources.reset();
  EXPECT_EQ(*destroyed, 0);
  producer = {};
  EXPECT_EQ(*destroyed, 1);
}
