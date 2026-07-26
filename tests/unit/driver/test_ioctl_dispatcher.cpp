#include <gtest/gtest.h>
#include "virtual_display/driver/ioctl_dispatcher.h"

#include <algorithm>
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
    vdd::BackendDisplayResult arrive_temporary_display(const vdd::DisplayDescriptor &descriptor) override {
      arrived.push_back(descriptor);
      if (fail_arrive) {
        return {vdd::BackendError::Failed, 0, 0};
      }

      return {vdd::BackendError::None, {99, 3}, next_target_id++};
    }

    vdd::BackendError depart_temporary_display(const std::uint64_t display_id) override {
      departed.push_back(display_id);
      return fail_depart ? vdd::BackendError::Failed : vdd::BackendError::None;
    }

    vdd::BackendError set_permanent_display_count(const vdd::PermanentDisplayCountRequest &request) override {
      permanent_counts.push_back(request.display_count);
      permanent_settings.push_back(request);
      return vdd::BackendError::None;
    }

    vdd::BackendError apply_display_manifest(const vdd::DisplayManifest &manifest) override {
      manifests.push_back(manifest);
      return set_permanent_display_count(vdd::permanent_settings_from_display_manifest(manifest));
    }

    vdd::BackendError set_render_adapter(const vdd::SetRenderAdapterRequest &request) override {
      render_adapter_requests.push_back(request);
      return vdd::BackendError::None;
    }

    bool fail_arrive {};
    bool fail_depart {};
    std::uint32_t next_target_id {12};
    std::vector<vdd::DisplayDescriptor> arrived {};
    std::vector<std::uint64_t> departed {};
    std::vector<std::uint32_t> permanent_counts {};
    std::vector<vdd::PermanentDisplayCountRequest> permanent_settings {};
    std::vector<vdd::DisplayManifest> manifests {};
    std::vector<vdd::SetRenderAdapterRequest> render_adapter_requests {};
  };

  struct Harness {
    FakeBackend backend {};
    vdd::DriverController controller {vdd::DisplayStore {4, 8}, backend};
    vdd::IoctlDispatcher dispatcher {controller};
  };

  vdd::CreateTemporaryDisplayRequest make_create_request() {
    vdd::CreateTemporaryDisplayRequest request {};
    request.lease_id = lease_id(100);
    request.display_id = 200;
    request.width = 1920;
    request.height = 1080;
    request.physical_width_mm = 530;
    request.physical_height_mm = 300;
    request.refresh_rate_millihz = 60'000;
    request.requested_timeout_ms = 30'000;
    set_display_name(request.display_name, "Sunshine");
    return request;
  }

  vdd::OwnerCapability owner_capability(const std::uint8_t seed = 1) {
    vdd::OwnerCapability capability {};
    for (std::size_t index = 0; index < capability.bytes.size(); ++index) {
      capability.bytes[index] = static_cast<std::uint8_t>(seed + index);
    }
    return capability;
  }
}  // namespace

TEST(VirtualDisplayDriverIoctlDispatcher, ReturnsProtocolVersion) {
  Harness harness;
  vdd::ProtocolVersion version {};

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlGetProtocolVersion,
    nullptr,
    0,
    &version,
    sizeof(version),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(result.bytes_returned, sizeof(version));
  EXPECT_EQ(version.api_namespace, vdd::kApiNamespaceGuid);
}

TEST(VirtualDisplayDriverIoctlDispatcher, SetRenderAdapterForwardsPreference) {
  Harness harness;
  vdd::SetRenderAdapterRequest request {};
  request.adapter_luid = {456, 7};

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlSetRenderAdapter,
    &request,
    sizeof(request),
    nullptr,
    0,
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::Success);
  ASSERT_EQ(harness.backend.render_adapter_requests.size(), 1u);
  EXPECT_EQ(harness.backend.render_adapter_requests[0].adapter_luid, (vdd::AdapterLuid {456, 7}));
}

