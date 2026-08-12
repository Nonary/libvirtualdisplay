#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwchar>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

extern "C" {
  HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(
    IDXGIDevice *dxgi_device,
    IInspectable **graphics_device
  );
}

struct
#if WINRT_IMPL_HAS_DECLSPEC_UUID
  __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
#endif
  IDirect3DDxgiInterfaceAccess: IUnknown {
  virtual HRESULT __stdcall GetInterface(REFIID id, IUnknown **object) = 0;
};

#if !WINRT_IMPL_HAS_DECLSPEC_UUID
static constexpr GUID GUID__IDirect3DDxgiInterfaceAccess = {
  0xA9B3D012,
  0x3DF2,
  0x4EE3,
  {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}
};

template<>
constexpr auto __mingw_uuidof<IDirect3DDxgiInterfaceAccess>() -> GUID const & {
  return GUID__IDirect3DDxgiInterfaceAccess;
}
#endif

using GraphicsCaptureItem = winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using Direct3D11CaptureFramePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using GraphicsCaptureSession = winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using DirectXPixelFormat = winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using IDirect3DDevice = winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using SizeInt32 = winrt::Windows::Graphics::SizeInt32;

namespace {
  struct Options {
    std::filesystem::path output_prefix {L"hdr-session-proof"};
    std::uint32_t duration_seconds {30};
    enum class WgcMode {
      none,
      window,
      monitor,
      both,
    } wgc_mode {WgcMode::none};
  };

  struct OutputSelection {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC description {};
  };

  struct WgcCaptureResult {
    UINT width {};
    UINT height {};
    UINT row_pitch {};
    DXGI_FORMAT format {DXGI_FORMAT_UNKNOWN};
    std::uint64_t fnv1a64 {};
    std::array<float, 4> minimum {
      std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::infinity(),
    };
    std::array<float, 4> maximum {
      -std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity(),
    };
    std::array<std::array<float, 4>, 5> band_centers {};
    std::uint64_t non_finite_count {};
    bool any_rgb_greater_than_one {};
  };

