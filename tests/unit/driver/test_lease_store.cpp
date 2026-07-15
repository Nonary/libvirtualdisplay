#include <gtest/gtest.h>
#include "virtual_display/driver/display_identity.h"
#include "virtual_display/driver/lease_store.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace vdd = virtual_display::driver;

namespace {
  constexpr std::uint64_t lease_id(const std::uint64_t suffix) {
    return vdd::kMinOpaqueLeaseId | suffix;
  }

  template <std::size_t N>
  void set_display_name(char (&destination)[N], const std::string_view name) {
    static_assert(N > 0);
    std::fill(std::begin(destination), std::end(destination), '\0');
    const auto bytes_to_copy = (std::min)(name.size(), N - 1u);
    std::memcpy(destination, name.data(), bytes_to_copy);
  }

  vdd::TemporaryDisplayRecord require_temporary_display(
    const vdd::DisplayStore &store,
    const std::uint64_t display_id
  ) {
    const auto record = store.find_temporary_display(display_id);
    EXPECT_TRUE(record) << "missing temporary display " << display_id;
    return record.value_or(vdd::TemporaryDisplayRecord {});
  }

  vdd::CreateTemporaryDisplayRequest make_create_request(
    const std::uint64_t lease_id_value = lease_id(100),
    const std::uint64_t display_id = 200
  ) {
    vdd::CreateTemporaryDisplayRequest request {};
    request.lease_id = lease_id_value;
    request.display_id = display_id;
    request.width = 1920;
    request.height = 1080;
    request.physical_width_mm = 530;
    request.physical_height_mm = 300;
    request.hdr_max_luminance_nits = 2000;
    request.refresh_rate_millihz = 60'000;
    request.requested_timeout_ms = 30'000;
    set_display_name(request.display_name, "Sunshine Display");
    return request;
  }

  vdd::OwnerCapability owner_capability(const std::uint8_t seed = 1) {
    vdd::OwnerCapability capability {};
    for (std::size_t index = 0; index < capability.bytes.size(); ++index) {
      capability.bytes[index] = static_cast<std::uint8_t>(seed + index);
    }
    return capability;
  }

  vdd::CreateTemporaryDisplayOwnedRequest make_owned_create_request(
    const std::uint64_t lease_id_value = lease_id(100),
    const std::uint64_t display_id = 200,
    const vdd::OwnerCapability &capability = owner_capability()
  ) {
    vdd::CreateTemporaryDisplayOwnedRequest request {};
    request.display = make_create_request(lease_id_value, display_id);
    request.owner_capability = capability;
    return request;
  }
}  // namespace