TEST(VirtualDisplayDriverIoctlDispatcher, RejectsShortCreateInput) {
  Harness harness;
  vdd::CreateTemporaryDisplayResult output {};

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlCreateTemporaryDisplay,
    nullptr,
    0,
    &output,
    sizeof(output),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::InvalidInputBuffer);
  EXPECT_EQ(result.bytes_returned, 0u);
}

TEST(VirtualDisplayDriverIoctlDispatcher, CreateTemporaryDisplayWritesResultAndArrivesBackend) {
  Harness harness;
  auto request = make_create_request();
  vdd::CreateTemporaryDisplayResult output {};

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlCreateTemporaryDisplay,
    &request,
    sizeof(request),
    &output,
    sizeof(output),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(result.bytes_returned, sizeof(output));
  EXPECT_EQ(output.lease_id, request.lease_id);
  EXPECT_EQ(output.display_id, request.display_id);
  EXPECT_EQ(output.os_adapter_luid, (vdd::AdapterLuid {99, 3}));
  EXPECT_EQ(output.target_id, 12u);
  ASSERT_EQ(harness.backend.arrived.size(), 1u);
  EXPECT_EQ(harness.backend.arrived[0].physical_width_mm, 530u);
  EXPECT_EQ(harness.backend.arrived[0].physical_height_mm, 300u);
  EXPECT_TRUE(vdd::has_hdr_static_metadata(harness.backend.arrived[0].edid));
}

TEST(VirtualDisplayDriverIoctlDispatcher, OwnedCreateAndReclaimRoundTripWithoutBackendChurn) {
  Harness harness;
  const auto now = std::chrono::steady_clock::now();
  vdd::CreateTemporaryDisplayOwnedRequest owned {};
  owned.display = make_create_request();
  owned.owner_capability = owner_capability();
  vdd::CreateTemporaryDisplayResult created {};
  ASSERT_EQ(
    harness.dispatcher.dispatch(
      vdd::kIoctlCreateTemporaryDisplayOwned,
      &owned,
      sizeof(owned),
      &created,
      sizeof(created),
      now
    ).status,
    vdd::IoctlStatus::Success
  );
  ASSERT_EQ(harness.backend.arrived.size(), 1u);

  vdd::ReclaimTemporaryDisplayRequest reclaim {};
  reclaim.display_id = owned.display.display_id;
  reclaim.new_lease_id = lease_id(101);
  reclaim.requested_timeout_ms = 60'000;
  reclaim.owner_capability = owned.owner_capability;
  vdd::ReclaimTemporaryDisplayResult reclaimed {};
  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlReclaimTemporaryDisplay,
    &reclaim,
    sizeof(reclaim),
    &reclaimed,
    sizeof(reclaimed),
    now + std::chrono::seconds(1)
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(result.bytes_returned, sizeof(reclaimed));
  EXPECT_EQ(reclaimed.lease_id, lease_id(101));
  EXPECT_EQ(reclaimed.display_id, owned.display.display_id);
  EXPECT_EQ(reclaimed.temporary_display_count, 1u);
  EXPECT_EQ(harness.backend.arrived.size(), 1u);
  EXPECT_TRUE(harness.backend.departed.empty());

  reclaim.owner_capability = owner_capability(9);
  EXPECT_EQ(
    harness.dispatcher.dispatch(
      vdd::kIoctlReclaimTemporaryDisplay,
      &reclaim,
      sizeof(reclaim),
      &reclaimed,
      sizeof(reclaimed),
      now + std::chrono::seconds(2)
    ).status,
    vdd::IoctlStatus::NotFound
  );
}

TEST(VirtualDisplayDriverIoctlDispatcher, CreateTemporaryDisplayRejectsShortOutputBeforeBackendArrival) {
  Harness harness;
  auto request = make_create_request();

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlCreateTemporaryDisplay,
    &request,
    sizeof(request),
    nullptr,
    0,
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::InvalidOutputBuffer);
  EXPECT_EQ(result.bytes_returned, 0u);
  EXPECT_EQ(harness.controller.store().temporary_display_count(), 0u);
  EXPECT_TRUE(harness.backend.arrived.empty());
  EXPECT_TRUE(harness.backend.departed.empty());
}

