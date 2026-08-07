#include <gtest/gtest.h>
#include "virtual_display/driver/driver_controller.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace vdd = virtual_display::driver;

namespace {
  constexpr std::uint64_t lease_id(const std::uint64_t suffix) {
    return vdd::kMinOpaqueLeaseId | suffix;
  }

  template<std::size_t N>
  void set_display_name(char (&destination)[N], const std::string_view value) {
    static_assert(N > 0);
    std::fill_n(destination, N, '\0');
    std::copy_n(value.data(), (std::min)(value.size(), N - 1), destination);
  }

  class FakeBackend: public vdd::DisplayDriverBackend {
  public:
    vdd::BackendError reserve_temporary_display_identity(const vdd::DisplayDescriptor &descriptor) override {
      reserved.push_back(descriptor);
      events.push_back("reserve");
      return fail_reserve ? vdd::BackendError::Failed : vdd::BackendError::None;
    }

    vdd::BackendError unreserve_temporary_display_identity(const std::uint64_t display_id) override {
      unreserved.push_back(display_id);
      events.push_back("unreserve");
      return fail_unreserve ? vdd::BackendError::Failed : vdd::BackendError::None;
    }

    vdd::BackendDisplayResult arrive_temporary_display(const vdd::DisplayDescriptor &descriptor) override {
      events.push_back("arrive");
      arrived.push_back(descriptor);
      if (fail_arrive) {
        return {vdd::BackendError::Failed, 0, 0};
      }

      return {vdd::BackendError::None, adapter_luid, next_target_id++};
    }

    vdd::BackendError depart_temporary_display(const std::uint64_t display_id, const std::uint64_t expected_generation) override {
      if (block_depart) {
        std::unique_lock lock {depart_mutex};
        depart_entered = true;
        depart_cv.notify_all();
        depart_cv.wait(lock, [&]() {
          return allow_depart;
        });
      }
      departed.push_back(display_id);
      departed_generations.push_back(expected_generation);
      return fail_depart ? vdd::BackendError::Failed : vdd::BackendError::None;
    }

    bool wait_for_departure(const std::chrono::milliseconds timeout) {
      std::unique_lock lock {depart_mutex};
      return depart_cv.wait_for(lock, timeout, [&]() {
        return depart_entered;
      });
    }

    void unblock_departure() {
      {
        std::lock_guard lock {depart_mutex};
        allow_depart = true;
      }
      depart_cv.notify_all();
    }

    vdd::BackendError set_permanent_display_count(const vdd::PermanentDisplayCountRequest &request) override {
      permanent_counts.push_back(request.display_count);
      permanent_settings.push_back(request);
      return fail_permanent ? vdd::BackendError::Failed : vdd::BackendError::None;
    }

    vdd::BackendError apply_display_manifest(const vdd::DisplayManifest &manifest) override {
      manifests.push_back(manifest);
      return set_permanent_display_count(vdd::permanent_settings_from_display_manifest(manifest));
    }

    bool fail_arrive {};
    bool fail_depart {};
    bool fail_permanent {};
    bool fail_reserve {};
    bool fail_unreserve {};
    bool fail_render_adapter {};
    bool block_depart {};
    vdd::AdapterLuid adapter_luid {44, 2};
    vdd::SetRenderAdapterRequest render_adapter_request {};
    std::uint32_t next_target_id {7};
    std::vector<vdd::DisplayDescriptor> reserved {};
    std::vector<std::uint64_t> unreserved {};
    std::vector<vdd::DisplayDescriptor> arrived {};
    std::vector<std::uint64_t> departed {};
    std::vector<std::uint64_t> departed_generations {};
    std::vector<std::uint32_t> permanent_counts {};
    std::vector<vdd::PermanentDisplayCountRequest> permanent_settings {};
    std::vector<vdd::DisplayManifest> manifests {};
    std::vector<std::string> events {};
    std::mutex depart_mutex {};
    std::condition_variable depart_cv {};
    bool depart_entered {};
    bool allow_depart {};

    vdd::BackendError set_render_adapter(const vdd::SetRenderAdapterRequest &request) override {
      render_adapter_request = request;
      events.push_back("set_render_adapter");
      return fail_render_adapter ? vdd::BackendError::Failed : vdd::BackendError::None;
    }
  };

  vdd::CreateTemporaryDisplayRequest make_create_request(
    const std::uint64_t lease_id_value = lease_id(100),
    const std::uint64_t display_id = 0x12345678
  ) {
    vdd::CreateTemporaryDisplayRequest request {};
    request.lease_id = lease_id_value;
    request.display_id = display_id;
    request.width = 2560;
    request.height = 1440;
    request.physical_width_mm = 590;
    request.physical_height_mm = 330;
    request.refresh_rate_millihz = 120'000;
    request.requested_timeout_ms = 30'000;
    set_display_name(request.display_name, "Sunshine HDR");
    return request;
  }

  vdd::OwnerCapability owner_capability() {
    vdd::OwnerCapability capability {};
    for (std::size_t index = 0; index < capability.bytes.size(); ++index) {
      capability.bytes[index] = static_cast<std::uint8_t>(index + 1);
    }
    return capability;
  }

  vdd::DriverController make_controller(FakeBackend &backend) {
    return vdd::DriverController {vdd::DisplayStore {4, 8}, backend};
  }
}  // namespace