TEST(VirtualDisplayDriverLeaseStore, CreatesTemporaryDisplayWithConnectorIndex) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();

  const auto created = store.create_temporary_display(make_create_request(), now);

  EXPECT_EQ(created.status.error, vdd::StoreError::None);
  EXPECT_EQ(created.result.lease_id, lease_id(100));
  EXPECT_EQ(created.result.display_id, 200u);
  EXPECT_EQ(created.result.connector_index, 2u);
  EXPECT_EQ(created.result.effective_timeout_ms, 30'000u);
  EXPECT_EQ(store.temporary_display_count(), 1u);

  const auto record = store.find_temporary_display(200);
  ASSERT_TRUE(record);
  EXPECT_EQ(record->width, 1920u);
  EXPECT_EQ(record->height, 1080u);
  EXPECT_EQ(record->physical_width_mm, 530u);
  EXPECT_EQ(record->physical_height_mm, 300u);
  EXPECT_EQ(record->hdr_max_luminance_nits, 2000u);
  EXPECT_EQ(record->refresh_rate_millihz, 60'000u);
  EXPECT_EQ(record->display_name, "Sunshine Display");
}

TEST(VirtualDisplayDriverLeaseStore, ReclaimsOwnedDisplayWithoutChangingIdentity) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();
  const auto capability = owner_capability();
  ASSERT_EQ(
    store.create_temporary_display_owned(make_owned_create_request(lease_id(100), 200, capability), now).status.error,
    vdd::StoreError::None
  );
  const auto before = require_temporary_display(store, 200);

  vdd::ReclaimTemporaryDisplayRequest reclaim {};
  reclaim.display_id = 200;
  reclaim.new_lease_id = lease_id(101);
  reclaim.requested_timeout_ms = 60'000;
  reclaim.owner_capability = capability;
  const auto reclaimed = store.reclaim_temporary_display(reclaim, now + std::chrono::seconds(10));

  ASSERT_EQ(reclaimed.status.error, vdd::StoreError::None);
  EXPECT_EQ(reclaimed.result.lease_id, lease_id(101));
  EXPECT_EQ(reclaimed.result.display_id, 200u);
  EXPECT_EQ(reclaimed.result.connector_index, before.connector_index);
  EXPECT_EQ(reclaimed.result.temporary_display_count, 1u);
  EXPECT_EQ(store.query_lease(lease_id(100), now + std::chrono::seconds(10)).lease_exists, 0u);
  EXPECT_EQ(store.query_lease(lease_id(101), now + std::chrono::seconds(10)).remaining_ms, 60'000u);

  const auto after = require_temporary_display(store, 200);
  EXPECT_EQ(after.lease_id, lease_id(101));
  EXPECT_EQ(after.connector_index, before.connector_index);
  EXPECT_EQ(after.identity_display_id, before.identity_display_id);
  EXPECT_EQ(after.generation, before.generation);
  EXPECT_EQ(after.width, before.width);
  EXPECT_EQ(after.height, before.height);
  EXPECT_EQ(after.physical_width_mm, before.physical_width_mm);
  EXPECT_EQ(after.physical_height_mm, before.physical_height_mm);
  EXPECT_EQ(after.refresh_rate_millihz, before.refresh_rate_millihz);
  EXPECT_EQ(after.display_name, before.display_name);
  EXPECT_EQ(after.retain_identity, before.retain_identity);
  EXPECT_EQ(after.hdr_max_luminance_nits, before.hdr_max_luminance_nits);
  EXPECT_EQ(after.retain_identity, before.retain_identity);
  EXPECT_EQ(after.width, before.width);
  EXPECT_EQ(after.height, before.height);
}

TEST(VirtualDisplayDriverLeaseStore, ReclaimRotatesEveryDisplayOnOwnedLease) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();
  const auto capability = owner_capability();
  ASSERT_EQ(
    store.create_temporary_display_owned(make_owned_create_request(lease_id(100), 200, capability), now).status.error,
    vdd::StoreError::None
  );
  ASSERT_EQ(
    store.create_temporary_display_owned(make_owned_create_request(lease_id(100), 201, capability), now).status.error,
    vdd::StoreError::None
  );

  vdd::ReclaimTemporaryDisplayRequest reclaim {};
  reclaim.display_id = 200;
  reclaim.new_lease_id = lease_id(101);
  reclaim.requested_timeout_ms = 30'000;
  reclaim.owner_capability = capability;
  const auto reclaimed = store.reclaim_temporary_display(reclaim, now + std::chrono::seconds(1));

  ASSERT_EQ(reclaimed.status.error, vdd::StoreError::None);
  EXPECT_EQ(reclaimed.result.temporary_display_count, 2u);
  EXPECT_EQ(require_temporary_display(store, 200).lease_id, lease_id(101));
  EXPECT_EQ(require_temporary_display(store, 201).lease_id, lease_id(101));
  EXPECT_EQ(store.query_lease(lease_id(100), now + std::chrono::seconds(1)).lease_exists, 0u);
  EXPECT_EQ(store.query_lease(lease_id(101), now + std::chrono::seconds(1)).temporary_display_count, 2u);
}