TEST(VirtualDisplayDriverIoctlDispatcher, CreateTemporaryDisplayMapsBackendFailure) {
  Harness harness;
  harness.backend.fail_arrive = true;
  auto request = make_create_request();
  vdd::CreateTemporaryDisplayResult output {};

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlCreateTemporaryDisplay,
    &request,
    sizeof(request),
    &output,
    sizeof(output),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::BackendFailed);
  EXPECT_EQ(result.bytes_returned, 0u);
  EXPECT_EQ(harness.controller.store().temporary_display_count(), 0u);
}

TEST(VirtualDisplayDriverIoctlDispatcher, RemoveTemporaryDisplayDepartsBackend) {
  Harness harness;
  auto request = make_create_request();
  vdd::CreateTemporaryDisplayResult output {};
  ASSERT_EQ(
    harness.dispatcher.dispatch(vdd::kIoctlCreateTemporaryDisplay, &request, sizeof(request), &output, sizeof(output), std::chrono::steady_clock::now()).status,
    vdd::IoctlStatus::Success
  );

  vdd::LeaseDisplayRequest remove {};
  remove.lease_id = request.lease_id;
  remove.display_id = request.display_id;
  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlRemoveTemporaryDisplay,
    &remove,
    sizeof(remove),
    nullptr,
    0,
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(result.bytes_returned, 0u);
  EXPECT_EQ(harness.backend.departed, (std::vector<std::uint64_t> {request.display_id}));
}

TEST(VirtualDisplayDriverIoctlDispatcher, RemoveTemporaryDisplayBackendFailureKeepsStore) {
  Harness harness;
  auto request = make_create_request();
  vdd::CreateTemporaryDisplayResult output {};
  ASSERT_EQ(
    harness.dispatcher.dispatch(vdd::kIoctlCreateTemporaryDisplay, &request, sizeof(request), &output, sizeof(output), std::chrono::steady_clock::now()).status,
    vdd::IoctlStatus::Success
  );
  harness.backend.fail_depart = true;

  vdd::LeaseDisplayRequest remove {};
  remove.lease_id = request.lease_id;
  remove.display_id = request.display_id;
  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlRemoveTemporaryDisplay,
    &remove,
    sizeof(remove),
    nullptr,
    0,
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::BackendFailed);
  EXPECT_EQ(result.bytes_returned, 0u);
  EXPECT_EQ(harness.controller.store().temporary_display_count(), 1u);
  EXPECT_TRUE(harness.controller.store().find_temporary_display(request.display_id));
  EXPECT_EQ(harness.backend.departed, (std::vector<std::uint64_t> {request.display_id}));

  // The stuck display does not poison the lease; feeding stays available until
  // that display's own deadline lapses.
  vdd::LeaseRequest feed {};
  feed.lease_id = request.lease_id;
  feed.requested_timeout_ms = 60'000;
  const auto feed_result = harness.dispatcher.dispatch(
    vdd::kIoctlFeedLease,
    &feed,
    sizeof(feed),
    nullptr,
    0,
    std::chrono::steady_clock::now()
  );
  EXPECT_EQ(feed_result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(feed_result.bytes_returned, 0u);
}