TEST(VirtualDisplayDriverController, CreateTemporaryDisplayArrivesBackendAndReturnsOsIdentity) {
  FakeBackend backend;
  auto controller = make_controller(backend);

  const auto created = controller.create_temporary_display(make_create_request(), std::chrono::steady_clock::now());

  EXPECT_TRUE(created.status.ok());
  EXPECT_EQ(created.result.os_adapter_luid, (vdd::AdapterLuid {44, 2}));
  EXPECT_EQ(created.result.target_id, 7u);
  EXPECT_EQ(created.result.connector_index, 4u);
  ASSERT_EQ(backend.arrived.size(), 1u);
  EXPECT_EQ(backend.arrived[0].display_id, 0x12345678u);
  EXPECT_EQ(backend.arrived[0].container_id, vdd::container_guid_from_display_id(0x12345678u));
  EXPECT_EQ(backend.arrived[0].connector_index, 4u);
  EXPECT_EQ(backend.arrived[0].width, 2560u);
  EXPECT_EQ(backend.arrived[0].physical_width_mm, 590u);
  EXPECT_EQ(backend.arrived[0].physical_height_mm, 330u);
  EXPECT_TRUE(vdd::has_valid_edid_checksums(backend.arrived[0].edid));
  EXPECT_TRUE(vdd::has_hdr_static_metadata(backend.arrived[0].edid));
}

TEST(VirtualDisplayDriverController, ReclaimOwnedDisplayRenewsLeaseWithoutBackendOrIdentityChurn) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  const auto now = std::chrono::steady_clock::now();
  vdd::CreateTemporaryDisplayOwnedRequest owned {};
  owned.display = make_create_request();
  owned.owner_capability = owner_capability();
  ASSERT_TRUE(controller.create_temporary_display_owned(owned, now).status.ok());
  const auto before = controller.store().find_temporary_display(owned.display.display_id);
  ASSERT_TRUE(before);

  vdd::ReclaimTemporaryDisplayRequest reclaim {};
  reclaim.display_id = owned.display.display_id;
  reclaim.new_lease_id = lease_id(101);
  reclaim.requested_timeout_ms = 60'000;
  reclaim.owner_capability = owned.owner_capability;
  const auto reclaimed = controller.reclaim_temporary_display(reclaim, now + std::chrono::seconds(5));

  ASSERT_TRUE(reclaimed.status.ok());
  EXPECT_EQ(reclaimed.result.connector_index, before->connector_index);
  EXPECT_EQ(backend.events, (std::vector<std::string> {"reserve", "arrive"}));
  EXPECT_TRUE(backend.departed.empty());
  ASSERT_EQ(backend.arrived.size(), 1u);

  const auto after = controller.store().find_temporary_display(owned.display.display_id);
  ASSERT_TRUE(after);
  EXPECT_EQ(after->lease_id, lease_id(101));
  EXPECT_EQ(after->connector_index, before->connector_index);
  EXPECT_EQ(after->identity_display_id, before->identity_display_id);
  EXPECT_EQ(after->generation, before->generation);

  const auto state = controller.query_display_state();
  ASSERT_EQ(state.temporary_display_count, 1u);
  ASSERT_EQ(state.entry_count, 1u);
  EXPECT_EQ(state.entries[0].lease_id, 0u);
  EXPECT_EQ(state.entries[0].display_id, owned.display.display_id);
  EXPECT_EQ(state.entries[0].container_id, vdd::container_guid_from_display_id(owned.display.display_id));
}

TEST(VirtualDisplayDriverController, ReclaimWaitsForInFlightDepartureAndCannotReviveRemovedDisplay) {
  FakeBackend backend;
  backend.block_depart = true;
  auto controller = make_controller(backend);
  const auto now = std::chrono::steady_clock::now();
  vdd::CreateTemporaryDisplayOwnedRequest owned {};
  owned.display = make_create_request();
  owned.owner_capability = owner_capability();
  ASSERT_TRUE(controller.create_temporary_display_owned(owned, now).status.ok());

  vdd::LeaseDisplayRequest remove {};
  remove.lease_id = owned.display.lease_id;
  remove.display_id = owned.display.display_id;
  std::timed_mutex controller_mutex;
  vdd::ControllerStatus remove_status {};
  std::thread remover([&]() {
    std::unique_lock controller_lock {controller_mutex};
    remove_status = controller.remove_temporary_display(remove, &controller_lock);
  });

  if (!backend.wait_for_departure(std::chrono::seconds(1))) {
    backend.unblock_departure();
    remover.join();
    FAIL() << "backend departure did not start";
  }

  vdd::ReclaimTemporaryDisplayRequest reclaim {};
  reclaim.display_id = owned.display.display_id;
  reclaim.new_lease_id = lease_id(101);
  reclaim.requested_timeout_ms = 60'000;
  reclaim.owner_capability = owned.owner_capability;
  std::promise<void> reclaim_acquired_controller;
  auto reclaim_acquired_controller_future = reclaim_acquired_controller.get_future();
  std::atomic<bool> reclaim_returned {false};
  vdd::ControllerReclaimResult reclaim_result {};
  std::thread reclaimer([&]() {
    std::unique_lock controller_lock {controller_mutex};
    reclaim_acquired_controller.set_value();
    reclaim_result = controller.reclaim_temporary_display(reclaim, now + std::chrono::seconds(1), &controller_lock);
    reclaim_returned.store(true, std::memory_order_release);
  });

  reclaim_acquired_controller_future.wait();
  {
    // The reclaim call releases the controller lock while it waits for the
    // backend lifecycle fence. Acquiring it here proves the reclaimer reached
    // that wait without relying on a scheduling delay.
    std::unique_lock controller_lock {controller_mutex};
    EXPECT_FALSE(reclaim_returned.load(std::memory_order_acquire));
    const auto record = controller.store().find_temporary_display(owned.display.display_id);
    EXPECT_TRUE(record);
    if (record) {
      EXPECT_EQ(record->lease_id, owned.display.lease_id);
    }
  }

  backend.unblock_departure();
  remover.join();
  reclaimer.join();

  EXPECT_TRUE(remove_status.ok());
  EXPECT_EQ(reclaim_result.status.store_error, vdd::StoreError::LeaseNotFound);
  EXPECT_FALSE(controller.store().find_temporary_display(owned.display.display_id));
  EXPECT_EQ(backend.departed, (std::vector<std::uint64_t> {owned.display.display_id}));
}