TEST(VirtualDisplayDriverLeaseStore, ReclaimFailsClosedForWrongCapabilityExpiryAndPendingDeparture) {
  const auto now = std::chrono::steady_clock::now();
  const auto capability = owner_capability();

  {
    vdd::DisplayStore store {2, 4};
    ASSERT_EQ(
      store.create_temporary_display_owned(make_owned_create_request(lease_id(100), 200, capability), now).status.error,
      vdd::StoreError::None
    );
    vdd::ReclaimTemporaryDisplayRequest reclaim {};
    reclaim.display_id = 200;
    reclaim.new_lease_id = lease_id(100);
    reclaim.requested_timeout_ms = 30'000;
    reclaim.owner_capability = capability;
    EXPECT_EQ(
      store.reclaim_temporary_display(reclaim, now + std::chrono::seconds(1)).status.error,
      vdd::StoreError::LeaseNotFound
    );
    EXPECT_EQ(require_temporary_display(store, 200).lease_id, lease_id(100));

    reclaim.new_lease_id = lease_id(101);
    reclaim.owner_capability = owner_capability(9);
    EXPECT_EQ(
      store.reclaim_temporary_display(reclaim, now + std::chrono::seconds(1)).status.error,
      vdd::StoreError::LeaseNotFound
    );
    EXPECT_EQ(require_temporary_display(store, 200).lease_id, lease_id(100));

    reclaim.owner_capability = capability;
    EXPECT_EQ(
      store.reclaim_temporary_display(reclaim, now + std::chrono::seconds(30)).status.error,
      vdd::StoreError::LeaseNotFound
    );
  }

  {
    vdd::DisplayStore store {2, 4};
    ASSERT_EQ(
      store.create_temporary_display_owned(make_owned_create_request(lease_id(100), 200, capability), now).status.error,
      vdd::StoreError::None
    );
    EXPECT_EQ(
      store.mark_temporary_display_pending_departure({vdd::kApiNamespaceGuid, lease_id(100), 200}).error,
      vdd::StoreError::None
    );
    vdd::ReclaimTemporaryDisplayRequest reclaim {};
    reclaim.display_id = 200;
    reclaim.new_lease_id = lease_id(101);
    reclaim.requested_timeout_ms = 30'000;
    reclaim.owner_capability = capability;
    EXPECT_EQ(
      store.reclaim_temporary_display(reclaim, now + std::chrono::seconds(1)).status.error,
      vdd::StoreError::LeaseNotFound
    );
  }
}

TEST(VirtualDisplayDriverLeaseStore, RejectsDuplicateDisplayId) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();

  ASSERT_EQ(store.create_temporary_display(make_create_request(), now).status.error, vdd::StoreError::None);
  EXPECT_EQ(
    store.create_temporary_display(make_create_request(lease_id(101), 200), now).status.error,
    vdd::StoreError::DisplayAlreadyExists
  );
}

TEST(VirtualDisplayDriverLeaseStore, RejectsDuplicateActiveEdidIdentity) {
  constexpr std::uint64_t first_display_id = 0x0000000012345000ull;
  constexpr std::uint64_t colliding_display_id = 0x1234000000005000ull;
  static_assert(first_display_id != colliding_display_id);

  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();

  ASSERT_EQ(
    vdd::product_code_from_display_id(first_display_id),
    vdd::product_code_from_display_id(colliding_display_id)
  );
  ASSERT_EQ(
    vdd::serial_number_from_display_id(first_display_id),
    vdd::serial_number_from_display_id(colliding_display_id)
  );

  ASSERT_EQ(
    store.create_temporary_display(make_create_request(lease_id(100), first_display_id), now).status.error,
    vdd::StoreError::None
  );
  EXPECT_EQ(
    store.create_temporary_display(make_create_request(lease_id(101), colliding_display_id), now).status.error,
    vdd::StoreError::DuplicateDisplayIdentity
  );
  EXPECT_EQ(store.temporary_display_count(), 1u);
}

TEST(VirtualDisplayDriverLeaseStore, EphemeralIdentitySkipsActiveEdidCollision) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();

  ASSERT_EQ(
    store.create_temporary_display(make_create_request(lease_id(100), 0x6000000000000001ull), now).status.error,
    vdd::StoreError::None
  );
  auto request = make_create_request(lease_id(101), 200);
  request.flags = vdd::kCreateTemporaryDisplayFlagEphemeralIdentity;

  ASSERT_EQ(store.create_temporary_display(request, now).status.error, vdd::StoreError::None);

  const auto record = store.find_temporary_display(200);
  ASSERT_TRUE(record);
  EXPECT_FALSE(record->retain_identity);
  EXPECT_EQ(record->identity_display_id, 0x6000000000000002ull);
}

