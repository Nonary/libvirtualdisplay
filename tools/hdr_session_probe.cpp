#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_6.h>
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
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
  struct Options {
    std::filesystem::path output_prefix {L"hdr-session-proof"};
    std::uint32_t duration_seconds {30};
  };

  struct OutputSelection {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC description {};
  };

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
    std::cerr << "usage: hdr_session_probe [--output-prefix <path>] [--duration-seconds <seconds>]\n";
    return 2;
  }

  std::error_code filesystem_error;
  if (!options.output_prefix.parent_path().empty()) {
    std::filesystem::create_directories(options.output_prefix.parent_path(), filesystem_error);
  }
  const auto log_path = options.output_prefix.wstring() + L".log";
  const auto source_path = options.output_prefix.wstring() + L"-source-r10g10b10a2.raw";
  const auto composed_path = options.output_prefix.wstring() + L"-composed-bgra8.bmp";
  std::ofstream log {log_path, std::ios::trunc};
  if (!log) {
    std::cerr << "could not open proof log\n";
    return 1;
  }
  log.setf(std::ios::unitbuf);

  DWORD session_id = (std::numeric_limits<DWORD>::max)();
  ProcessIdToSessionId(GetCurrentProcessId(), &session_id);
  log << "process_id=" << GetCurrentProcessId() << '\n'
      << "session_id=" << session_id << '\n'
      << "duration_seconds=" << options.duration_seconds << '\n';

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

  const std::array<std::array<float, 4>, 5> colors {{
    {0.0f, 0.0f, 0.0f, 1.0f},
    {0.508078f, 0.508078f, 0.508078f, 1.0f},
    {0.652579f, 0.0f, 0.0f, 1.0f},
    {0.0f, 0.751827f, 0.0f, 1.0f},
    {0.751827f, 0.751827f, 0.751827f, 1.0f},
  }};

  bool source_captured = false;
  bool composed_captured = false;
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
    if (!composed_captured && std::chrono::steady_clock::now() - start >= std::chrono::seconds(2)) {
      composed_captured = write_composed_bmp(rectangle, composed_path, log);
    }
  }

  log << "present_count=" << present_count << '\n'
      << "proof_completed=1\n";
  if (IsWindow(window)) {
    DestroyWindow(window);
  }
  return source_captured && composed_captured && SUCCEEDED(color_set_result) ? 0 : 1;
}