TEST(VirtualDisplayDriverController, SetRenderAdapterForwardsPreferenceToBackend) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  vdd::SetRenderAdapterRequest request {};
  request.adapter_luid = {123, 4};

  const auto status = controller.set_render_adapter(request);

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(backend.render_adapter_request.adapter_luid, (vdd::AdapterLuid {123, 4}));
  EXPECT_EQ(backend.events, (std::vector<std::string> {"set_render_adapter"}));
}

TEST(VirtualDisplayDriverController, SetRenderAdapterRejectsUnknownFlags) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  vdd::SetRenderAdapterRequest request {};
  request.flags = 1;

  const auto status = controller.set_render_adapter(request);

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.validation_error, vdd::ValidationError::InvalidFlags);
  EXPECT_TRUE(backend.events.empty());
}

TEST(VirtualDisplayDriverController, CreateTemporaryDisplayReservesIdentityBeforeArrival) {
  FakeBackend backend;
  auto controller = make_controller(backend);

  const auto created = controller.create_temporary_display(make_create_request(), std::chrono::steady_clock::now());

  EXPECT_TRUE(created.status.ok());
  ASSERT_EQ(backend.reserved.size(), 1u);
  ASSERT_EQ(backend.arrived.size(), 1u);
  EXPECT_EQ(backend.reserved[0].display_id, 0x12345678u);
  EXPECT_EQ(backend.reserved[0].connector_index, 4u);
  EXPECT_EQ(backend.reserved[0].container_id, vdd::container_guid_from_display_id(0x12345678u));
  EXPECT_EQ(backend.events, (std::vector<std::string> {"reserve", "arrive"}));
}

TEST(VirtualDisplayDriverController, CreateEphemeralTemporaryDisplaySkipsIdentityReserve) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  auto request = make_create_request();
  request.flags = vdd::kCreateTemporaryDisplayFlagEphemeralIdentity;

  const auto created = controller.create_temporary_display(request, std::chrono::steady_clock::now());

  EXPECT_TRUE(created.status.ok());
  EXPECT_TRUE(backend.reserved.empty());
  ASSERT_EQ(backend.arrived.size(), 1u);
  EXPECT_FALSE(backend.arrived[0].retain_identity);
  EXPECT_NE(backend.arrived[0].container_id, vdd::container_guid_from_display_id(request.display_id));
  EXPECT_EQ(backend.events, (std::vector<std::string> {"arrive"}));
}

TEST(VirtualDisplayDriverController, CreateTemporaryDisplayRollsBackStoreWhenIdentityReserveFails) {
  FakeBackend backend;
  backend.fail_reserve = true;
  auto controller = make_controller(backend);

  const auto created = controller.create_temporary_display(make_create_request(), std::chrono::steady_clock::now());

  EXPECT_FALSE(created.status.ok());
  EXPECT_EQ(created.status.backend_error, vdd::BackendError::Failed);
  EXPECT_EQ(controller.store().temporary_display_count(), 0u);
  EXPECT_FALSE(controller.store().find_temporary_display(0x12345678));
  EXPECT_EQ(backend.reserved.size(), 1u);
  EXPECT_TRUE(backend.arrived.empty());
  EXPECT_EQ(backend.events, (std::vector<std::string> {"reserve"}));

  backend.fail_reserve = false;
  const auto retried = controller.create_temporary_display(
    make_create_request(lease_id(101), 0x87654321),
    std::chrono::steady_clock::now()
  );
  EXPECT_TRUE(retried.status.ok());
  EXPECT_EQ(retried.result.connector_index, 4u);
}

TEST(VirtualDisplayDriverController, CreateTemporaryDisplayRollsBackStoreWhenBackendFails) {
  FakeBackend backend;
  backend.fail_arrive = true;
  auto controller = make_controller(backend);

  const auto created = controller.create_temporary_display(make_create_request(), std::chrono::steady_clock::now());

  EXPECT_FALSE(created.status.ok());
  EXPECT_EQ(created.status.backend_error, vdd::BackendError::Failed);
  EXPECT_EQ(controller.store().temporary_display_count(), 0u);
  EXPECT_FALSE(controller.store().find_temporary_display(0x12345678));
  EXPECT_EQ(backend.unreserved, (std::vector<std::uint64_t> {0x12345678}));
  EXPECT_EQ(backend.events, (std::vector<std::string> {"reserve", "arrive", "unreserve"}));

  backend.fail_arrive = false;
  const auto retried = controller.create_temporary_display(
    make_create_request(lease_id(101), 0x87654321),
    std::chrono::steady_clock::now()
  );
  EXPECT_TRUE(retried.status.ok());
  EXPECT_EQ(retried.result.connector_index, 4u);
}