  float half_to_float(const std::uint16_t value) {
    const int sign = (value >> 15) != 0 ? -1 : 1;
    const int exponent = (value >> 10) & 0x1f;
    const int mantissa = value & 0x3ff;
    if (exponent == 0) {
      return static_cast<float>(sign) * std::ldexp(static_cast<float>(mantissa) / 1024.0f, -14);
    }
    if (exponent == 0x1f) {
      if (mantissa == 0) {
        return sign < 0 ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
      }
      return std::numeric_limits<float>::quiet_NaN();
    }
    return static_cast<float>(sign) * std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, exponent - 15);
  }

  std::string wgc_mode_name(const Options::WgcMode mode) {
    switch (mode) {
    case Options::WgcMode::window:
      return "window";
    case Options::WgcMode::monitor:
      return "monitor";
    case Options::WgcMode::both:
      return "both";
    case Options::WgcMode::none:
      return "none";
    }
    return "unknown";
  }

  std::string narrow_ascii(const std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const wchar_t ch: value) {
      result.push_back(ch >= 0 && ch <= 0x7f ? static_cast<char>(ch) : '?');
    }
    return result;
  }

  std::string hex_hresult(const HRESULT result) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << static_cast<std::uint32_t>(result);
    return stream.str();
  }

  bool parse_u32(const wchar_t *text, std::uint32_t &value) {
    if (!text || *text == L'\0') {
      return false;
    }
    wchar_t *end = nullptr;
    const unsigned long parsed = std::wcstoul(text, &end, 10);
    if (!end || *end != L'\0' || parsed > (std::numeric_limits<std::uint32_t>::max)()) {
      return false;
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
  }

  bool parse_options(const int argc, wchar_t **argv, Options &options) {
    for (int index = 1; index < argc; ++index) {
      const std::wstring_view argument {argv[index]};
      if (argument == L"--output-prefix" && index + 1 < argc) {
        options.output_prefix = argv[++index];
      } else if (argument == L"--duration-seconds" && index + 1 < argc) {
        if (!parse_u32(argv[++index], options.duration_seconds) || options.duration_seconds == 0) {
          return false;
        }
      } else if (argument == L"--wgc-mode" && index + 1 < argc) {
        const std::wstring_view mode {argv[++index]};
        if (mode == L"window") {
          options.wgc_mode = Options::WgcMode::window;
        } else if (mode == L"monitor") {
          options.wgc_mode = Options::WgcMode::monitor;
        } else if (mode == L"both") {
          options.wgc_mode = Options::WgcMode::both;
        } else {
          return false;
        }
      } else {
        return false;
      }
    }
    return true;
  }

  OutputSelection find_desktop_output(IDXGIFactory1 *factory) {
    OutputSelection fallback;
    for (UINT adapter_index = 0;; ++adapter_index) {
      ComPtr<IDXGIAdapter1> adapter;
      if (factory->EnumAdapters1(adapter_index, &adapter) == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      for (UINT output_index = 0;; ++output_index) {
        ComPtr<IDXGIOutput> output;
        if (adapter->EnumOutputs(output_index, &output) == DXGI_ERROR_NOT_FOUND) {
          break;
        }
        DXGI_OUTPUT_DESC description {};
        if (FAILED(output->GetDesc(&description))) {
          continue;
        }
        if (!fallback.output) {
          fallback = {adapter, output, description};
        }
        if (description.AttachedToDesktop) {
          return {adapter, output, description};
        }
      }
    }
    return fallback;
  }

  LRESULT CALLBACK window_proc(HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam) {
    if (message == WM_CLOSE) {
      DestroyWindow(window);
      return 0;
    }
    if (message == WM_DESTROY) {
      PostQuitMessage(0);
      return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
  }

  bool write_source_capture(
    ID3D11Device *device,
    ID3D11DeviceContext *context,
    ID3D11Texture2D *source,
    const std::filesystem::path &path,
    std::ostream &log
  ) {
    D3D11_TEXTURE2D_DESC description {};
    source->GetDesc(&description);
    D3D11_TEXTURE2D_DESC staging_description = description;
    staging_description.Usage = D3D11_USAGE_STAGING;
    staging_description.BindFlags = 0;
    staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_description.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT result = device->CreateTexture2D(&staging_description, nullptr, &staging);
    if (FAILED(result)) {
      log << "source_capture_create_staging_result=" << hex_hresult(result) << '\n';
      return false;
    }

    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
      log << "source_capture_map_result=" << hex_hresult(result) << '\n';
      return false;
    }

    const std::uint32_t packed_row_bytes = description.Width * sizeof(std::uint32_t);
    std::ofstream capture {path, std::ios::binary | std::ios::trunc};
    std::uint64_t hash = 1469598103934665603ull;
    for (UINT y = 0; y < description.Height; ++y) {
      const auto *row = static_cast<const std::uint8_t *>(mapped.pData) + y * mapped.RowPitch;
      capture.write(reinterpret_cast<const char *>(row), packed_row_bytes);
      for (std::uint32_t index = 0; index < packed_row_bytes; ++index) {
        hash ^= row[index];
        hash *= 1099511628211ull;
      }
    }
    const auto *top_row = static_cast<const std::uint32_t *>(mapped.pData);
    const auto *middle_row = reinterpret_cast<const std::uint32_t *>(
      static_cast<const std::uint8_t *>(mapped.pData) + (description.Height / 2) * mapped.RowPitch
    );
    const std::uint32_t first_pixel = top_row[0];
    const std::uint32_t middle_pixel = middle_row[description.Width / 2];
    const std::uint32_t last_pixel = middle_row[description.Width - 1];
    context->Unmap(staging.Get(), 0);

    log << "source_capture_path=" << narrow_ascii(path.wstring()) << '\n'
        << "source_capture_width=" << description.Width << '\n'
        << "source_capture_height=" << description.Height << '\n'
        << "source_capture_format=" << static_cast<std::uint32_t>(description.Format) << '\n'
        << "source_capture_packed_row_bytes=" << packed_row_bytes << '\n'
        << "source_capture_fnv1a64=0x" << std::hex << std::uppercase << hash << std::dec << '\n'
        << "source_capture_first_pixel=0x" << std::hex << std::uppercase << first_pixel << std::dec << '\n'
        << "source_capture_middle_pixel=0x" << std::hex << std::uppercase << middle_pixel << std::dec << '\n'
        << "source_capture_last_pixel=0x" << std::hex << std::uppercase << last_pixel << std::dec << '\n';
    return capture.good();
  }

  bool create_graphics_capture_item(
    HWND window,
    HMONITOR monitor,
    GraphicsCaptureItem &item,
    std::ostream &log,
    const std::string_view label
  ) {
    try {
      auto activation_factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
      const HRESULT result = window ? activation_factory->CreateForWindow(
        window,
        winrt::guid_of<GraphicsCaptureItem>(),
        winrt::put_abi(item)
      ) : activation_factory->CreateForMonitor(
        monitor,
        winrt::guid_of<GraphicsCaptureItem>(),
        winrt::put_abi(item)
      );
      log << "wgc_" << label << "_item_result=" << hex_hresult(result) << '\n';
      return SUCCEEDED(result) && item;
    } catch (const winrt::hresult_error &error) {
      log << "wgc_" << label << "_item_exception=" << hex_hresult(error.code().value) << '\n';
      return false;
    }
  }

  class WgcCapture {
  public:
    WgcCapture(
      std::string label,
      GraphicsCaptureItem item,
      IDirect3DDevice winrt_device,
      ID3D11Device *device,
      ID3D11DeviceContext *context,
      std::mutex &context_mutex,
      std::filesystem::path output_path,
      std::ostream &log
    ):
      label_(std::move(label)),
      item_(std::move(item)),
      winrt_device_(std::move(winrt_device)),
      device_(device),
      context_(context),
      context_mutex_(context_mutex),
      output_path_(std::move(output_path)),
      log_(log) {}

    WgcCapture(const WgcCapture &) = delete;
    WgcCapture &operator=(const WgcCapture &) = delete;

    ~WgcCapture() {
      stop();
    }

    bool start() {
      try {
        const auto item_size = item_.Size();
        if (item_size.Width <= 0 || item_size.Height <= 0) {
          set_failure("invalid_item_size");
          return false;
        }
        width_ = static_cast<UINT>(item_size.Width);
        height_ = static_cast<UINT>(item_size.Height);
        frame_pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
          winrt_device_,
          DirectXPixelFormat::R16G16B16A16Float,
          2,
          SizeInt32 {item_size.Width, item_size.Height}
        );
        if (!frame_pool_) {
          set_failure("frame_pool_null");
          return false;
        }
        frame_arrived_token_ = frame_pool_.FrameArrived([this](
          Direct3D11CaptureFramePool const &sender,
          winrt::Windows::Foundation::IInspectable const &
        ) {
          process_frame(sender);
        });
        closed_token_ = item_.Closed([this](
          GraphicsCaptureItem const &,
          winrt::Windows::Foundation::IInspectable const &
        ) {
          set_failure("capture_item_closed");
        });
        capture_session_ = frame_pool_.CreateCaptureSession(item_);
        capture_session_.StartCapture();
        log_ << "wgc_" << label_ << "_started=1\n"
             << "wgc_" << label_ << "_item_width=" << width_ << '\n'
             << "wgc_" << label_ << "_item_height=" << height_ << '\n'
             << "wgc_" << label_ << "_requested_format="
             << static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT) << '\n';
        return true;
      } catch (const winrt::hresult_error &error) {
        set_failure(std::string("start_exception_") + hex_hresult(error.code().value));
        return false;
      }
    }

    bool complete() const {
      std::lock_guard lock(state_mutex_);
      return complete_;
    }

    bool failed() const {
      std::lock_guard lock(state_mutex_);
      return failed_;
    }

    void timeout() {
      set_failure("timeout");
    }

    bool succeeded() const {
      std::lock_guard lock(state_mutex_);
      return complete_ && !failed_;
    }

  private:
    bool enter_callback() {
      std::lock_guard lock(state_mutex_);
      if (stopping_ || complete_ || failed_) {
        return false;
      }
      ++active_callbacks_;
      return true;
    }

    void leave_callback() {
      std::lock_guard lock(state_mutex_);
      --active_callbacks_;
      state_cv_.notify_all();
    }

    void set_failure(const std::string_view reason) {
      std::lock_guard lock(state_mutex_);
      if (!complete_ && !failed_) {
        failed_ = true;
        failure_reason_ = reason;
        log_ << "wgc_" << label_ << "_failure=" << failure_reason_ << '\n';
      }
    }

    void process_frame(Direct3D11CaptureFramePool const &sender) {
      if (!enter_callback()) {
        return;
      }
      try {
        auto frame = sender.TryGetNextFrame();
        while (auto next = sender.TryGetNextFrame()) {
          frame = std::move(next);
        }
        if (!frame) {
          leave_callback();
          return;
        }

        auto surface = frame.Surface();
        ComPtr<IDirect3DDxgiInterfaceAccess> surface_access;
        HRESULT result = winrt::get_unknown(surface)->QueryInterface(
          __uuidof(IDirect3DDxgiInterfaceAccess),
          reinterpret_cast<void **>(surface_access.GetAddressOf())
        );
        if (FAILED(result)) {
          set_failure(std::string("surface_access_") + hex_hresult(result));
          leave_callback();
          return;
        }

        ComPtr<ID3D11Texture2D> source;
        result = surface_access->GetInterface(
          __uuidof(ID3D11Texture2D),
          reinterpret_cast<IUnknown **>(source.GetAddressOf())
        );
        if (FAILED(result) || !source) {
          set_failure(std::string("surface_texture_") + hex_hresult(result));
          leave_callback();
          return;
        }

        D3D11_TEXTURE2D_DESC source_description {};
        source->GetDesc(&source_description);
        if (source_description.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
          set_failure("unexpected_format");
          leave_callback();
          return;
        }
        D3D11_TEXTURE2D_DESC staging_description = source_description;
        staging_description.Usage = D3D11_USAGE_STAGING;
        staging_description.BindFlags = 0;
        staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_description.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        result = device_->CreateTexture2D(&staging_description, nullptr, &staging);
        if (FAILED(result)) {
          set_failure(std::string("create_staging_") + hex_hresult(result));
          leave_callback();
          return;
        }

        const std::uint32_t packed_row_bytes = source_description.Width * 4u * sizeof(std::uint16_t);
        std::vector<std::uint8_t> packed(
          static_cast<std::size_t>(packed_row_bytes) * source_description.Height
        );
        D3D11_MAPPED_SUBRESOURCE mapped {};
        {
          std::lock_guard context_lock(context_mutex_);
          context_->CopyResource(staging.Get(), source.Get());
          result = context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
          if (FAILED(result)) {
            set_failure(std::string("map_staging_") + hex_hresult(result));
            leave_callback();
            return;
          }
          for (UINT y = 0; y < source_description.Height; ++y) {
            const auto *row = static_cast<const std::uint8_t *>(mapped.pData) + y * mapped.RowPitch;
            std::copy_n(row, packed_row_bytes, packed.data() + static_cast<std::size_t>(y) * packed_row_bytes);
          }
          context_->Unmap(staging.Get(), 0);
        }

        WgcCaptureResult capture_result;
        capture_result.width = source_description.Width;
        capture_result.height = source_description.Height;
        capture_result.row_pitch = mapped.RowPitch;
        capture_result.format = source_description.Format;
        capture_result.fnv1a64 = 1469598103934665603ull;
        for (std::size_t index = 0; index < packed.size(); ++index) {
          capture_result.fnv1a64 ^= packed[index];
          capture_result.fnv1a64 *= 1099511628211ull;
        }
        const auto *pixels = reinterpret_cast<const std::uint16_t *>(packed.data());
        for (UINT y = 0; y < capture_result.height; ++y) {
          for (UINT x = 0; x < capture_result.width; ++x) {
            const auto *pixel = pixels + (static_cast<std::size_t>(y) * capture_result.width + x) * 4;
            for (std::size_t channel = 0; channel < 4; ++channel) {
              const float value = half_to_float(pixel[channel]);
              if (!std::isfinite(value)) {
                ++capture_result.non_finite_count;
              } else {
                capture_result.minimum[channel] = (std::min)(capture_result.minimum[channel], value);
                capture_result.maximum[channel] = (std::max)(capture_result.maximum[channel], value);
              }
              if (channel < 3 && std::isfinite(value) && value > 1.0f) {
                capture_result.any_rgb_greater_than_one = true;
              }
            }
          }
        }
        for (std::size_t band = 0; band < capture_result.band_centers.size(); ++band) {
          const UINT x = static_cast<UINT>((static_cast<std::uint64_t>(2 * band + 1) * capture_result.width) / 10);
          const UINT y = capture_result.height / 2;
          const auto *pixel = pixels + (static_cast<std::size_t>(y) * capture_result.width + x) * 4;
          for (std::size_t channel = 0; channel < 4; ++channel) {
            capture_result.band_centers[band][channel] = half_to_float(pixel[channel]);
          }
        }

        std::ofstream capture {output_path_, std::ios::binary | std::ios::trunc};
        capture.write(reinterpret_cast<const char *>(packed.data()), static_cast<std::streamsize>(packed.size()));
        if (!capture.good()) {
          set_failure("write_raw");
          leave_callback();
          return;
        }
        {
          std::lock_guard lock(state_mutex_);
          result_ = capture_result;
          complete_ = true;
        }
        log_ << "wgc_" << label_ << "_path=" << narrow_ascii(output_path_.wstring()) << '\n'
             << "wgc_" << label_ << "_width=" << capture_result.width << '\n'
             << "wgc_" << label_ << "_height=" << capture_result.height << '\n'
             << "wgc_" << label_ << "_format=" << static_cast<std::uint32_t>(capture_result.format) << '\n'
             << "wgc_" << label_ << "_row_pitch=" << capture_result.row_pitch << '\n'
             << "wgc_" << label_ << "_packed_row_bytes=" << packed_row_bytes << '\n'
             << "wgc_" << label_ << "_fnv1a64=0x" << std::hex << std::uppercase << capture_result.fnv1a64 << std::dec << '\n'
             << "wgc_" << label_ << "_non_finite_count=" << capture_result.non_finite_count << '\n'
             << "wgc_" << label_ << "_any_rgb_greater_than_one=" << (capture_result.any_rgb_greater_than_one ? 1 : 0) << '\n';
        for (std::size_t channel = 0; channel < 4; ++channel) {
          log_ << "wgc_" << label_ << "_min_" << channel << '=' << capture_result.minimum[channel] << '\n'
               << "wgc_" << label_ << "_max_" << channel << '=' << capture_result.maximum[channel] << '\n';
        }
        for (std::size_t band = 0; band < capture_result.band_centers.size(); ++band) {
          log_ << "wgc_" << label_ << "_band_" << band << '='
               << capture_result.band_centers[band][0] << ','
               << capture_result.band_centers[band][1] << ','
               << capture_result.band_centers[band][2] << ','
               << capture_result.band_centers[band][3] << '\n';
        }
      } catch (const winrt::hresult_error &error) {
        set_failure(std::string("frame_exception_") + hex_hresult(error.code().value));
      } catch (...) {
        set_failure("frame_exception_unknown");
      }
      leave_callback();
    }

    void stop() noexcept {
      {
        std::lock_guard lock(state_mutex_);
        if (stopping_) {
          return;
        }
        stopping_ = true;
      }
      try {
        if (frame_pool_ && frame_arrived_token_.value != 0) {
          frame_pool_.FrameArrived(frame_arrived_token_);
          frame_arrived_token_.value = 0;
        }
        if (item_ && closed_token_.value != 0) {
          item_.Closed(closed_token_);
          closed_token_.value = 0;
        }
        if (capture_session_) {
          capture_session_.Close();
          capture_session_ = nullptr;
        }
        if (frame_pool_) {
          frame_pool_.Close();
          frame_pool_ = nullptr;
        }
      } catch (...) {
        // Cleanup is best effort; the bounded owner lifetime still releases all WinRT references.
      }
      std::unique_lock lock(state_mutex_);
      state_cv_.wait(lock, [this] { return active_callbacks_ == 0; });
    }

    std::string label_;
    GraphicsCaptureItem item_ {nullptr};
    IDirect3DDevice winrt_device_ {nullptr};
    ID3D11Device *device_ {};
    ID3D11DeviceContext *context_ {};
    std::mutex &context_mutex_;
    std::filesystem::path output_path_;
    std::ostream &log_;
    UINT width_ {};
    UINT height_ {};
    Direct3D11CaptureFramePool frame_pool_ {nullptr};
    GraphicsCaptureSession capture_session_ {nullptr};
    winrt::event_token frame_arrived_token_ {};
    winrt::event_token closed_token_ {};
    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;
    WgcCaptureResult result_ {};
    std::string failure_reason_;
    std::size_t active_callbacks_ {};
    bool stopping_ {};
    bool complete_ {};
    bool failed_ {};
  };

  bool write_composed_bmp(const RECT &rectangle, const std::filesystem::path &path, std::ostream &log) {
    const LONG width = rectangle.right - rectangle.left;
    const LONG height = rectangle.bottom - rectangle.top;
    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO bitmap_info {};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    void *pixels = nullptr;
    HBITMAP bitmap = memory ? CreateDIBSection(screen, &bitmap_info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
    HGDIOBJ previous = bitmap && memory ? SelectObject(memory, bitmap) : nullptr;
    const BOOL copied = previous ? BitBlt(
      memory,
      0,
      0,
      width,
      height,
      screen,
      rectangle.left,
      rectangle.top,
      SRCCOPY | CAPTUREBLT
    ) : FALSE;

    bool written = false;
    if (copied && pixels) {
      const std::uint32_t pixel_bytes = static_cast<std::uint32_t>(width * height * 4);
      BITMAPFILEHEADER file_header {};
      file_header.bfType = 0x4d42;
      file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
      file_header.bfSize = file_header.bfOffBits + pixel_bytes;
      std::ofstream capture {path, std::ios::binary | std::ios::trunc};
      capture.write(reinterpret_cast<const char *>(&file_header), sizeof(file_header));
      capture.write(reinterpret_cast<const char *>(&bitmap_info.bmiHeader), sizeof(bitmap_info.bmiHeader));
      capture.write(static_cast<const char *>(pixels), pixel_bytes);
      written = capture.good();
    }

    if (previous) {
      SelectObject(memory, previous);
    }
    if (bitmap) {
      DeleteObject(bitmap);
    }
    if (memory) {
      DeleteDC(memory);
    }
    if (screen) {
      ReleaseDC(nullptr, screen);
    }

    log << "composed_capture_path=" << narrow_ascii(path.wstring()) << '\n'
        << "composed_capture_bitblt=" << (copied ? 1 : 0) << '\n'
        << "composed_capture_written=" << (written ? 1 : 0) << '\n'
        << "composed_capture_format=BGRA8_BMP\n";
    return written;
  }
}

