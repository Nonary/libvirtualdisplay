#include <gtest/gtest.h>

#include "virtual_display/driver/control_client.h"
#include "virtual_display/driver/ioctl_dispatcher.h"

#include <cstring>
#include <optional>
#include <vector>

namespace vdd = virtual_display::driver;

namespace {
  class Backend final: public vdd::DisplayDriverBackend {
  public:
    vdd::BackendDisplayResult arrive_temporary_display(const vdd::DisplayDescriptor &) override { return {}; }
    vdd::BackendError depart_temporary_display(std::uint64_t, std::uint64_t) override { return vdd::BackendError::None; }
    vdd::BackendError set_permanent_display_count(const vdd::PermanentDisplayCountRequest &) override { return vdd::BackendError::None; }
    vdd::BackendError set_render_adapter(const vdd::SetRenderAdapterRequest &) override { return vdd::BackendError::None; }
  };

  class Transport final: public vdd::ControlTransport {
  public:
    bool ioctl(std::uint32_t code, const void *input, std::size_t input_size,
               void *output, std::size_t output_size,
               std::size_t &bytes_returned, std::uint32_t &native_error) override {
      calls.push_back(code);
      native_error = 0;
      if (code == vdd::kIoctlGetProtocolVersion && version) {
        std::memcpy(output, &*version, sizeof(*version));
        bytes_returned = sizeof(*version);
        return true;
      }
      if (code == vdd::kIoctlQueryFrameTransportCapabilities && capabilities) {
        std::memcpy(output, &*capabilities, sizeof(*capabilities));
        bytes_returned = sizeof(*capabilities);
        return true;
      }
      if (code == vdd::kIoctlQueryFrameTransportTarget && target) {
        std::memcpy(output, &*target, sizeof(*target));
        bytes_returned = sizeof(*target);
        return true;
      }
      const auto result = dispatcher.dispatch(code, input, input_size, output, output_size,
                                             std::chrono::steady_clock::now());
      bytes_returned = result.bytes_returned;
      return result.status == vdd::IoctlStatus::Success;
    }

    Backend backend;
    vdd::DriverController controller {vdd::DisplayStore {2, 2}, backend};
    vdd::IoctlDispatcher dispatcher {controller};
    std::optional<vdd::ProtocolVersion> version {};
    std::optional<vdd::FrameTransportCapabilities> capabilities {};
    std::optional<vdd::FrameTransportTarget> target {};
    std::vector<std::uint32_t> calls {};
  };
}

TEST(FrameTransportCapabilities, DispatcherCapabilityTracksTheExplicitBuildOption) {
  Transport transport;
  vdd::ControlClient client {transport};
  const auto capabilities = client.query_frame_transport_capabilities();
  ASSERT_TRUE(capabilities.ok());
#if defined(VDD_ENABLE_FRAME_TRANSPORT) && defined(_WIN32)
  EXPECT_EQ(capabilities.value.capabilities, vdd::kFrameTransportCapabilityKeyedMutexRing);
  EXPECT_EQ(capabilities.value.max_ring_slots, vdd::kFrameRingSlotCount);
#else
  EXPECT_EQ(capabilities.value.capabilities, 0u);
  EXPECT_EQ(capabilities.value.max_ring_slots, 0u);
#endif
  EXPECT_EQ(transport.calls, (std::vector<std::uint32_t> {
    vdd::kIoctlGetProtocolVersion, vdd::kIoctlQueryFrameTransportCapabilities
  }));
  // Existing display control remains available after capability discovery.
  EXPECT_TRUE(client.query_display_state().ok());
}

TEST(FrameTransportCapabilities, CompatibleLegacyDriverNeedsNoNewIoctl) {
  for (const auto minor : {6, 7}) {
    Transport transport;
    transport.version = vdd::ProtocolVersion {};
    transport.version->minor = minor;
    vdd::ControlClient client {transport};
    const auto capabilities = client.query_frame_transport_capabilities();
    ASSERT_TRUE(capabilities.ok());
    EXPECT_EQ(capabilities.value.capabilities, 0u);
    EXPECT_EQ(transport.calls, (std::vector<std::uint32_t> {vdd::kIoctlGetProtocolVersion}));
  }
}

TEST(FrameTransportCapabilities, IncompatibleDriverCannotBeTreatedAsAnEmptyCompatibleTransport) {
  Transport transport;
  transport.version = vdd::ProtocolVersion {};
  transport.version->major = 4;
  vdd::ControlClient client {transport};
  EXPECT_EQ(client.query_frame_transport_capabilities().status, vdd::ControlStatus::ProtocolIncompatible);
  EXPECT_EQ(transport.calls.size(), 1u);
}