TEST(VirtualDisplayDriverController, CreateTemporaryDisplayRetainsConnectorWhenIdentityCleanupFails) {
  FakeBackend backend;
  backend.fail_arrive = true;
  backend.fail_unreserve = true;
  auto controller = make_controller(backend);

  const auto created = controller.create_temporary_display(make_create_request(), std::chrono::steady_clock::now());

  EXPECT_FALSE(created.status.ok());
  EXPECT_EQ(controller.store().temporary_display_count(), 0u);
  EXPECT_FALSE(controller.store().find_temporary_display(0x12345678));
  EXPECT_EQ(backend.unreserved, (std::vector<std::uint64_t> {0x12345678}));
  EXPECT_EQ(backend.events, (std::vector<std::string> {"reserve", "arrive", "unreserve"}));

  backend.fail_arrive = false;
  backend.fail_unreserve = false;
  const auto retried = controller.create_temporary_display(
    make_create_request(lease_id(101), 0x87654321),
    std::chrono::steady_clock::now()
  );
  EXPECT_TRUE(retried.status.ok());
  EXPECT_EQ(retried.result.connector_index, 5u);
}

TEST(VirtualDisplayDriverController, RemoveTemporaryDisplayDepartsBackend) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(), std::chrono::steady_clock::now()).status.ok());

  vdd::LeaseDisplayRequest remove {};
  remove.lease_id = lease_id(100);
  remove.display_id = 0x12345678;

  const auto status = controller.remove_temporary_display(remove);

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(controller.store().temporary_display_count(), 0u);
  ASSERT_EQ(backend.departed.size(), 1u);
  EXPECT_EQ(backend.departed[0], 0x12345678u);
}

TEST(VirtualDisplayDriverController, RemoveTemporaryDisplayKeepsStoreWhenBackendDepartFails) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  const auto now = std::chrono::steady_clock::now();
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(), now).status.ok());
  backend.fail_depart = true;

  vdd::LeaseDisplayRequest remove {};
  remove.lease_id = lease_id(100);
  remove.display_id = 0x12345678;

  const auto status = controller.remove_temporary_display(remove);

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.backend_error, vdd::BackendError::Failed);
  EXPECT_EQ(controller.store().temporary_display_count(), 1u);
  const auto retained = controller.store().find_temporary_display(0x12345678);
  ASSERT_TRUE(retained);
  EXPECT_TRUE(retained->pending_departure);
  EXPECT_EQ(controller.query_lease(lease_id(100), now).pending_departure_count, 1u);
  EXPECT_EQ(backend.departed, (std::vector<std::uint64_t> {0x12345678}));

  // A display stuck pending departure does not poison its lease: the owner can
  // still feed to keep any healthy sibling alive. The pending display keeps its
  // original deadline, though, so the reaper collects it on schedule.
  vdd::LeaseRequest feed {};
  feed.lease_id = lease_id(100);
  feed.requested_timeout_ms = 60'000;
  EXPECT_EQ(controller.feed_lease(feed, now + std::chrono::seconds(1)).store_error, vdd::StoreError::None);
  const auto after_feed = controller.store().find_temporary_display(0x12345678);
  ASSERT_TRUE(after_feed);
  EXPECT_TRUE(after_feed->pending_departure);
  EXPECT_EQ(after_feed->timeout_ms, 30'000u);
  EXPECT_EQ(after_feed->expires_at, retained->expires_at);

  // Once that original deadline lapses the lease stops being feedable.
  EXPECT_EQ(
    controller.feed_lease(feed, now + std::chrono::seconds(31)).store_error,
    vdd::StoreError::LeaseNotFound
  );

  backend.fail_depart = false;
  const auto retry_status = controller.remove_temporary_display(remove);
  EXPECT_TRUE(retry_status.ok());
  EXPECT_EQ(controller.store().temporary_display_count(), 0u);
  EXPECT_EQ(controller.query_lease(lease_id(100), now).lease_exists, 0u);
}

TEST(VirtualDisplayDriverController, ReleaseLeaseDepartsEveryDisplayInLease) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(lease_id(100), 200), std::chrono::steady_clock::now()).status.ok());
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(lease_id(100), 201), std::chrono::steady_clock::now()).status.ok());

  vdd::LeaseRequest release {};
  release.lease_id = lease_id(100);

  const auto status = controller.release_lease(release);

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(controller.store().temporary_display_count(), 0u);
  EXPECT_EQ(backend.departed, (std::vector<std::uint64_t> {200, 201}));
}