int wmain(const int argc, wchar_t **argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    std::cerr << "usage: hdr_session_probe [--output-prefix <path>] [--duration-seconds <seconds>] [--wgc-mode window|monitor|both]\n";
    return 2;
  }

  std::error_code filesystem_error;
  if (!options.output_prefix.parent_path().empty()) {
    std::filesystem::create_directories(options.output_prefix.parent_path(), filesystem_error);
  }
  const auto log_path = options.output_prefix.wstring() + L".log";
  const auto source_path = options.output_prefix.wstring() + L"-source-r10g10b10a2.raw";
  const auto composed_path = options.output_prefix.wstring() + L"-composed-bgra8.bmp";
  std::ofstream log;
  log.open(log_path.c_str(), std::ios::trunc);
  if (!log) {
    std::cerr << "could not open proof log\n";
    return 1;
  }
  log.setf(std::ios::unitbuf);

  DWORD session_id = (std::numeric_limits<DWORD>::max)();
  ProcessIdToSessionId(GetCurrentProcessId(), &session_id);
  log << "process_id=" << GetCurrentProcessId() << '\n'
      << "session_id=" << session_id << '\n'
      << "duration_seconds=" << options.duration_seconds << '\n'
      << "wgc_mode=" << wgc_mode_name(options.wgc_mode) << '\n';

  ComPtr<IDXGIFactory2> factory;
  HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  log << "create_factory_result=" << hex_hresult(result) << '\n';
  if (FAILED(result)) {
    return 1;
  }

  const OutputSelection selected = find_desktop_output(factory.Get());
  if (!selected.output || !selected.adapter) {
    log << "desktop_output_found=0\n";
    return 1;
  }
  const RECT rectangle = selected.description.DesktopCoordinates;
  const UINT width = static_cast<UINT>(rectangle.right - rectangle.left);
  const UINT height = static_cast<UINT>(rectangle.bottom - rectangle.top);
  log << "desktop_output_found=1\n"
      << "output_device=" << narrow_ascii(selected.description.DeviceName) << '\n'
      << "output_attached=" << (selected.description.AttachedToDesktop ? 1 : 0) << '\n'
      << "output_rect=" << rectangle.left << ',' << rectangle.top << ',' << rectangle.right << ',' << rectangle.bottom << '\n';

  const wchar_t *window_class_name = L"LibvirtualdisplayHdrSessionProbe";
  WNDCLASSW window_class {};
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = window_class_name;
  RegisterClassW(&window_class);
  HWND window = CreateWindowExW(
    WS_EX_TOPMOST,
    window_class_name,
    L"HDR session transport proof",
    WS_POPUP | WS_VISIBLE,
    rectangle.left,
    rectangle.top,
    static_cast<int>(width),
    static_cast<int>(height),
    nullptr,
    nullptr,
    window_class.hInstance,
    nullptr
  );
  log << "window_created=" << (window ? 1 : 0) << " native_error=" << GetLastError() << '\n';
  if (!window) {
    return 1;
  }
  ShowWindow(window, SW_SHOW);
  SetWindowPos(window, HWND_TOPMOST, rectangle.left, rectangle.top, width, height, SWP_SHOWWINDOW);

  constexpr std::array feature_levels {
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
  };
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL feature_level {};
  result = D3D11CreateDevice(
    selected.adapter.Get(),
    D3D_DRIVER_TYPE_UNKNOWN,
    nullptr,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
    feature_levels.data(),
    static_cast<UINT>(feature_levels.size()),
    D3D11_SDK_VERSION,
    &device,
    &feature_level,
    &context
  );
  log << "create_device_result=" << hex_hresult(result) << '\n'
      << "feature_level=0x" << std::hex << static_cast<std::uint32_t>(feature_level) << std::dec << '\n';
  if (FAILED(result)) {
    DestroyWindow(window);
    return 1;
  }

  DXGI_SWAP_CHAIN_DESC1 swapchain_description {};
  swapchain_description.Width = width;
  swapchain_description.Height = height;
  swapchain_description.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  swapchain_description.SampleDesc.Count = 1;
  swapchain_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swapchain_description.BufferCount = 3;
  swapchain_description.Scaling = DXGI_SCALING_STRETCH;
  swapchain_description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  swapchain_description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

  ComPtr<IDXGISwapChain1> swapchain1;
  result = factory->CreateSwapChainForHwnd(
    device.Get(),
    window,
    &swapchain_description,
    nullptr,
    selected.output.Get(),
    &swapchain1
  );
  log << "create_swapchain_result=" << hex_hresult(result) << '\n'
      << "swapchain_format=" << static_cast<std::uint32_t>(swapchain_description.Format) << '\n';
  if (FAILED(result)) {
    DestroyWindow(window);
    return 1;
  }
  factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

  ComPtr<IDXGISwapChain3> swapchain3;
  result = swapchain1.As(&swapchain3);
  log << "query_swapchain3_result=" << hex_hresult(result) << '\n';
  UINT color_space_support = 0;
  const HRESULT color_check_result = swapchain3 ? swapchain3->CheckColorSpaceSupport(
    DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
    &color_space_support
  ) : E_NOINTERFACE;
  const HRESULT color_set_result = swapchain3 ? swapchain3->SetColorSpace1(
    DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
  ) : E_NOINTERFACE;
  log << "hdr10_color_space=" << static_cast<std::uint32_t>(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) << '\n'
      << "check_hdr10_color_space_result=" << hex_hresult(color_check_result) << '\n'
      << "hdr10_color_space_support=0x" << std::hex << color_space_support << std::dec << '\n'
      << "set_hdr10_color_space_result=" << hex_hresult(color_set_result) << '\n';

  DXGI_HDR_METADATA_HDR10 metadata {};
  metadata.RedPrimary[0] = 34000;
  metadata.RedPrimary[1] = 16000;
  metadata.GreenPrimary[0] = 13250;
  metadata.GreenPrimary[1] = 34500;
  metadata.BluePrimary[0] = 7500;
  metadata.BluePrimary[1] = 3000;
  metadata.WhitePoint[0] = 15635;
  metadata.WhitePoint[1] = 16450;
  metadata.MaxMasteringLuminance = 1000 * 10000;
  metadata.MinMasteringLuminance = 500;
  metadata.MaxContentLightLevel = 1000;
  metadata.MaxFrameAverageLightLevel = 400;
  ComPtr<IDXGISwapChain4> swapchain4;
  result = swapchain1.As(&swapchain4);
  const HRESULT metadata_result = swapchain4 ? swapchain4->SetHDRMetaData(
    DXGI_HDR_METADATA_TYPE_HDR10,
    sizeof(metadata),
    &metadata
  ) : E_NOINTERFACE;
  log << "query_swapchain4_result=" << hex_hresult(result) << '\n'
      << "set_hdr10_metadata_result=" << hex_hresult(metadata_result) << '\n'
      << "metadata_max_mastering_nits=1000\n"
      << "metadata_max_cll_nits=1000\n";

  ComPtr<ID3D11DeviceContext1> context1;
  result = context.As(&context1);
  log << "query_context1_result=" << hex_hresult(result) << '\n';
  if (FAILED(result)) {
    DestroyWindow(window);
    return 1;
  }

  std::mutex d3d_context_mutex;
  std::unique_ptr<WgcCapture> wgc_window;
  std::unique_ptr<WgcCapture> wgc_monitor;
  if (options.wgc_mode != Options::WgcMode::none) {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      ComPtr<IDXGIDevice> dxgi_device;
      result = device.As(&dxgi_device);
      winrt::com_ptr<::IDirect3DDevice> interop_device;
      if (SUCCEEDED(result)) {
        result = CreateDirect3D11DeviceFromDXGIDevice(
          dxgi_device.Get(),
          reinterpret_cast<::IInspectable **>(winrt::put_abi(interop_device))
        );
      }
      IDirect3DDevice winrt_device = interop_device.try_as<IDirect3DDevice>();
      log << "wgc_create_interop_device_result=" << hex_hresult(result) << '\n'
          << "wgc_interop_device_available=" << (winrt_device ? 1 : 0) << '\n';
      if (FAILED(result) || !winrt_device) {
      } else {
        const HMONITOR monitor = MonitorFromRect(&rectangle, MONITOR_DEFAULTTONEAREST);
        const bool want_window = options.wgc_mode == Options::WgcMode::window || options.wgc_mode == Options::WgcMode::both;
        const bool want_monitor = options.wgc_mode == Options::WgcMode::monitor || options.wgc_mode == Options::WgcMode::both;
        if (want_window) {
          GraphicsCaptureItem item {nullptr};
          if (create_graphics_capture_item(window, nullptr, item, log, "window")) {
            wgc_window = std::make_unique<WgcCapture>(
              "window",
              std::move(item),
              winrt_device,
              device.Get(),
              context.Get(),
              d3d_context_mutex,
              options.output_prefix.wstring() + L"-wgc-window-r16g16b16a16f.raw",
              log
            );
          } else {
          }
        }
        if (want_monitor) {
          GraphicsCaptureItem item {nullptr};
          if (create_graphics_capture_item(nullptr, monitor, item, log, "monitor")) {
            wgc_monitor = std::make_unique<WgcCapture>(
              "monitor",
              std::move(item),
              winrt_device,
              device.Get(),
              context.Get(),
              d3d_context_mutex,
              options.output_prefix.wstring() + L"-wgc-monitor-r16g16b16a16f.raw",
              log
            );
          } else {
          }
        }
      }
    } catch (const winrt::hresult_error &error) {
      log << "wgc_setup_exception=" << hex_hresult(error.code().value) << '\n';
    }
  }

  const std::array<std::array<float, 4>, 5> colors {{
    {0.0f, 0.0f, 0.0f, 1.0f},
    {0.508078f, 0.508078f, 0.508078f, 1.0f},
    {0.652579f, 0.0f, 0.0f, 1.0f},
    {0.0f, 0.751827f, 0.0f, 1.0f},
    {0.751827f, 0.751827f, 0.751827f, 1.0f},
  }};

  bool source_captured = false;
  bool composed_captured = false;
  bool wgc_started = false;
  auto wgc_start_time = std::chrono::steady_clock::now();
  std::uint64_t present_count = 0;
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + std::chrono::seconds(options.duration_seconds);
  while (std::chrono::steady_clock::now() < deadline && IsWindow(window)) {
    MSG message {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) {
        DestroyWindow(window);
        break;
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (!IsWindow(window)) {
      break;
    }

    ComPtr<ID3D11Texture2D> backbuffer;
    result = swapchain1->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    if (FAILED(result)) {
      log << "get_backbuffer_result=" << hex_hresult(result) << '\n';
      break;
    }
    ComPtr<ID3D11RenderTargetView> render_target;
    result = device->CreateRenderTargetView(backbuffer.Get(), nullptr, &render_target);
    if (FAILED(result)) {
      log << "create_render_target_result=" << hex_hresult(result) << '\n';
      break;
    }
    for (std::size_t index = 0; index < colors.size(); ++index) {
      const LONG left = static_cast<LONG>((static_cast<std::uint64_t>(width) * index) / colors.size());
      const LONG right = static_cast<LONG>((static_cast<std::uint64_t>(width) * (index + 1)) / colors.size());
      const D3D11_RECT band {left, 0, right, static_cast<LONG>(height)};
      context1->ClearView(render_target.Get(), colors[index].data(), &band, 1);
    }
    if (!source_captured) {
      source_captured = write_source_capture(device.Get(), context.Get(), backbuffer.Get(), source_path, log);
      log << "source_capture_written=" << (source_captured ? 1 : 0) << '\n';
    }

    result = swapchain1->Present(1, 0);
    if (FAILED(result)) {
      log << "present_result=" << hex_hresult(result) << '\n';
      break;
    }
    ++present_count;
    // Present does not reliably block to the compositor cadence in a terminal
    // session. Keep this diagnostic producer near 60 FPS instead of spinning.
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    if (!wgc_started && options.wgc_mode != Options::WgcMode::none && present_count >= 3) {
      const bool window_started = !wgc_window || wgc_window->start();
      const bool monitor_started = !wgc_monitor || wgc_monitor->start();
      wgc_started = true;
      wgc_start_time = std::chrono::steady_clock::now();
      log << "wgc_window_start_result=" << (window_started ? 1 : 0) << '\n'
          << "wgc_monitor_start_result=" << (monitor_started ? 1 : 0) << '\n';
    }
    if (wgc_started && options.wgc_mode != Options::WgcMode::none &&
        std::chrono::steady_clock::now() - wgc_start_time > std::chrono::seconds(8) &&
        ((!wgc_window || wgc_window->complete() || wgc_window->failed()) &&
         (!wgc_monitor || wgc_monitor->complete() || wgc_monitor->failed()))) {
      // Both requested items have either produced a frame or failed independently.
      break;
    }
    if (!composed_captured && std::chrono::steady_clock::now() - start >= std::chrono::seconds(2)) {
      composed_captured = write_composed_bmp(rectangle, composed_path, log);
    }
  }

  if (wgc_started) {
    if (wgc_window && !wgc_window->complete() && !wgc_window->failed()) {
      wgc_window->timeout();
    }
    if (wgc_monitor && !wgc_monitor->complete() && !wgc_monitor->failed()) {
      wgc_monitor->timeout();
    }
  }

  log << "present_count=" << present_count << '\n'
      << "proof_completed=1\n";
  if (IsWindow(window)) {
    DestroyWindow(window);
  }
  const bool wgc_completed = options.wgc_mode == Options::WgcMode::none || (
    (options.wgc_mode == Options::WgcMode::window && wgc_window && wgc_window->succeeded()) ||
    (options.wgc_mode == Options::WgcMode::monitor && wgc_monitor && wgc_monitor->succeeded()) ||
    (options.wgc_mode == Options::WgcMode::both && wgc_window && wgc_monitor &&
      wgc_window->succeeded() && wgc_monitor->succeeded())
  );
  log << "wgc_completed=" << (wgc_completed ? 1 : 0) << '\n';
  return source_captured && composed_captured && SUCCEEDED(color_set_result) && wgc_completed ? 0 : 1;
}