TEST(VirtualDisplayDriverLeaseStore, RejectsTemporaryDisplayLimit) {
  vdd::DisplayStore store {2, 1};
  const auto now = std::chrono::steady_clock::now();

  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(1), 10), now).status.error, vdd::StoreError::None);
  EXPECT_EQ(
    store.create_temporary_display(make_create_request(lease_id(2), 20), now).status.error,
    vdd::StoreError::TemporaryDisplayLimitReached
  );
}

TEST(VirtualDisplayDriverLeaseStore, ClampsOverflowingTemporaryConnectorRange) {
  vdd::DisplayStore store {(std::numeric_limits<std::uint32_t>::max)() - 1u, 4};
  const auto now = std::chrono::steady_clock::now();

  const auto created = store.create_temporary_display(make_create_request(lease_id(1), 10), now);
  EXPECT_EQ(created.status.error, vdd::StoreError::None);
  EXPECT_EQ(created.result.connector_index, (std::numeric_limits<std::uint32_t>::max)() - 1u);

  EXPECT_EQ(
    store.create_temporary_display(make_create_request(lease_id(2), 20), now).status.error,
    vdd::StoreError::TemporaryDisplayLimitReached
  );
}

TEST(VirtualDisplayDriverLeaseStore, QueriesAndFeedsLease) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();
  ASSERT_EQ(store.create_temporary_display(make_create_request(), now).status.error, vdd::StoreError::None);

  auto query = store.query_lease(lease_id(100), now + std::chrono::seconds(10));
  EXPECT_EQ(query.lease_exists, 1u);
  EXPECT_EQ(query.temporary_display_count, 1u);
  EXPECT_EQ(query.pending_departure_count, 0u);
  EXPECT_EQ(query.effective_timeout_ms, 30'000u);
  EXPECT_EQ(query.remaining_ms, 20'000u);

  const vdd::LeaseRequest feed {
    vdd::kApiNamespaceGuid,
    lease_id(100),
    60'000,
    0
  };
  EXPECT_EQ(store.feed_lease(feed, now + std::chrono::seconds(20)).error, vdd::StoreError::None);

  query = store.query_lease(lease_id(100), now + std::chrono::seconds(20));
  EXPECT_EQ(query.effective_timeout_ms, 60'000u);
  EXPECT_EQ(query.remaining_ms, 60'000u);
}

TEST(VirtualDisplayDriverLeaseStore, RejectsFeedAfterLeaseExpiry) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();
  ASSERT_EQ(store.create_temporary_display(make_create_request(), now).status.error, vdd::StoreError::None);

  const vdd::LeaseRequest feed {
    vdd::kApiNamespaceGuid,
    lease_id(100),
    60'000,
    0
  };

  EXPECT_EQ(store.feed_lease(feed, now + std::chrono::milliseconds(30'000)).error, vdd::StoreError::LeaseNotFound);
  const auto query = store.query_lease(lease_id(100), now + std::chrono::milliseconds(30'000));
  EXPECT_EQ(query.lease_exists, 1u);
  EXPECT_EQ(query.remaining_ms, 0u);
}

TEST(VirtualDisplayDriverLeaseStore, RejectsAdditionalDisplayAfterLeaseExpiry) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();
  ASSERT_EQ(store.create_temporary_display(make_create_request(), now).status.error, vdd::StoreError::None);

  const auto created = store.create_temporary_display(
    make_create_request(lease_id(100), 201),
    now + std::chrono::milliseconds(30'000)
  );

  EXPECT_EQ(created.status.error, vdd::StoreError::LeaseNotFound);
  EXPECT_EQ(store.temporary_display_count(), 1u);
  EXPECT_FALSE(store.find_temporary_display(201));
}

TEST(VirtualDisplayDriverLeaseStore, AdditionalDisplaysUseExistingLeaseExpiry) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();
  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(100), 200), now).status.error, vdd::StoreError::None);

  auto request = make_create_request(lease_id(100), 201);
  request.requested_timeout_ms = 5'000;
  const auto created = store.create_temporary_display(request, now + std::chrono::seconds(10));

  EXPECT_EQ(created.status.error, vdd::StoreError::None);
  EXPECT_EQ(created.result.effective_timeout_ms, 30'000u);
  const auto display201 = require_temporary_display(store, 201);
  EXPECT_EQ(display201.timeout_ms, 30'000u);

  auto query = store.query_lease(lease_id(100), now + std::chrono::seconds(10));
  EXPECT_EQ(query.effective_timeout_ms, 30'000u);
  EXPECT_EQ(query.remaining_ms, 20'000u);
  EXPECT_EQ(query.temporary_display_count, 2u);

  EXPECT_EQ(store.reap_expired(now + std::chrono::milliseconds(29'999)), 0u);
  EXPECT_EQ(store.temporary_display_count(), 2u);
  EXPECT_EQ(store.reap_expired(now + std::chrono::milliseconds(30'000)), 2u);
  EXPECT_EQ(store.query_lease(lease_id(100), now + std::chrono::milliseconds(30'000)).lease_exists, 0u);
}

TEST(VirtualDisplayDriverLeaseStore, RemovesSingleDisplayFromLease) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();
  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(100), 200), now).status.error, vdd::StoreError::None);
  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(100), 201), now).status.error, vdd::StoreError::None);

  const vdd::LeaseDisplayRequest remove {
    vdd::kApiNamespaceGuid,
    lease_id(100),
    200
  };

  EXPECT_EQ(store.remove_temporary_display(remove).error, vdd::StoreError::None);
  EXPECT_EQ(store.temporary_display_count(), 1u);
  EXPECT_FALSE(store.find_temporary_display(200));
  EXPECT_TRUE(store.find_temporary_display(201));
  EXPECT_EQ(store.query_lease(lease_id(100), now).lease_exists, 1u);
}

TEST(VirtualDisplayDriverLeaseStore, RemoveRejectsWrongLeaseAndMissingDisplay) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();
  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(100), 200), now).status.error, vdd::StoreError::None);

  auto remove = vdd::LeaseDisplayRequest {
    vdd::kApiNamespaceGuid,
    lease_id(101),
    200
  };
  EXPECT_EQ(store.remove_temporary_display(remove).error, vdd::StoreError::DisplayNotFound);
  EXPECT_EQ(store.temporary_display_count(), 1u);

  remove = vdd::LeaseDisplayRequest {
    vdd::kApiNamespaceGuid,
    lease_id(100),
    201
  };
  EXPECT_EQ(store.remove_temporary_display(remove).error, vdd::StoreError::DisplayNotFound);
  EXPECT_EQ(store.temporary_display_count(), 1u);
  EXPECT_EQ(store.query_lease(lease_id(100), now).lease_exists, 1u);
}

TEST(VirtualDisplayDriverLeaseStore, TemporaryConnectorIndexesUseFixedReservedRange) {
  vdd::DisplayStore store {3, 4};
  const auto now = std::chrono::steady_clock::now();

  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 2;
  EXPECT_EQ(store.set_permanent_display_count(request).error, vdd::StoreError::None);

  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(1), 10), now).status.error, vdd::StoreError::None);
  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(2), 20), now).status.error, vdd::StoreError::None);

  auto display10 = require_temporary_display(store, 10);
  auto display20 = require_temporary_display(store, 20);
  EXPECT_EQ(display10.connector_index, 3u);
  EXPECT_EQ(display20.connector_index, 4u);

  request.display_count = 0;
  EXPECT_EQ(store.set_permanent_display_count(request).error, vdd::StoreError::None);
  display10 = require_temporary_display(store, 10);
  display20 = require_temporary_display(store, 20);
  EXPECT_EQ(display10.connector_index, 3u);
  EXPECT_EQ(display20.connector_index, 4u);
}

