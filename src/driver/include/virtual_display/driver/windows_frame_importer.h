#pragma once
#include "virtual_display/driver/frame_ring_server.h"
#ifdef _WIN32
#include <Windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace virtual_display::driver {
  // Native request adapter must supply WdfRequestGetRequestorProcessId(request),
  // not the device opener PID or a broker/client-supplied payload PID. The
  // capture helper pins that process instance and verifies creation time again
  // on import. Broker forwarding is deliberately unsupported for handle IOCTLs.
  std::optional<FrameRequestPeer> capture_frame_request_peer(std::uint32_t requestor_pid);

  class WindowsImportedFrameRing final: public FrameRingResources {
  public:
    ~WindowsImportedFrameRing() override;
    [[nodiscard]] HANDLE texture_handle(std::size_t slot) const {
      return slot < kFrameRingSlotCount ? handles_[slot] : nullptr;
    }
    [[nodiscard]] const FrameRingRegisterRequest &request() const { return request_; }
    [[nodiscard]] FrameRingMetadata *metadata_view() const { return metadata; }
    [[nodiscard]] HANDLE event_handle() const { return event; }
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, kFrameRingSlotCount> textures;
    std::array<Microsoft::WRL::ComPtr<IDXGIKeyedMutex>, kFrameRingSlotCount> mutexes;
    FrameRingMetadata *metadata {};
    HANDLE event {};
  private:
    friend class WindowsFrameImporter;
    FrameRingRegisterRequest request_ {};
    HANDLE source_process_ {};
    std::array<HANDLE, 6> handles_ {};
  };

  class WindowsFrameImporter final: public FrameResourceImporter {
  public:
    WindowsFrameImporter() = default;
    explicit WindowsFrameImporter(ID3D11Device1 *device);
    std::shared_ptr<FrameRingResources> import(const FrameRequestPeer &peer,
                                             const FrameRingRegisterRequest &request) override;
  private:
    Microsoft::WRL::ComPtr<ID3D11Device1> device_;
  };
}
#endif