TEST(FrameTransportCapabilities, RejectsMalformedAndUnusableCapabilityReplies) {
  for (int mutation = 0; mutation < 5; ++mutation) {
    Transport transport;
    transport.capabilities = vdd::FrameTransportCapabilities {};
    switch (mutation) {
      case 0: ++transport.capabilities->version; break;
      case 1: --transport.capabilities->size; break;
      case 2: transport.capabilities->reserved = 1; break;
      case 3: transport.capabilities->capabilities = vdd::kFrameTransportCapabilityKeyedMutexRing; break;
      case 4: transport.capabilities->api_namespace = {}; break;
    }
    vdd::ControlClient client {transport};
    EXPECT_EQ(client.query_frame_transport_capabilities().status,
              mutation == 4 ? vdd::ControlStatus::ProtocolIncompatible : vdd::ControlStatus::InvalidOutput);
  }
}

TEST(FrameTransportCapabilities, UnknownCapabilityDoesNotImplyAKeyedMutexTransport) {
  Transport transport;
  transport.capabilities = vdd::FrameTransportCapabilities {};
  transport.capabilities->capabilities = 1ull << 63;
  vdd::ControlClient client {transport};
  const auto capabilities = client.query_frame_transport_capabilities();
  ASSERT_TRUE(capabilities.ok());
  EXPECT_EQ(capabilities.value.capabilities & vdd::kFrameTransportCapabilityKeyedMutexRing, 0u);
}

TEST(FrameTransportCapabilities, DispatcherRejectsInputOrTruncatedOutput) {
  Transport transport;
  vdd::FrameTransportCapabilities output {};
  const auto now = std::chrono::steady_clock::now();
  auto result = transport.dispatcher.dispatch(vdd::kIoctlQueryFrameTransportCapabilities,
    nullptr, 0, &output, sizeof(output) - 1, now);
  EXPECT_EQ(result.status, vdd::IoctlStatus::InvalidOutputBuffer);
  EXPECT_EQ(result.bytes_returned, 0u);
  result = transport.dispatcher.dispatch(vdd::kIoctlQueryFrameTransportCapabilities,
    &output, sizeof(output), &output, sizeof(output), now);
  EXPECT_EQ(result.status, vdd::IoctlStatus::InvalidInputBuffer);
  EXPECT_EQ(result.bytes_returned, 0u);
}

TEST(FrameTransportTarget, ClientReturnsOnlyAnOwnerBoundLiveModeToken) {
  Transport transport;
  transport.target = vdd::FrameTransportTarget {};
  transport.target->lease_id = 10;
  transport.target->display_id = 20;
  transport.target->display_generation = 30;
  transport.target->width = 2560;
  transport.target->height = 1440;
  transport.target->adapter_luid = {1, 2};
  transport.target->format = 87;
  transport.target->color_space = 0;
  vdd::FrameTransportTargetRequest request {};
  request.lease_id = 10;
  request.display_id = 20;
  request.owner_capability.bytes.fill(1);

  vdd::ControlClient client {transport};
  const auto target = client.query_frame_transport_target(request);
  ASSERT_TRUE(target.ok());
  EXPECT_EQ(target.value.display_generation, 30u);
  EXPECT_EQ(target.value.adapter_luid, (vdd::AdapterLuid {1, 2}));
  EXPECT_EQ(target.value.width, 2560u);
  EXPECT_EQ(transport.calls, (std::vector<std::uint32_t> {
    vdd::kIoctlGetProtocolVersion, vdd::kIoctlQueryFrameTransportTarget
  }));
}

TEST(FrameTransportTarget, ClientRejectsInvalidRequestAndMismatchedModeReply) {
  Transport transport;
  transport.target = vdd::FrameTransportTarget {};
  transport.target->lease_id = 10;
  transport.target->display_id = 20;
  transport.target->display_generation = 30;
  transport.target->width = 2560;
  transport.target->height = 1440;
  transport.target->adapter_luid = {1, 2};
  transport.target->format = 10;
  transport.target->color_space = 0;
  vdd::FrameTransportTargetRequest request {};
  request.lease_id = 10;
  request.display_id = 20;
  request.owner_capability.bytes.fill(1);

  vdd::ControlClient client {transport};
  EXPECT_EQ(client.query_frame_transport_target(request).status, vdd::ControlStatus::InvalidOutput);
  request.owner_capability.bytes.fill(0);
  EXPECT_EQ(client.query_frame_transport_target(request).status, vdd::ControlStatus::InvalidOutput);
  EXPECT_EQ(transport.calls.size(), 3u); // version + target reply; then version only for malformed request
}

TEST(FrameTransportTarget, LegacyProtocolDoesNotReceiveTheNewQuery) {
  Transport transport;
  transport.version = vdd::ProtocolVersion {};
  transport.version->minor = 7;
  vdd::FrameTransportTargetRequest request {};
  request.lease_id = 10;
  request.display_id = 20;
  request.owner_capability.bytes.fill(1);
  vdd::ControlClient client {transport};
  EXPECT_EQ(client.query_frame_transport_target(request).status, vdd::ControlStatus::ProtocolIncompatible);
  EXPECT_EQ(transport.calls, (std::vector<std::uint32_t> {vdd::kIoctlGetProtocolVersion}));
}