TEST(VirtualDisplayDriverIoctlDispatcher, QueryAndFeedLeaseUseLeaseApi) {
  Harness harness;
  const auto now = std::chrono::steady_clock::now();
  auto request = make_create_request();
  vdd::CreateTemporaryDisplayResult output {};
  ASSERT_EQ(
    harness.dispatcher.dispatch(vdd::kIoctlCreateTemporaryDisplay, &request, sizeof(request), &output, sizeof(output), now).status,
    vdd::IoctlStatus::Success
  );

  vdd::LeaseRequest feed {};
  feed.lease_id = request.lease_id;
  feed.requested_timeout_ms = 60'000;
  const auto feed_result = harness.dispatcher.dispatch(
    vdd::kIoctlFeedLease,
    &feed,
    sizeof(feed),
    nullptr,
    0,
    now + std::chrono::seconds(10)
  );
  EXPECT_EQ(feed_result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(feed_result.bytes_returned, 0u);

  vdd::QueryLeaseResult query {};
  const auto query_result = harness.dispatcher.dispatch(
    vdd::kIoctlQueryLease,
    &feed,
    sizeof(feed),
    &query,
    sizeof(query),
    now + std::chrono::seconds(10)
  );

  EXPECT_EQ(query_result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(query_result.bytes_returned, sizeof(query));
  EXPECT_EQ(query.lease_exists, 1u);
  EXPECT_EQ(query.effective_timeout_ms, 60'000u);
}

TEST(VirtualDisplayDriverIoctlDispatcher, QueryLeaseRejectsWrongNamespace) {
  Harness harness;
  vdd::LeaseRequest query_request {};
  query_request.lease_id = lease_id(100);
  query_request.api_namespace.data1 ^= 1;
  vdd::QueryLeaseResult query {};

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlQueryLease,
    &query_request,
    sizeof(query_request),
    &query,
    sizeof(query),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::InvalidRequest);
  EXPECT_EQ(result.bytes_returned, 0u);
  EXPECT_EQ(result.controller_status.validation_error, vdd::ValidationError::WrongApiNamespace);
}

TEST(VirtualDisplayDriverIoctlDispatcher, QueryLeaseRejectsMissingLeaseId) {
  Harness harness;
  vdd::LeaseRequest query_request {};
  vdd::QueryLeaseResult query {};

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlQueryLease,
    &query_request,
    sizeof(query_request),
    &query,
    sizeof(query),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::InvalidRequest);
  EXPECT_EQ(result.bytes_returned, 0u);
  EXPECT_EQ(result.controller_status.validation_error, vdd::ValidationError::MissingLeaseId);
}

TEST(VirtualDisplayDriverIoctlDispatcher, ReleaseLeaseBackendFailureKeepsStore) {
  Harness harness;
  const auto now = std::chrono::steady_clock::now();
  auto first = make_create_request();
  auto second = make_create_request();
  second.display_id = 201;
  vdd::CreateTemporaryDisplayResult output {};
  ASSERT_EQ(
    harness.dispatcher.dispatch(vdd::kIoctlCreateTemporaryDisplay, &first, sizeof(first), &output, sizeof(output), now).status,
    vdd::IoctlStatus::Success
  );
  ASSERT_EQ(
    harness.dispatcher.dispatch(vdd::kIoctlCreateTemporaryDisplay, &second, sizeof(second), &output, sizeof(output), now).status,
    vdd::IoctlStatus::Success
  );
  harness.backend.fail_depart = true;

  vdd::LeaseRequest release {};
  release.lease_id = first.lease_id;
  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlReleaseLease,
    &release,
    sizeof(release),
    nullptr,
    0,
    now
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::BackendFailed);
  EXPECT_EQ(result.bytes_returned, 0u);
  EXPECT_EQ(harness.controller.store().temporary_display_count(), 2u);
  EXPECT_EQ(harness.controller.query_lease(first.lease_id, now).lease_exists, 1u);
  EXPECT_EQ(harness.backend.departed, (std::vector<std::uint64_t> {first.display_id, second.display_id}));

  // Both displays are pending departure, so feeding extends nothing, but it is
  // still accepted until their deadlines lapse.
  vdd::LeaseRequest feed {};
  feed.lease_id = first.lease_id;
  feed.requested_timeout_ms = 60'000;
  const auto feed_result = harness.dispatcher.dispatch(
    vdd::kIoctlFeedLease,
    &feed,
    sizeof(feed),
    nullptr,
    0,
    now
  );
  EXPECT_EQ(feed_result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(feed_result.bytes_returned, 0u);

  const auto expired_feed_result = harness.dispatcher.dispatch(
    vdd::kIoctlFeedLease,
    &feed,
    sizeof(feed),
    nullptr,
    0,
    now + std::chrono::seconds(31)
  );
  EXPECT_EQ(expired_feed_result.status, vdd::IoctlStatus::NotFound);
  EXPECT_EQ(expired_feed_result.bytes_returned, 0u);
}

TEST(VirtualDisplayDriverIoctlDispatcher, SetAndQueryPermanentDisplayCountUsePermanentApi) {
  Harness harness;
  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 2;
  request.width = 2560;
  request.height = 1440;
  request.physical_width_mm = 590;
  request.physical_height_mm = 330;
  request.refresh_rate_millihz = 120'000;
  set_display_name(request.display_name, "CLI Display");
  vdd::PermanentDisplayCountResult output {};

  const auto set_result = harness.dispatcher.dispatch(
    vdd::kIoctlSetPermanentDisplayCount,
    &request,
    sizeof(request),
    &output,
    sizeof(output),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(set_result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(set_result.bytes_returned, sizeof(output));
  EXPECT_EQ(output.current_display_count, 2u);
  EXPECT_EQ(output.width, 2560u);
  EXPECT_EQ(output.height, 1440u);
  EXPECT_EQ(output.physical_width_mm, 590u);
  EXPECT_EQ(output.physical_height_mm, 330u);
  EXPECT_EQ(output.refresh_rate_millihz, 120'000u);
  EXPECT_EQ(harness.backend.permanent_counts, (std::vector<std::uint32_t> {2}));
  ASSERT_EQ(harness.backend.permanent_settings.size(), 1u);
  EXPECT_EQ(harness.backend.permanent_settings[0].width, 2560u);
  EXPECT_EQ(harness.backend.permanent_settings[0].physical_width_mm, 590u);

  output = {};
  const auto query_result = harness.dispatcher.dispatch(
    vdd::kIoctlQueryPermanentDisplayCount,
    nullptr,
    0,
    &output,
    sizeof(output),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(query_result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(query_result.bytes_returned, sizeof(output));
  EXPECT_EQ(output.current_display_count, 2u);
  EXPECT_EQ(output.width, 2560u);
  EXPECT_EQ(output.physical_width_mm, 590u);
}

TEST(VirtualDisplayDriverIoctlDispatcher, SetPermanentDisplayCountMapsLimitValidation) {
  Harness harness;
  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 5;
  vdd::PermanentDisplayCountResult output {};

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlSetPermanentDisplayCount,
    &request,
    sizeof(request),
    &output,
    sizeof(output),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::LimitReached);
  EXPECT_EQ(result.bytes_returned, 0u);
  EXPECT_EQ(result.controller_status.validation_error, vdd::ValidationError::PermanentDisplayCountTooHigh);
  EXPECT_TRUE(harness.backend.permanent_counts.empty());
}

TEST(VirtualDisplayDriverIoctlDispatcher, QueryDisplayStateReturnsActiveIdentities) {
  Harness harness;
  vdd::PermanentDisplayCountRequest permanent {};
  permanent.display_count = 1;
  vdd::PermanentDisplayCountResult permanent_output {};
  ASSERT_EQ(
    harness.dispatcher.dispatch(
      vdd::kIoctlSetPermanentDisplayCount,
      &permanent,
      sizeof(permanent),
      &permanent_output,
      sizeof(permanent_output),
      std::chrono::steady_clock::now()
    ).status,
    vdd::IoctlStatus::Success
  );

  auto temporary = make_create_request();
  temporary.flags = vdd::kCreateTemporaryDisplayFlagEphemeralIdentity;
  vdd::CreateTemporaryDisplayResult created {};
  ASSERT_EQ(
    harness.dispatcher.dispatch(
      vdd::kIoctlCreateTemporaryDisplay,
      &temporary,
      sizeof(temporary),
      &created,
      sizeof(created),
      std::chrono::steady_clock::now()
    ).status,
    vdd::IoctlStatus::Success
  );

  vdd::QueryDisplayStateResult state {};
  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlQueryDisplayState,
    nullptr,
    0,
    &state,
    sizeof(state),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(result.bytes_returned, sizeof(state));
  EXPECT_EQ(state.api_namespace, vdd::kApiNamespaceGuid);
  EXPECT_EQ(state.permanent_display_count, 1u);
  EXPECT_EQ(state.temporary_display_count, 1u);
  ASSERT_EQ(state.entry_count, 2u);
  EXPECT_EQ(state.entries[0].kind, vdd::kDisplayStateKindPermanent);
  EXPECT_EQ(state.entries[1].kind, vdd::kDisplayStateKindTemporary);
  EXPECT_EQ(state.entries[1].flags & vdd::kDisplayStateFlagRetainIdentity, 0u);
}

TEST(VirtualDisplayDriverIoctlDispatcher, SetAndQueryDisplayManifestUseManifestApi) {
  Harness harness;
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
  profile.position_x = 2560;
  profile.position_y = 0;
  profile.allowed_modes[0] = {1920, 1080, 60'000};
  profile.allowed_modes[1] = {2560, 1440, 120'000};
  set_display_name(profile.display_name, "Side Display");
  vdd::DisplayManifest output {};

  const auto set_result = harness.dispatcher.dispatch(
    vdd::kIoctlSetDisplayManifest,
    &manifest,
    sizeof(manifest),
    &output,
    sizeof(output),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(set_result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(set_result.bytes_returned, sizeof(output));
  ASSERT_EQ(output.profile_count, 1u);
  EXPECT_EQ(output.profiles[0].connector_index, 2u);
  EXPECT_EQ(output.profiles[0].layout_policy, vdd::kDisplayManifestLayoutPolicyApplyAndPersist);
  EXPECT_EQ(output.profiles[0].position_x, 2560);
  EXPECT_EQ(output.profiles[0].allowed_modes[1].width, 2560u);
  EXPECT_EQ(harness.backend.permanent_counts, (std::vector<std::uint32_t> {1}));
  ASSERT_EQ(harness.backend.manifests.size(), 1u);
  EXPECT_EQ(harness.backend.manifests[0].profiles[0].connector_index, 2u);

  output = {};
  const auto query_result = harness.dispatcher.dispatch(
    vdd::kIoctlQueryDisplayManifest,
    nullptr,
    0,
    &output,
    sizeof(output),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(query_result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(query_result.bytes_returned, sizeof(output));
  EXPECT_EQ(output.api_namespace, vdd::kApiNamespaceGuid);
  EXPECT_EQ(output.profiles[0].display_id, 0x7000000000000100ull);
  EXPECT_EQ(vdd::trim_display_name(output.profiles[0].display_name), "Side Display");
}

TEST(VirtualDisplayDriverIoctlDispatcher, SetPermanentDisplayCountRejectsShortOutputBeforeBackendMutation) {
  Harness harness;
  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 2;

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlSetPermanentDisplayCount,
    &request,
    sizeof(request),
    nullptr,
    0,
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::InvalidOutputBuffer);
  EXPECT_EQ(result.bytes_returned, 0u);
  EXPECT_TRUE(harness.backend.permanent_counts.empty());
  EXPECT_EQ(harness.controller.query_permanent_display_count().current_display_count, 0u);
}

TEST(VirtualDisplayDriverIoctlDispatcher, RejectsUnknownIoctl) {
  Harness harness;

  const auto result = harness.dispatcher.dispatch(
    0xdeadbeefu,
    nullptr,
    0,
    nullptr,
    0,
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::InvalidIoctl);
  EXPECT_EQ(result.bytes_returned, 0u);
}