TEST(VirtualDisplayDriverController, ReleaseLeaseKeepsStoreWhenBackendDepartFails) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  const auto now = std::chrono::steady_clock::now();
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(lease_id(100), 200), now).status.ok());
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(lease_id(100), 201), now).status.ok());
  backend.fail_depart = true;

  vdd::LeaseRequest release {};
  release.lease_id = lease_id(100);

  const auto status = controller.release_lease(release);

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.backend_error, vdd::BackendError::Failed);
  EXPECT_EQ(controller.store().temporary_display_count(), 2u);
  for (const auto &display: controller.store().temporary_displays()) {
    EXPECT_TRUE(display.pending_departure);
  }
  const auto query = controller.query_lease(lease_id(100), now);
  EXPECT_EQ(query.lease_exists, 1u);
  EXPECT_EQ(query.pending_departure_count, 2u);
  EXPECT_EQ(backend.departed, (std::vector<std::uint64_t> {200, 201}));

  // Every display in the lease is pending departure, so feeding extends nothing,
  // but it is still accepted until those deadlines lapse.
  vdd::LeaseRequest feed {};
  feed.lease_id = lease_id(100);
  feed.requested_timeout_ms = 60'000;
  EXPECT_EQ(controller.feed_lease(feed, now + std::chrono::seconds(1)).store_error, vdd::StoreError::None);
  for (const auto &display: controller.store().temporary_displays()) {
    EXPECT_TRUE(display.pending_departure);
    EXPECT_EQ(display.timeout_ms, 30'000u);
  }
  EXPECT_EQ(
    controller.feed_lease(feed, now + std::chrono::seconds(31)).store_error,
    vdd::StoreError::LeaseNotFound
  );

  backend.fail_depart = false;
  const auto retry_status = controller.release_lease(release);
  EXPECT_TRUE(retry_status.ok());
  EXPECT_EQ(controller.store().temporary_display_count(), 0u);
  EXPECT_EQ(controller.query_lease(lease_id(100), now).lease_exists, 0u);
}

