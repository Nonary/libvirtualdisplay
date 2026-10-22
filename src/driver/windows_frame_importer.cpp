#include "virtual_display/driver/windows_frame_importer.h"
#include "virtual_display/driver/windows_control_protocol.h"
#ifdef _WIN32
#include <dxgi1_4.h>

namespace virtual_display::driver {
  namespace {
    std::uint64_t file_time(const FILETIME &t) {
      return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    }
    std::uint64_t process_start(HANDLE process) {
      FILETIME created {}, exited {}, kernel {}, user {};
      return GetProcessTimes(process, &created, &exited, &kernel, &user) ? file_time(created) : 0;
    }
    bool device_matches(ID3D11Device1 *device, const AdapterLuid &luid) {
      Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
      Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
      DXGI_ADAPTER_DESC desc {};
      return SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
             SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)) &&
             desc.AdapterLuid.LowPart == luid.low_part && desc.AdapterLuid.HighPart == luid.high_part;
    }

    Microsoft::WRL::ComPtr<ID3D11Device1> create_hardware_device_for_luid(const AdapterLuid &luid) {
      Microsoft::WRL::ComPtr<ID3D11Device1> result;
      Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
      Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
      if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
          FAILED(factory->EnumAdapterByLuid(to_windows_luid(luid), IID_PPV_ARGS(&adapter)))) return {};
      static constexpr D3D_FEATURE_LEVEL levels[] {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
      };
      Microsoft::WRL::ComPtr<ID3D11Device> device;
      Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
      D3D_FEATURE_LEVEL selected {};
      if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                   &device, &selected, &context)) || FAILED(device.As(&result)) ||
          !device_matches(result.Get(), luid)) return {};
      return result;
    }
  }

  std::optional<FrameRequestPeer> capture_frame_request_peer(const std::uint32_t pid) {
    if (!pid) return {};
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, pid);
    if (!process) return {};
    const auto start = process_start(process);
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    if (!start || !alive) return {};
    return FrameRequestPeer {pid, start};
  }

  WindowsImportedFrameRing::~WindowsImportedFrameRing() {
    if (metadata) UnmapViewOfFile(metadata);
    for (const auto handle: handles_) if (handle) CloseHandle(handle);
    if (source_process_) CloseHandle(source_process_);
    // COM resources remain owned until the last native GPU-work snapshot drops.
    // No keyed mutex wait, GPU flush or consumer wait occurs in destruction.
  }

  WindowsFrameImporter::WindowsFrameImporter(ID3D11Device1 *device): device_(device) {}
  std::shared_ptr<FrameRingResources> WindowsFrameImporter::import(
    const FrameRequestPeer &peer, const FrameRingRegisterRequest &r) {
    if (!peer.valid() || peer.process_id > MAXDWORD || !valid_frame_ring_request(r)) return {};
    auto device = device_ ? device_ : create_hardware_device_for_luid(r.adapter_luid);
    if (!device || !device_matches(device.Get(), r.adapter_luid)) return {};
    auto ring = std::make_shared<WindowsImportedFrameRing>();
    ring->request_ = r;
    ring->source_process_ = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE | SYNCHRONIZE,
                                       FALSE, static_cast<DWORD>(peer.process_id));
    if (!ring->source_process_ || process_start(ring->source_process_) != peer.process_start ||
        WaitForSingleObject(ring->source_process_, 0) != WAIT_TIMEOUT) return {};
    std::array<std::uint64_t, 6> values {};
    std::copy(r.texture_handles.begin(), r.texture_handles.end(), values.begin());
    values[4] = r.metadata_handle; values[5] = r.event_handle;
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (values[i] > (std::numeric_limits<std::uintptr_t>::max)() ||
          !DuplicateHandle(ring->source_process_, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(values[i])),
                           GetCurrentProcess(), &ring->handles_[i], 0, FALSE, DUPLICATE_SAME_ACCESS)) return {};
    }
    for (std::size_t i = 0; i < ring->handles_.size(); ++i) {
      for (std::size_t j = 0; j < i; ++j) {
        if (CompareObjectHandles(ring->handles_[i], ring->handles_[j])) return {};
      }
    }
    for (std::size_t i = 0; i < kFrameRingSlotCount; ++i) {
      if (FAILED(device->OpenSharedResource1(ring->handles_[i], IID_PPV_ARGS(&ring->textures[i]))) ||
          FAILED(ring->textures[i].As(&ring->mutexes[i]))) return {};
      D3D11_TEXTURE2D_DESC desc {};
      ring->textures[i]->GetDesc(&desc);
      if (desc.Width != r.width || desc.Height != r.height || desc.Format != r.format ||
          desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
          desc.SampleDesc.Quality != 0 || desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags != 0 ||
          desc.BindFlags != (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) ||
          desc.MiscFlags != (D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) return {};
    }
    ring->metadata = static_cast<FrameRingMetadata *>(MapViewOfFile(ring->handles_[4], FILE_MAP_READ | FILE_MAP_WRITE,
                                                                  0, 0, kFrameRingMetadataBytes));
    if (!ring->metadata) return {};
    const auto &header = *ring->metadata;
    if (!is_valid_api_namespace(header.api_namespace) || header.version != kFrameRingWireVersion ||
        header.size != sizeof(header) || header.client_cookie != r.client_cookie ||
        std::any_of(header.reserved.begin(), header.reserved.end(), [](auto b) { return b != 0; })) return {};
    // SetEvent verifies the duplicated object is an event. Its extra wake is a
    // harmless hint: consumers always scan/validate mutex-protected slot data.
    if (!SetEvent(ring->handles_[5])) return {};
    ring->event = ring->handles_[5];
    return ring;
  }
}
#endif