TEST(VirtualDisplayDriverLeaseStore, ReusesReservedConnectorIndexForSameDisplayId) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();

  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(1), 10), now).status.error, vdd::StoreError::None);
  auto display10 = require_temporary_display(store, 10);
  ASSERT_EQ(display10.connector_index, 2u);

  const vdd::LeaseDisplayRequest remove {
    vdd::kApiNamespaceGuid,
    lease_id(1),
    10
  };
  ASSERT_EQ(store.remove_temporary_display(remove).error, vdd::StoreError::None);

  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(2), 10), now).status.error, vdd::StoreError::None);
  display10 = require_temporary_display(store, 10);
  EXPECT_EQ(display10.connector_index, 2u);
}

TEST(VirtualDisplayDriverLeaseStore, RemovesDuplicateConnectorReservationWhenDisplayClaimsIt) {
  vdd::DisplayStore store {2, 4, {{10, 2}, {20, 2}}};
  const auto now = std::chrono::steady_clock::now();

  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(1), 10), now).status.error, vdd::StoreError::None);
  const auto display10 = require_temporary_display(store, 10);
  EXPECT_EQ(display10.connector_index, 2u);

  const vdd::LeaseDisplayRequest remove {
    vdd::kApiNamespaceGuid,
    lease_id(1),
    10
  };
  ASSERT_EQ(store.remove_temporary_display(remove).error, vdd::StoreError::None);

  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(2), 20), now).status.error, vdd::StoreError::None);
  const auto display20 = require_temporary_display(store, 20);
  EXPECT_EQ(display20.connector_index, 3u);
}