TEST(VirtualDisplayDriverController, FeedLeaseExtendsStoreWithoutBackendArrival) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  const auto now = std::chrono::steady_clock::now();
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(), now).status.ok());

  vdd::LeaseRequest feed {};
  feed.lease_id = lease_id(100);
  feed.requested_timeout_ms = 60'000;

  const auto status = controller.feed_lease(feed, now + std::chrono::seconds(10));

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(backend.arrived.size(), 1u);
  const auto query = controller.query_lease(lease_id(100), now + std::chrono::seconds(10));
  EXPECT_EQ(query.effective_timeout_ms, 60'000u);
  EXPECT_EQ(query.remaining_ms, 60'000u);
}

TEST(VirtualDisplayDriverController, ReapExpiredDisplaysDepartsBackend) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  const auto now = std::chrono::steady_clock::now();
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(), now).status.ok());

  EXPECT_EQ(controller.reap_expired(now + std::chrono::milliseconds(30'000)), 1u);
  EXPECT_EQ(controller.store().temporary_display_count(), 0u);
  EXPECT_EQ(backend.departed, (std::vector<std::uint64_t> {0x12345678}));
}

TEST(VirtualDisplayDriverController, ReapExpiredDisplaysKeepsStoreWhenBackendDepartFails) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  const auto now = std::chrono::steady_clock::now();
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(), now).status.ok());
  backend.fail_depart = true;

  EXPECT_EQ(controller.reap_expired(now + std::chrono::milliseconds(30'000)), 0u);
  EXPECT_EQ(controller.store().temporary_display_count(), 1u);
  const auto retained = controller.store().find_temporary_display(0x12345678);
  ASSERT_TRUE(retained);
  EXPECT_TRUE(retained->pending_departure);
  EXPECT_EQ(backend.departed, (std::vector<std::uint64_t> {0x12345678}));

  vdd::LeaseRequest feed {};
  feed.lease_id = lease_id(100);
  feed.requested_timeout_ms = 60'000;
  EXPECT_EQ(controller.feed_lease(feed, now + std::chrono::milliseconds(30'000)).store_error, vdd::StoreError::LeaseNotFound);
}

TEST(VirtualDisplayDriverController, SetPermanentDisplayCountReconcilesBackendBeforeStore) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 2;
  request.width = 3840;
  request.height = 2160;
  request.physical_width_mm = 700;
  request.physical_height_mm = 390;
  request.refresh_rate_millihz = 144'000;
  set_display_name(request.display_name, "Desk Display");

  const auto status = controller.set_permanent_display_count(request);

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(backend.permanent_counts, (std::vector<std::uint32_t> {2}));
  ASSERT_EQ(backend.permanent_settings.size(), 1u);
  EXPECT_EQ(backend.permanent_settings[0].width, 3840u);
  EXPECT_EQ(backend.permanent_settings[0].height, 2160u);
  EXPECT_EQ(backend.permanent_settings[0].physical_width_mm, 700u);
  EXPECT_EQ(backend.permanent_settings[0].physical_height_mm, 390u);
  EXPECT_EQ(controller.query_permanent_display_count().current_display_count, 2u);
  EXPECT_EQ(controller.query_permanent_display_count().refresh_rate_millihz, 144'000u);
}

TEST(VirtualDisplayDriverController, SetPermanentDisplayCountKeepsStoreUnchangedWhenBackendFails) {
  FakeBackend backend;
  backend.fail_permanent = true;
  auto controller = make_controller(backend);
  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 2;

  const auto status = controller.set_permanent_display_count(request);

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.backend_error, vdd::BackendError::Failed);
  EXPECT_EQ(controller.query_permanent_display_count().current_display_count, 0u);
}

TEST(VirtualDisplayDriverController, QueryDisplayStateReportsPermanentAndTemporaryIdentity) {
  FakeBackend backend;
  auto controller = make_controller(backend);

  vdd::PermanentDisplayCountRequest permanent {};
  permanent.display_count = 1;
  permanent.width = 3840;
  permanent.height = 2160;
  permanent.physical_width_mm = 700;
  permanent.physical_height_mm = 390;
  permanent.refresh_rate_millihz = 144'000;
  set_display_name(permanent.display_name, "Desk Display");
  ASSERT_TRUE(controller.set_permanent_display_count(permanent).ok());

  auto temporary = make_create_request(lease_id(100), 0x12345678);
  temporary.flags = vdd::kCreateTemporaryDisplayFlagEphemeralIdentity;
  ASSERT_TRUE(controller.create_temporary_display(temporary, std::chrono::steady_clock::now()).status.ok());

  const auto state = controller.query_display_state();

  EXPECT_EQ(state.permanent_display_count, 1u);
  EXPECT_EQ(state.temporary_display_count, 1u);
  ASSERT_EQ(state.entry_count, 2u);
  EXPECT_EQ(state.entries[0].kind, vdd::kDisplayStateKindPermanent);
  EXPECT_EQ(state.entries[0].display_id, vdd::permanent_display_id(0));
  EXPECT_EQ(state.entries[0].connector_index, 0u);
  EXPECT_EQ(state.entries[0].product_code, vdd::permanent_product_code(0));
  EXPECT_EQ(state.entries[0].serial_number, vdd::serial_number_from_display_id(vdd::permanent_display_id(0)));
  EXPECT_EQ(state.entries[0].flags & vdd::kDisplayStateFlagRetainIdentity, vdd::kDisplayStateFlagRetainIdentity);
  EXPECT_EQ(vdd::trim_display_name(state.entries[0].display_name), "Desk Display");

  EXPECT_EQ(state.entries[1].kind, vdd::kDisplayStateKindTemporary);
  EXPECT_EQ(state.entries[1].display_id, 0x12345678u);
  EXPECT_EQ(state.entries[1].lease_id, 0u);
  EXPECT_NE(state.entries[1].container_id, vdd::container_guid_from_display_id(0x12345678u));
  EXPECT_NE(state.entries[1].product_code, vdd::product_code_from_display_id(0x12345678u));
  EXPECT_EQ(state.entries[1].flags & vdd::kDisplayStateFlagRetainIdentity, 0u);
  EXPECT_EQ(state.entries[1].physical_width_mm, temporary.physical_width_mm);
}

TEST(VirtualDisplayDriverController, ApplyDisplayManifestReportsPerSlotIdentity) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  vdd::DisplayManifest manifest {};
  manifest.profile_count = 1;
  manifest.max_profile_count = 4;
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
  profile.native_mode_index = 1;
  profile.allowed_mode_count = 2;
  profile.layout_policy = vdd::kDisplayManifestLayoutPolicyApplyAndPersist;
  profile.position_x = -2560;
  profile.position_y = 200;
  profile.allowed_modes[0] = {1920, 1080, 60'000};
  profile.allowed_modes[1] = {2560, 1440, 120'000};
  set_display_name(profile.display_name, "Side Display");

  const auto status = controller.apply_display_manifest(manifest);

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(controller.query_display_manifest().profiles[0].layout_policy, vdd::kDisplayManifestLayoutPolicyApplyAndPersist);
  EXPECT_EQ(controller.query_display_manifest().profiles[0].position_x, -2560);
  EXPECT_EQ(backend.permanent_counts, (std::vector<std::uint32_t> {1}));
  ASSERT_EQ(backend.manifests.size(), 1u);
  EXPECT_EQ(backend.manifests[0].profiles[0].connector_index, 2u);
  const auto state = controller.query_display_state();
  ASSERT_EQ(state.entry_count, 1u);
  EXPECT_EQ(state.entries[0].connector_index, 2u);
  EXPECT_EQ(state.entries[0].display_id, profile.display_id);
  EXPECT_EQ(state.entries[0].product_code, 0x4100u);
  EXPECT_EQ(state.entries[0].width, 2560u);
  EXPECT_EQ(state.entries[0].height, 1440u);
  EXPECT_EQ(state.entries[0].refresh_rate_millihz, 120'000u);
  EXPECT_EQ(state.entries[0].flags & vdd::kDisplayStateFlagHdrSupported, 0u);
  EXPECT_EQ(state.entries[0].flags & vdd::kDisplayStateFlagRetainIdentity, vdd::kDisplayStateFlagRetainIdentity);
  EXPECT_EQ(vdd::trim_display_name(state.entries[0].display_name), "Side Display");
}

TEST(VirtualDisplayDriverController, DeparturesCarryTheRecordGenerationFence) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  const auto now = std::chrono::steady_clock::now();

  ASSERT_TRUE(controller.create_temporary_display(make_create_request(), now).status.ok());
  const auto record = controller.store().find_temporary_display(0x12345678u);
  ASSERT_TRUE(record);
  const auto generation = record->generation;
  ASSERT_NE(generation, 0u);

  vdd::LeaseDisplayRequest remove {};
  remove.lease_id = lease_id(100);
  remove.display_id = 0x12345678u;
  ASSERT_TRUE(controller.remove_temporary_display(remove).ok());

  ASSERT_EQ(backend.departed.size(), 1u);
  EXPECT_EQ(backend.departed[0], 0x12345678u);
  ASSERT_EQ(backend.departed_generations.size(), 1u);
  EXPECT_EQ(backend.departed_generations[0], generation);
}

TEST(VirtualDisplayDriverController, ReapExpiredDepartsWithTheSnapshotGeneration) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  const auto now = std::chrono::steady_clock::now();

  ASSERT_TRUE(controller.create_temporary_display(make_create_request(), now).status.ok());
  const auto record = controller.store().find_temporary_display(0x12345678u);
  ASSERT_TRUE(record);

  const auto removed = controller.reap_expired(now + std::chrono::hours(1));

  EXPECT_EQ(removed, 1u);
  ASSERT_EQ(backend.departed_generations.size(), 1u);
  EXPECT_EQ(backend.departed_generations[0], record->generation);
  EXPECT_FALSE(controller.store().find_temporary_display(0x12345678u));
}

TEST(VirtualDisplayDriverController, SecureReclaimKeepsGenerationFenceValidForDeparturesAndRecreation) {
  // Field flow (Vibepollo recovery): a host that lost sight of its display
  // securely reclaims it under a fresh lease, decides the reclaimed display is
  // not reusable, removes it, and recreates the same display_id. The departure
  // issued after the reclaim must still carry the original record generation
  // (reclaim rotates only the lease_id), and the recreation must get a fresh
  // generation so old-lineage departures cannot retract it.
  FakeBackend backend;
  auto controller = make_controller(backend);
  const auto now = std::chrono::steady_clock::now();

  vdd::CreateTemporaryDisplayOwnedRequest owned {};
  owned.display = make_create_request();
  owned.owner_capability = owner_capability();
  ASSERT_TRUE(controller.create_temporary_display_owned(owned, now).status.ok());
  const auto before = controller.store().find_temporary_display(owned.display.display_id);
  ASSERT_TRUE(before);
  const auto original_generation = before->generation;
  ASSERT_NE(original_generation, 0u);

  vdd::ReclaimTemporaryDisplayRequest reclaim {};
  reclaim.display_id = owned.display.display_id;
  reclaim.new_lease_id = lease_id(101);
  reclaim.requested_timeout_ms = 60'000;
  reclaim.owner_capability = owned.owner_capability;
  ASSERT_TRUE(controller.reclaim_temporary_display(reclaim, now + std::chrono::seconds(1)).status.ok());
  const auto reclaimed = controller.store().find_temporary_display(owned.display.display_id);
  ASSERT_TRUE(reclaimed);
  EXPECT_EQ(reclaimed->lease_id, lease_id(101));
  EXPECT_EQ(reclaimed->generation, original_generation);

  vdd::LeaseDisplayRequest remove {};
  remove.lease_id = lease_id(101);
  remove.display_id = owned.display.display_id;
  ASSERT_TRUE(controller.remove_temporary_display(remove).ok());
  ASSERT_EQ(backend.departed_generations.size(), 1u);
  EXPECT_EQ(backend.departed_generations[0], original_generation);
  EXPECT_FALSE(controller.store().find_temporary_display(owned.display.display_id));

  vdd::CreateTemporaryDisplayOwnedRequest recreated {};
  recreated.display = make_create_request(lease_id(102));
  recreated.owner_capability = owner_capability();
  ASSERT_TRUE(controller.create_temporary_display_owned(recreated, now + std::chrono::seconds(2)).status.ok());
  const auto after = controller.store().find_temporary_display(owned.display.display_id);
  ASSERT_TRUE(after);
  EXPECT_NE(after->generation, original_generation);
  ASSERT_EQ(backend.arrived.size(), 2u);
  EXPECT_EQ(backend.arrived[1].generation, after->generation);
}

TEST(VirtualDisplayDriverController, PersistentDepartFailureLeavesOwnerAnEscapePathWithoutDriverRestart) {
  // Field wedge (vibeshine#253 family; Vibepollo 1.18.3-stable.4 reports):
  // when the backend departure fails persistently (wedged IddCx teardown),
  // every owner-facing path for the display_id fail-closes:
  //   - remove marks the record pending_departure and fails,
  //   - the 1 Hz reaper retries the same failing departure forever,
  //   - secure reclaim refuses pending-departure records,
  //   - create's pending-departure heal re-departs, fails, and aborts.
  // Nothing the owning host can do clears the wedge; in the field only a
  // driver restart (or the "switch driver to SudoVDA and back" workaround,
  // which restarts the device) recovers. A bounded number of owner retries
  // with proven capability must eventually make forward progress instead.
  FakeBackend backend;
  auto controller = make_controller(backend);
  const auto now = std::chrono::steady_clock::now();

  vdd::CreateTemporaryDisplayOwnedRequest owned {};
  owned.display = make_create_request();
  owned.owner_capability = owner_capability();
  ASSERT_TRUE(controller.create_temporary_display_owned(owned, now).status.ok());

  backend.fail_depart = true;

  // Session teardown fails and leaves the record pending departure.
  vdd::LeaseDisplayRequest remove {};
  remove.lease_id = owned.display.lease_id;
  remove.display_id = owned.display.display_id;
  const auto remove_status = controller.remove_temporary_display(remove);
  EXPECT_EQ(remove_status.backend_error, vdd::BackendError::Failed);
  {
    const auto record = controller.store().find_temporary_display(owned.display.display_id);
    ASSERT_TRUE(record);
    EXPECT_TRUE(record->pending_departure);
  }

  // The reaper cannot collect it while the backend keeps failing.
  EXPECT_EQ(controller.reap_expired(now + std::chrono::hours(1)), 0u);
  EXPECT_TRUE(controller.store().find_temporary_display(owned.display.display_id));

  // Recovery reclaim is refused for pending-departure records.
  vdd::ReclaimTemporaryDisplayRequest reclaim {};
  reclaim.display_id = owned.display.display_id;
  reclaim.new_lease_id = lease_id(101);
  reclaim.requested_timeout_ms = 60'000;
  reclaim.owner_capability = owned.owner_capability;
  EXPECT_EQ(
    controller.reclaim_temporary_display(reclaim, now + std::chrono::seconds(1)).status.store_error,
    vdd::StoreError::LeaseNotFound
  );

  // The host's bounded create retries (attempt 1/3 .. 3/3 in the field logs,
  // give it a couple extra here) must not all fail while the owner presents
  // the same capability that created the display.
  const auto wedged_connector = controller.store().find_temporary_display(owned.display.display_id)->connector_index;
  bool made_forward_progress = false;
  for (std::uint64_t attempt = 0; attempt < 5 && !made_forward_progress; ++attempt) {
    vdd::CreateTemporaryDisplayOwnedRequest retry {};
    retry.display = make_create_request(lease_id(110 + attempt));
    retry.owner_capability = owned.owner_capability;
    made_forward_progress = controller
                              .create_temporary_display_owned(retry, now + std::chrono::seconds(2 + attempt))
                              .status.ok();
  }
  EXPECT_TRUE(made_forward_progress)
    << "every owner-facing path fail-closed; only a driver restart clears the display_id";

  // The escape must not reuse the wedged connector: the recreated display gets
  // a fresh connector and the condemned lineage's connector stays quarantined.
  const auto recreated = controller.store().find_temporary_display(owned.display.display_id);
  ASSERT_TRUE(recreated);
  EXPECT_FALSE(recreated->pending_departure);
  EXPECT_NE(recreated->connector_index, wedged_connector);

  // A stranger presenting the wrong capability must never be able to evict a
  // pending record: recreate the wedge under a fresh id and check fail-closed
  // behavior survives for non-owners.
  backend.fail_depart = false;
  vdd::CreateTemporaryDisplayOwnedRequest other {};
  other.display = make_create_request(lease_id(200), 0x777);
  other.owner_capability = owner_capability();
  ASSERT_TRUE(controller.create_temporary_display_owned(other, now).status.ok());
  backend.fail_depart = true;
  vdd::LeaseDisplayRequest remove_other {};
  remove_other.lease_id = lease_id(200);
  remove_other.display_id = 0x777;
  EXPECT_EQ(controller.remove_temporary_display(remove_other).backend_error, vdd::BackendError::Failed);
  vdd::CreateTemporaryDisplayOwnedRequest stranger {};
  stranger.display = make_create_request(lease_id(201), 0x777);
  stranger.owner_capability = owner_capability();
  stranger.owner_capability.bytes[0] ^= 0xff;
  EXPECT_EQ(
    controller.create_temporary_display_owned(stranger, now + std::chrono::seconds(1)).status.backend_error,
    vdd::BackendError::Failed
  );
  {
    const auto still_pending = controller.store().find_temporary_display(0x777);
    ASSERT_TRUE(still_pending);
    EXPECT_TRUE(still_pending->pending_departure);
  }
}

TEST(VirtualDisplayDriverController, BusyBackendLockDoesNotPoisonLiveDisplayWithPendingDeparture) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  const auto now = std::chrono::steady_clock::now();

  ASSERT_TRUE(controller.create_temporary_display(make_create_request(lease_id(100), 0x111), now).status.ok());
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(lease_id(102), 0x222), now).status.ok());

  // Hold the backend-call slot via a departure that blocks inside the backend.
  backend.block_depart = true;
  vdd::LeaseDisplayRequest remove_blocked {};
  remove_blocked.lease_id = lease_id(100);
  remove_blocked.display_id = 0x111;
  vdd::ControllerStatus blocked_status {};
  std::thread remover([&]() {
    blocked_status = controller.remove_temporary_display(remove_blocked);
  });
  if (!backend.wait_for_departure(std::chrono::seconds(1))) {
    backend.unblock_departure();
    remover.join();
    FAIL() << "backend departure did not start";
  }

  // A second remove cannot acquire the backend-call slot within the timeout.
  // It must report Busy and leave the live record untouched: marking it
  // pending-departure here would freeze its lease expiry and let the reaper
  // depart a healthy, in-use display over a mere lock timeout.
  vdd::LeaseDisplayRequest remove_busy {};
  remove_busy.lease_id = lease_id(102);
  remove_busy.display_id = 0x222;
  const auto busy_status = controller.remove_temporary_display(remove_busy);
  EXPECT_EQ(busy_status.backend_error, vdd::BackendError::Busy);
  {
    const auto untouched = controller.store().find_temporary_display(0x222);
    ASSERT_TRUE(untouched);
    EXPECT_FALSE(untouched->pending_departure);
  }

  backend.unblock_departure();
  remover.join();
  EXPECT_TRUE(blocked_status.ok());

  // Once the slot frees up the same remove succeeds normally.
  backend.block_depart = false;
  EXPECT_TRUE(controller.remove_temporary_display(remove_busy).ok());
  EXPECT_FALSE(controller.store().find_temporary_display(0x222));
}
