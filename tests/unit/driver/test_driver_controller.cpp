#include <gtest/gtest.h>
#include "virtual_display/driver/driver_controller.h"

#include <algorithm>
#include <string>
#include <string_view>
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
      return vdd::BackendError::None;
    }

    vdd::BackendDisplayResult arrive_temporary_display(const vdd::DisplayDescriptor &descriptor) override {
      events.push_back("arrive");
      arrived.push_back(descriptor);
      if (fail_arrive) {
        return {vdd::BackendError::Failed, 0, 0};
      }

      return {vdd::BackendError::None, adapter_luid, next_target_id++};
    }

    vdd::BackendError depart_temporary_display(const std::uint64_t display_id) override {
      departed.push_back(display_id);
      return fail_depart ? vdd::BackendError::Failed : vdd::BackendError::None;
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
    bool fail_render_adapter {};
    vdd::AdapterLuid adapter_luid {44, 2};
    vdd::SetRenderAdapterRequest render_adapter_request {};
    std::uint32_t next_target_id {7};
    std::vector<vdd::DisplayDescriptor> reserved {};
    std::vector<std::uint64_t> unreserved {};
    std::vector<vdd::DisplayDescriptor> arrived {};
    std::vector<std::uint64_t> departed {};
    std::vector<std::uint32_t> permanent_counts {};
    std::vector<vdd::PermanentDisplayCountRequest> permanent_settings {};
    std::vector<vdd::DisplayManifest> manifests {};
    std::vector<std::string> events {};

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

  vdd::LeaseRequest feed {};
  feed.lease_id = lease_id(100);
  feed.requested_timeout_ms = 60'000;
  EXPECT_EQ(controller.feed_lease(feed, now + std::chrono::seconds(1)).store_error, vdd::StoreError::LeaseNotFound);

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

  vdd::LeaseRequest feed {};
  feed.lease_id = lease_id(100);
  feed.requested_timeout_ms = 60'000;
  EXPECT_EQ(controller.feed_lease(feed, now + std::chrono::seconds(1)).store_error, vdd::StoreError::LeaseNotFound);

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