TEST(VirtualDisplayDriverLeaseStore, DoesNotReuseReservedConnectorIndexForDifferentDisplayId) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();

  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(1), 10), now).status.error, vdd::StoreError::None);
  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(2), 20), now).status.error, vdd::StoreError::None);

  const vdd::LeaseDisplayRequest remove {
    vdd::kApiNamespaceGuid,
    lease_id(1),
    10
  };
  ASSERT_EQ(store.remove_temporary_display(remove).error, vdd::StoreError::None);

  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(3), 30), now).status.error, vdd::StoreError::None);
  const auto display30 = require_temporary_display(store, 30);
  EXPECT_EQ(display30.connector_index, 4u);

  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(4), 10), now).status.error, vdd::StoreError::None);
  const auto display10 = require_temporary_display(store, 10);
  EXPECT_EQ(display10.connector_index, 2u);
}

TEST(VirtualDisplayDriverLeaseStore, ReusesInactiveReservedConnectorWhenPoolIsOtherwiseExhausted) {
  vdd::DisplayStore store {0, 2};
  const auto now = std::chrono::steady_clock::now();

  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(1), 10), now).status.error, vdd::StoreError::None);
  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(2), 20), now).status.error, vdd::StoreError::None);

  EXPECT_EQ(
    store.remove_temporary_display({vdd::kApiNamespaceGuid, lease_id(1), 10}).error,
    vdd::StoreError::None
  );
  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(3), 30), now).status.error, vdd::StoreError::None);
  const auto display30 = require_temporary_display(store, 30);
  EXPECT_EQ(display30.connector_index, 0u);

  EXPECT_EQ(
    store.create_temporary_display(make_create_request(lease_id(4), 10), now).status.error,
    vdd::StoreError::TemporaryDisplayLimitReached
  );
}

TEST(VirtualDisplayDriverLeaseStore, EphemeralIdentityAvoidsRetainedConnectorWhenAvailable) {
  vdd::DisplayStore store {0, 2};
  const auto now = std::chrono::steady_clock::now();

  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(1), 10), now).status.error, vdd::StoreError::None);
  auto display10 = require_temporary_display(store, 10);
  EXPECT_EQ(display10.connector_index, 0u);
  EXPECT_TRUE(display10.retain_identity);

  EXPECT_EQ(
    store.remove_temporary_display({vdd::kApiNamespaceGuid, lease_id(1), 10}).error,
    vdd::StoreError::None
  );

  auto request = make_create_request(lease_id(2), 10);
  request.flags = vdd::kCreateTemporaryDisplayFlagEphemeralIdentity;
  ASSERT_EQ(store.create_temporary_display(request, now).status.error, vdd::StoreError::None);
  display10 = require_temporary_display(store, 10);
  EXPECT_EQ(display10.connector_index, 1u);
  EXPECT_FALSE(display10.retain_identity);
  EXPECT_NE(display10.identity_display_id, 10u);
}

TEST(VirtualDisplayDriverLeaseStore, ReleasesWholeLease) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();
  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(100), 200), now).status.error, vdd::StoreError::None);
  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(100), 201), now).status.error, vdd::StoreError::None);

  const vdd::LeaseRequest release {
    vdd::kApiNamespaceGuid,
    lease_id(100),
    0,
    0
  };

  EXPECT_EQ(store.release_lease(release).error, vdd::StoreError::None);
  EXPECT_EQ(store.temporary_display_count(), 0u);
  EXPECT_EQ(store.query_lease(lease_id(100), now).lease_exists, 0u);
}

TEST(VirtualDisplayDriverLeaseStore, FeedAndReleaseMissingLeaseReturnNotFound) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();

  const vdd::LeaseRequest request {
    vdd::kApiNamespaceGuid,
    lease_id(404),
    0,
    0
  };

  EXPECT_EQ(store.feed_lease(request, now).error, vdd::StoreError::LeaseNotFound);
  EXPECT_EQ(store.release_lease(request).error, vdd::StoreError::LeaseNotFound);
}

TEST(VirtualDisplayDriverLeaseStore, ReapsExpiredDisplays) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();
  ASSERT_EQ(store.create_temporary_display(make_create_request(lease_id(100), 200), now).status.error, vdd::StoreError::None);

  EXPECT_EQ(store.reap_expired(now + std::chrono::milliseconds(29'999)), 0u);
  EXPECT_EQ(store.reap_expired(now + std::chrono::milliseconds(30'000)), 1u);
  EXPECT_EQ(store.temporary_display_count(), 0u);
  EXPECT_EQ(store.query_lease(lease_id(100), now).lease_exists, 0u);
}

TEST(VirtualDisplayDriverLeaseStore, SetsAndQueriesPermanentDisplayCount) {
  vdd::DisplayStore store {2, 4};
  ASSERT_EQ(
    store.create_temporary_display(make_create_request(lease_id(100), 200), std::chrono::steady_clock::now()).status.error,
    vdd::StoreError::None
  );

  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 2;
  request.width = 2560;
  request.height = 1440;
  request.physical_width_mm = 590;
  request.physical_height_mm = 330;
  request.refresh_rate_millihz = 120'000;
  set_display_name(request.display_name, "Desk Display");

  EXPECT_EQ(store.set_permanent_display_count(request).error, vdd::StoreError::None);

  const auto result = store.query_permanent_display_count();
  EXPECT_EQ(result.current_display_count, 2u);
  EXPECT_EQ(result.max_display_count, 2u);
  EXPECT_EQ(result.temporary_display_count, 1u);
  EXPECT_EQ(result.width, 2560u);
  EXPECT_EQ(result.height, 1440u);
  EXPECT_EQ(result.physical_width_mm, 590u);
  EXPECT_EQ(result.physical_height_mm, 330u);
  EXPECT_EQ(result.refresh_rate_millihz, 120'000u);
  EXPECT_EQ(vdd::trim_display_name(result.display_name), "Desk Display");
}

TEST(VirtualDisplayDriverLeaseStore, ConvertsPermanentSettingsToDisplayManifest) {
  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 2;
  request.width = 3840;
  request.height = 2160;
  request.physical_width_mm = 700;
  request.physical_height_mm = 390;
  request.refresh_rate_millihz = 144'000;
  set_display_name(request.display_name, "Desk Display");

  const auto manifest = vdd::display_manifest_from_permanent_settings(request, 4);

  EXPECT_EQ(manifest.version, vdd::kDisplayManifestVersion);
  EXPECT_EQ(manifest.profile_count, 2u);
  EXPECT_EQ(manifest.max_profile_count, 4u);
  EXPECT_EQ(manifest.profiles[0].connector_index, 0u);
  EXPECT_EQ(manifest.profiles[1].connector_index, 1u);
  EXPECT_EQ(manifest.profiles[0].display_id, vdd::permanent_display_id(0));
  EXPECT_EQ(manifest.profiles[1].display_id, vdd::permanent_display_id(1));
  EXPECT_NE(manifest.profiles[0].flags & vdd::kDisplayManifestProfileFlagPermanentIdentity, 0u);
  EXPECT_EQ(std::string(manifest.profiles[0].manufacturer_id), "SDD");
  EXPECT_EQ(manifest.profiles[0].product_code, vdd::permanent_product_code(0));
  EXPECT_EQ(manifest.profiles[0].allowed_mode_count, 1u);
  EXPECT_EQ(manifest.profiles[0].layout_policy, vdd::kDisplayManifestLayoutPolicyNone);
  EXPECT_EQ(manifest.profiles[0].orientation, vdd::kDisplayManifestOrientationDefault);
  EXPECT_EQ(manifest.profiles[0].allowed_modes[0].width, 3840u);
  EXPECT_EQ(manifest.profiles[0].allowed_modes[0].refresh_rate_millihz, 144'000u);
  EXPECT_EQ(vdd::trim_display_name(manifest.profiles[0].display_name), "Desk Display");
}

TEST(VirtualDisplayDriverLeaseStore, AppliesDisplayManifestAsPermanentState) {
  vdd::DisplayStore store {4, 4};
  auto manifest = vdd::display_manifest_from_permanent_settings(vdd::PermanentDisplayCountRequest {}, 4);
  manifest.profile_count = 1;
  auto &profile = manifest.profiles[0];
  profile.flags = vdd::kDisplayManifestProfileFlagRetainIdentity |
    vdd::kDisplayManifestProfileFlagPermanentIdentity;
  profile.connector_index = 2;
  profile.display_id = 0x7000000000000100ull;
  profile.container_id = vdd::container_guid_from_display_id(profile.display_id);
  profile.product_code = 0x4100;
  profile.serial_number = 0x100;
  profile.physical_width_mm = 620;
  profile.physical_height_mm = 350;
  profile.allowed_mode_count = 2;
  profile.native_mode_index = 1;
  profile.layout_policy = vdd::kDisplayManifestLayoutPolicyApplyAndPersist;
  profile.position_x = 1920;
  profile.position_y = 0;
  profile.allowed_modes[0] = {1920, 1080, 60'000};
  profile.allowed_modes[1] = {2560, 1440, 120'000};
  set_display_name(profile.display_name, "Side Display");

  EXPECT_EQ(store.apply_display_manifest(manifest).error, vdd::StoreError::None);

  EXPECT_EQ(store.permanent_display_count(), 1u);
  EXPECT_EQ(store.display_manifest().profiles[0].connector_index, 2u);
  EXPECT_EQ(store.display_manifest().profiles[0].layout_policy, vdd::kDisplayManifestLayoutPolicyApplyAndPersist);
  EXPECT_EQ(store.display_manifest().profiles[0].position_x, 1920);
  const auto legacy = store.query_permanent_display_count();
  EXPECT_EQ(legacy.width, 2560u);
  EXPECT_EQ(legacy.height, 1440u);
  EXPECT_EQ(legacy.refresh_rate_millihz, 120'000u);
  EXPECT_EQ(vdd::trim_display_name(legacy.display_name), "Side Display");
}

TEST(VirtualDisplayDriverLeaseStore, InitializesFromValidDisplayManifest) {
  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 1;
  auto manifest = vdd::display_manifest_from_permanent_settings(request, 4);
  manifest.profiles[0].position_x = 1280;
  manifest.profiles[0].layout_policy = vdd::kDisplayManifestLayoutPolicyApply;

  vdd::DisplayStore store {4, 4, {}, manifest};

  EXPECT_EQ(store.permanent_display_count(), 1u);
  EXPECT_EQ(store.display_manifest().profiles[0].connector_index, 0u);
  EXPECT_EQ(store.display_manifest().profiles[0].position_x, 1280);
}

TEST(VirtualDisplayDriverLeaseStore, RejectsTooManyPermanentDisplays) {
  vdd::DisplayStore store {2, 4};
  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 3;

  const auto result = store.set_permanent_display_count(request);
  EXPECT_EQ(result.error, vdd::StoreError::ValidationFailed);
  EXPECT_EQ(result.validation_error, vdd::ValidationError::PermanentDisplayCountTooHigh);
  EXPECT_EQ(store.permanent_display_count(), 0u);
}
