// alttab_stress.cpp - Virtual-display alt-tab / teardown brute-force stress harness.
//
// Reproduces the virtual-display alt-tab deadlock/BSOD class by replicating, on a loop and
// as intensively as possible, what a real Sunshine stream of a fullscreen GAME does when
// the user alt-tabs between the game (on the virtual display) and the desktop:
//
//   connect  -> create a temporary virtual display via the libvirtualdisplay control IOCTLs
//               (monitor arrival + swapchain assign)
//   "game"   -> a fullscreen flip-model D3D11 swapchain ON the virtual display that toggles
//               SetFullscreenState on/off rapidly. Entering/leaving exclusive fullscreen is
//               exactly the alt-tab transition: the OS rotates the IddCx swapchain (assign/
//               unassign + the driver worker's DXGI_ERROR_ACCESS_LOST -> teardown/device path).
//   capture  -> DDA-duplicate the virtual display like Sunshine, RECREATING the capture D3D11
//               device on every ACCESS_LOST. That capture device destroy/create is the host-
//               side D3DKMTDestroyHwQueue / D3D11CreateDevice that the dump showed wedged; we
//               issue it concurrently with the game's fullscreen transition, which is the race.
//   disconnect-> release the lease (monitor departure)
//
// Three D3D11 devices contend on the render adapter during each transition (the driver's, the
// game's, and the capture client's) - the documented deadlock condition.
//
// Self-contained: creates and destroys its OWN temporary virtual display, so a driver crash
// only kills the harness's virtual display, not a real stream. Run with a physical display
// present as the safety net.
//
// A watchdog flags any operation that exceeds its bound (the user-mode signature of the wedge)
// and prints heartbeat counters. Logs go to stdout and alttab_stress.log, flushed per line.
//
// Must run as LocalSystem in the interactive session (the control device SDDL grants only SY +
// the broker), e.g.:
//   PsExec64 -accepteula -s -i -w <dir> alttab_stress.exe
// No driver test-signing or reboot needed (Microsoft-whitelisted IddCx driver).

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <virtual_display/driver/control_client.h>
#include <virtual_display/driver/control_protocol.h>
#include <virtual_display/driver/windows_control_client.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace vd = virtual_display::driver;
using sclock = std::chrono::steady_clock;

namespace {
  // ------------------------------------------------------------------ logging --

  std::mutex g_log_mutex;
  FILE *g_logfile = nullptr;

  long long now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(sclock::now().time_since_epoch()).count();
  }

  void logmsg(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    std::lock_guard<std::mutex> lg(g_log_mutex);
    fprintf(stdout, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    fflush(stdout);
    if (g_logfile) {
      fprintf(g_logfile, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
      fflush(g_logfile);
    }
  }

  // ------------------------------------------------------------- run state --

  std::atomic<bool> g_stop {false};

  std::atomic<std::uint64_t> g_sessions {0};
  std::atomic<std::uint64_t> g_alttabs {0};  // fullscreen enter/exit transitions
  std::atomic<std::uint64_t> g_capture_reinits {0};
  std::atomic<std::uint64_t> g_render_reinits {0};
  std::atomic<std::uint64_t> g_capture_frames {0};
  std::atomic<std::uint64_t> g_access_lost {0};
  std::atomic<std::uint64_t> g_create_fail {0};
  std::atomic<std::uint64_t> g_transport_fail {0};

  std::atomic<const char *> g_op_label {nullptr};
  std::atomic<long long> g_op_deadline {0};
  std::atomic<long long> g_op_started {0};

  void begin_op(const char *label, int timeout_s) {
    g_op_started.store(now_ns(), std::memory_order_release);
    g_op_deadline.store(now_ns() + static_cast<long long>(timeout_s) * 1000000000LL, std::memory_order_release);
    g_op_label.store(label, std::memory_order_release);
  }

  void end_op() {
    g_op_label.store(nullptr, std::memory_order_release);
  }

  struct ScopedOp {
    ScopedOp(const char *label, int timeout_s) {
      begin_op(label, timeout_s);
    }

    ~ScopedOp() {
      end_op();
    }
  };

  void watchdog_thread() {
    bool flagged = false;
    const char *flagged_label = nullptr;
    auto last_heartbeat = sclock::now();

    while (!g_stop.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));

      const char *label = g_op_label.load(std::memory_order_acquire);
      const long long deadline = g_op_deadline.load(std::memory_order_acquire);
      const long long now = now_ns();

      if (label && now > deadline) {
        const long long stuck_ms = (now - g_op_started.load(std::memory_order_acquire)) / 1000000LL;
        if (!flagged || flagged_label != label) {
          logmsg("*** HANG DETECTED *** operation '%s' in flight for %lld ms (exceeded its bound). "
                 "User-mode signature of the teardown wedge - capture a full dump of this process AND the "
                 "WUDFHost now.",
                 label, stuck_ms);
          flagged = true;
          flagged_label = label;
        } else if (stuck_ms % 10000 < 600) {
          logmsg("*** still hung *** '%s' stuck for %lld ms.", label, stuck_ms);
        }
      } else if (flagged && !label) {
        logmsg("Hang on '%s' cleared (operation eventually returned).", flagged_label ? flagged_label : "?");
        flagged = false;
        flagged_label = nullptr;
      }

      if (sclock::now() - last_heartbeat >= std::chrono::seconds(5)) {
        last_heartbeat = sclock::now();
        logmsg("heartbeat: sessions=%llu alt_tabs=%llu capture_reinits=%llu render_reinits=%llu frames=%llu "
               "access_lost=%llu create_fail=%llu transport_fail=%llu%s",
               (unsigned long long) g_sessions.load(), (unsigned long long) g_alttabs.load(),
               (unsigned long long) g_capture_reinits.load(), (unsigned long long) g_render_reinits.load(),
               (unsigned long long) g_capture_frames.load(), (unsigned long long) g_access_lost.load(),
               (unsigned long long) g_create_fail.load(), (unsigned long long) g_transport_fail.load(),
               label ? " (op in flight)" : "");
      }
    }
  }

  BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
      logmsg("Ctrl signal received; stopping after the current session...");
      g_stop.store(true, std::memory_order_release);
      return TRUE;
    }
    return FALSE;
  }

  // ------------------------------------------------------------- DXGI utils --

  struct OutputRef {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    std::wstring name;
    LUID luid {};
    LONG left {};
    LONG top {};
    UINT width {};
    UINT height {};
  };

  std::vector<OutputRef> enumerate_attached_outputs() {
    std::vector<OutputRef> result;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      return result;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
      DXGI_ADAPTER_DESC1 adesc {};
      adapter->GetDesc1(&adesc);

      ComPtr<IDXGIOutput> output;
      for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
        DXGI_OUTPUT_DESC odesc {};
        output->GetDesc(&odesc);
        if (odesc.AttachedToDesktop) {
          OutputRef ref;
          ref.adapter = adapter;
          ref.output = output;
          ref.name = odesc.DeviceName;
          ref.luid = adesc.AdapterLuid;
          ref.left = odesc.DesktopCoordinates.left;
          ref.top = odesc.DesktopCoordinates.top;
          ref.width = static_cast<UINT>(odesc.DesktopCoordinates.right - odesc.DesktopCoordinates.left);
          ref.height = static_cast<UINT>(odesc.DesktopCoordinates.bottom - odesc.DesktopCoordinates.top);
          result.push_back(std::move(ref));
        }
        output.Reset();
      }
      adapter.Reset();
    }
    return result;
  }

  bool pick_render_adapter_luid(vd::AdapterLuid &out) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    SIZE_T best = 0;
    bool found = false;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
      DXGI_ADAPTER_DESC1 adesc {};
      adapter->GetDesc1(&adesc);
      if (!(adesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && (!found || adesc.DedicatedVideoMemory > best)) {
        best = adesc.DedicatedVideoMemory;
        out.low_part = adesc.AdapterLuid.LowPart;
        out.high_part = adesc.AdapterLuid.HighPart;
        found = true;
      }
      adapter.Reset();
    }
    return found;
  }

  // --------------------------------------------------------------- capture --
  // Replicates Sunshine's DDA capture, recreating the device on ACCESS_LOST (the contending
  // D3D11CreateDevice / D3DKMTDestroyHwQueue during the mode change).

  class CaptureWorker {
  public:
    void start(OutputRef ref) {
      ref_ = std::move(ref);
      stop_.store(false, std::memory_order_release);
      reinit_.store(true, std::memory_order_release);
      thread_ = std::thread([this]() { run(); });
    }

    void request_reinit() {
      reinit_.store(true, std::memory_order_release);
    }

    void stop_and_join() {
      stop_.store(true, std::memory_order_release);
      if (thread_.joinable()) {
        ScopedOp op {"capture_thread_join", 20};
        thread_.join();
      }
    }

  private:
    void teardown_device() {
      ScopedOp op {"capture_destroy_device", 20};
      dup_.Reset();
      ctx_.Reset();
      dev_.Reset();
    }

    bool init_device() {
      teardown_device();
      ScopedOp op {"capture_create_device", 20};

      const D3D_FEATURE_LEVEL levels[] {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
      };
      D3D_FEATURE_LEVEL got {};
      HRESULT hr = D3D11CreateDevice(ref_.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels,
                                     ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev_, &got, &ctx_);
      if (FAILED(hr)) {
        return false;
      }

      ComPtr<IDXGIOutput5> output5;
      if (SUCCEEDED(ref_.output.As(&output5))) {
        const DXGI_FORMAT formats[] {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
                                     DXGI_FORMAT_R16G16B16A16_FLOAT};
        for (int attempt = 0; attempt < 2; ++attempt) {
          hr = output5->DuplicateOutput1(dev_.Get(), 0, ARRAYSIZE(formats), formats, &dup_);
          if (SUCCEEDED(hr)) {
            return true;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
      }
      ComPtr<IDXGIOutput1> output1;
      if (SUCCEEDED(ref_.output.As(&output1))) {
        for (int attempt = 0; attempt < 2; ++attempt) {
          hr = output1->DuplicateOutput(dev_.Get(), &dup_);
          if (SUCCEEDED(hr)) {
            return true;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
      }
      return false;
    }

    void run() {
      while (!stop_.load(std::memory_order_acquire)) {
        if (reinit_.load(std::memory_order_acquire) || !dup_) {
          reinit_.store(false, std::memory_order_release);
          g_capture_reinits.fetch_add(1, std::memory_order_relaxed);
          if (!init_device()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            continue;
          }
        }

        DXGI_OUTDUPL_FRAME_INFO info {};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dup_->AcquireNextFrame(200, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
          continue;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == WAIT_ABANDONED || hr == DXGI_ERROR_ACCESS_DENIED) {
          g_access_lost.fetch_add(1, std::memory_order_relaxed);
          reinit_.store(true, std::memory_order_release);
          continue;
        }
        if (FAILED(hr)) {
          if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            reinit_.store(true, std::memory_order_release);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          continue;
        }

        g_capture_frames.fetch_add(1, std::memory_order_relaxed);
        res.Reset();
        hr = dup_->ReleaseFrame();
        if (hr == DXGI_ERROR_ACCESS_LOST) {
          g_access_lost.fetch_add(1, std::memory_order_relaxed);
          reinit_.store(true, std::memory_order_release);
        }
      }
      teardown_device();
    }

    OutputRef ref_;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIOutputDuplication> dup_;
    std::thread thread_;
    std::atomic<bool> stop_ {false};
    std::atomic<bool> reinit_ {false};
  };

  // ---------------------------------------------------------------- "game" --
  // A fullscreen flip-model swapchain ON the virtual display that toggles exclusive
  // fullscreen on/off - the alt-tab between a fullscreen game and the desktop. Each
  // transition rotates the IddCx swapchain on the virtual display.

  LRESULT CALLBACK game_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) {
      PostQuitMessage(0);
      return 0;
    }
    return DefWindowProcW(h, m, w, l);
  }

  class GameWorker {
  public:
    void start(OutputRef ref, CaptureWorker *capture, int hold_ms, int mode) {
      ref_ = std::move(ref);
      capture_ = capture;
      hold_ms_ = hold_ms;
      mode_ = mode;
      stop_.store(false, std::memory_order_release);
      thread_ = std::thread([this]() { run(); });
    }

    void stop_and_join() {
      stop_.store(true, std::memory_order_release);
      if (thread_.joinable()) {
        ScopedOp op {"game_thread_join", 20};
        thread_.join();
      }
    }

  private:
    void ensure_class() {
      static std::once_flag once;
      std::call_once(once, []() {
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = game_wndproc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"AltTabStressGame";
        wc.hCursor = nullptr;
        RegisterClassExW(&wc);
      });
    }

    void release_gfx() {
      if (swap_) {
        swap_->SetFullscreenState(FALSE, nullptr);
      }
      rtv_.Reset();
      swap_.Reset();
      ctx_.Reset();
      dev_.Reset();
    }

    bool make_rtv() {
      rtv_.Reset();
      ComPtr<ID3D11Texture2D> back;
      if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        return false;
      }
      return SUCCEEDED(dev_->CreateRenderTargetView(back.Get(), nullptr, &rtv_));
    }

    bool create_gfx() {
      ScopedOp op {"game_create_device", 20};
      release_gfx();

      const D3D_FEATURE_LEVEL levels[] {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
      D3D_FEATURE_LEVEL got {};
      if (FAILED(D3D11CreateDevice(ref_.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev_, &got, &ctx_))) {
        return false;
      }

      ComPtr<IDXGIDevice> dxgi_dev;
      ComPtr<IDXGIAdapter> adap;
      ComPtr<IDXGIFactory2> factory;
      if (FAILED(dev_.As(&dxgi_dev)) || FAILED(dxgi_dev->GetAdapter(&adap)) ||
          FAILED(adap->GetParent(IID_PPV_ARGS(&factory)))) {
        return false;
      }

      DXGI_SWAP_CHAIN_DESC1 scd {};
      scd.Width = ref_.width;
      scd.Height = ref_.height;
      scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      scd.SampleDesc = {1, 0};
      scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      scd.BufferCount = 2;
      scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      scd.Scaling = DXGI_SCALING_NONE;
      scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

      ComPtr<IDXGISwapChain1> sc1;
      HRESULT hr = factory->CreateSwapChainForHwnd(dev_.Get(), hwnd_, &scd, nullptr, ref_.output.Get(), &sc1);
      if (FAILED(hr)) {
        logmsg("game: CreateSwapChainForHwnd failed hr=0x%08lx", (unsigned long) hr);
        return false;
      }
      // Stop DXGI's own Alt+Enter handling; we drive fullscreen explicitly.
      factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
      swap_ = sc1;
      return make_rtv();
    }

    void present_one(float phase) {
      if (!rtv_) {
        return;
      }
      const float color[4] {0.05f, phase, 1.0f - phase, 1.0f};
      ctx_->ClearRenderTargetView(rtv_.Get(), color);
      HRESULT hr = swap_->Present(1, 0);
      if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_ACCESS_LOST) {
        need_recreate_ = true;
      }
    }

    void toggle_fse() {
      fullscreen_ = !fullscreen_;
      if (fullscreen_) {
        SetForegroundWindow(hwnd_);
      }
      {
        ScopedOp op {fullscreen_ ? "game_enter_fullscreen" : "game_exit_fullscreen", 15};
        HRESULT hr = swap_->SetFullscreenState(fullscreen_ ? TRUE : FALSE, fullscreen_ ? ref_.output.Get() : nullptr);
        if (FAILED(hr)) {
          fullscreen_ = false;
        }
      }
      rtv_.Reset();
      HRESULT rb = swap_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
      if (FAILED(rb)) {
        need_recreate_ = true;
      } else {
        make_rtv();
      }
    }

    void toggle_borderless() {
      // Borderless-windowed alt-tab (the confirmed real-world repro): the game window covers
      // the virtual display with a flip-model swapchain. Minimizing makes the OS demote it
      // from independent flip / MPO and reassign the DWM swapchain to the IddCx monitor;
      // restore+foreground re-promotes it. Each transition rotates the driver's swapchain.
      active_ = !active_;
      if (active_) {
        ScopedOp op {"game_alttab_in", 15};
        ShowWindow(hwnd_, SW_RESTORE);
        SetWindowPos(hwnd_, HWND_TOPMOST, ref_.left, ref_.top, static_cast<int>(ref_.width),
                     static_cast<int>(ref_.height), SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd_);
      } else {
        ScopedOp op {"game_alttab_out", 15};
        ShowWindow(hwnd_, SW_MINIMIZE);
      }
    }

    void toggle_alttab() {
      if (!swap_) {
        return;
      }
      // Recreate the capture device right at the transition so its D3D11CreateDevice /
      // DestroyHwQueue overlaps the driver's swapchain rotation - the documented race.
      if (capture_) {
        capture_->request_reinit();
      }
      if (mode_ == 1) {
        toggle_fse();
      } else {
        toggle_borderless();
      }
      g_alttabs.fetch_add(1, std::memory_order_relaxed);
    }

    void run() {
      ensure_class();
      // Allow our (SYSTEM) process to take/relinquish foreground so independent flip
      // promotes/demotes on the alt-tab toggle.
      SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, reinterpret_cast<PVOID>(0), SPIF_SENDCHANGE);
      hwnd_ = CreateWindowExW(0, L"AltTabStressGame", L"AltTabStress Game", WS_POPUP | WS_VISIBLE, ref_.left,
                              ref_.top, static_cast<int>(ref_.width), static_cast<int>(ref_.height), nullptr,
                              nullptr, GetModuleHandleW(nullptr), nullptr);
      if (!hwnd_) {
        logmsg("game: CreateWindow failed err=%lu", GetLastError());
        return;
      }

      if (!create_gfx()) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return;
      }

      auto next_toggle = sclock::now() + std::chrono::milliseconds(hold_ms_);
      unsigned frame = 0;
      while (!stop_.load(std::memory_order_acquire)) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }

        if (need_recreate_) {
          need_recreate_ = false;
          fullscreen_ = false;
          g_render_reinits.fetch_add(1, std::memory_order_relaxed);
          if (!create_gfx()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            continue;
          }
        }

        present_one(static_cast<float>((frame++ % 60) / 60.0f));
        if (!active_ && mode_ != 1) {
          std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }

        if (sclock::now() >= next_toggle) {
          toggle_alttab();
          next_toggle = sclock::now() + std::chrono::milliseconds(hold_ms_);
        }
      }

      release_gfx();
      if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
      }
    }

    OutputRef ref_;
    CaptureWorker *capture_ {nullptr};
    int hold_ms_ {150};
    HWND hwnd_ {nullptr};
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    int mode_ {0};
    bool active_ {true};
    bool fullscreen_ {false};
    bool need_recreate_ {false};
    std::thread thread_;
    std::atomic<bool> stop_ {false};
  };

  // --------------------------------------------------------- control client --

  std::uint64_t g_display_id_counter = 0;

  std::uint64_t gen_lease_id(std::mt19937_64 &rng) {
    std::uint64_t id = 0;
    while (id < vd::kMinOpaqueLeaseId) {
      id = rng() | vd::kMinOpaqueLeaseId;
    }
    return id;
  }

  // --------------------------------------------------------------- session --

  struct Args {
    int minutes = 0;            // 0 == run until Ctrl+C
    int session_seconds = 6;    // how long a "stream" stays connected
    int fullscreen_hold_ms = 150;  // dwell time per game/desktop state (the alt-tab cadence)
    int session_gap_ms = 150;
    int mode = 0;               // 0 = borderless windowed (confirmed repro), 1 = exclusive fullscreen
    UINT width = 1920;
    UINT height = 1080;
    UINT refresh_millihz = 60000;
  };

  void run_session(vd::ControlClient &client, std::mt19937_64 &rng, const Args &args) {
    const std::uint64_t lease_id = gen_lease_id(rng);
    const std::uint64_t display_id = (++g_display_id_counter) | 0x1000u;

    auto names_before = enumerate_attached_outputs();

    vd::CreateTemporaryDisplayRequest req {};
    req.lease_id = lease_id;
    req.display_id = display_id;
    req.width = args.width;
    req.height = args.height;
    req.physical_width_mm = vd::kDefaultPhysicalWidthMillimeters;
    req.physical_height_mm = vd::kDefaultPhysicalHeightMillimeters;
    req.refresh_rate_millihz = args.refresh_millihz;
    req.requested_timeout_ms = 30000;
    std::snprintf(req.display_name, sizeof(req.display_name), "AltTabStress");

    vd::CreateTemporaryDisplayResult create_value {};
    {
      ScopedOp op {"create_vd", 15};
      auto created = client.create_temporary_display(req);
      if (!created.ok()) {
        g_create_fail.fetch_add(1, std::memory_order_relaxed);
        if (created.status == vd::ControlStatus::TransportFailed) {
          g_transport_fail.fetch_add(1, std::memory_order_relaxed);
        }
        logmsg("create_temporary_display failed: status=%s native_error=%lu (display_id=%llu)",
               vd::to_string(created.status), (unsigned long) created.native_error,
               (unsigned long long) display_id);
        return;
      }
      create_value = created.value;
    }

    const std::uint64_t eff_lease = create_value.lease_id != 0 ? create_value.lease_id : lease_id;

    OutputRef vd_output;
    bool found = false;
    {
      ScopedOp op {"resolve_vd_output", 15};
      auto deadline = sclock::now() + std::chrono::seconds(8);
      while (sclock::now() < deadline && !found && !g_stop.load(std::memory_order_acquire)) {
        auto after = enumerate_attached_outputs();
        for (auto &o : after) {
          bool existed = false;
          for (auto &b : names_before) {
            if (b.name == o.name) {
              existed = true;
              break;
            }
          }
          if (!existed) {
            vd_output = o;
            found = true;
            break;
          }
        }
        if (!found) {
          std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
      }
    }

    if (!found) {
      logmsg("could not resolve the new virtual-display output; releasing lease and retrying.");
      ScopedOp op {"release_vd", 15};
      vd::LeaseRequest rel {};
      rel.lease_id = eff_lease;
      rel.requested_timeout_ms = 30000;
      (void) client.release_lease(rel);
      return;
    }

    std::atomic<bool> feed_stop {false};
    std::thread feeder([&]() {
      while (!feed_stop.load(std::memory_order_acquire) && !g_stop.load(std::memory_order_acquire)) {
        vd::LeaseRequest feed {};
        feed.lease_id = eff_lease;
        feed.requested_timeout_ms = 30000;
        (void) client.feed_lease(feed);
        for (int i = 0; i < 40 && !feed_stop.load(std::memory_order_acquire); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    });

    CaptureWorker capture;
    capture.start(vd_output);

    GameWorker game;
    game.start(vd_output, &capture, args.fullscreen_hold_ms, args.mode);

    // Let the game alt-tab against the captured virtual display for the session window.
    auto session_end = sclock::now() + std::chrono::seconds(args.session_seconds);
    while (sclock::now() < session_end && !g_stop.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Disconnect: stop the game (last fullscreen->windowed transition), stop capture, depart.
    game.stop_and_join();
    capture.stop_and_join();

    feed_stop.store(true, std::memory_order_release);
    if (feeder.joinable()) {
      feeder.join();
    }

    {
      ScopedOp op {"release_vd", 15};
      vd::LeaseRequest rel {};
      rel.lease_id = eff_lease;
      rel.requested_timeout_ms = 30000;
      auto released = client.release_lease(rel);
      if (!released.ok()) {
        logmsg("release_lease failed: status=%s native_error=%lu", vd::to_string(released.status),
               (unsigned long) released.native_error);
        if (released.status == vd::ControlStatus::TransportFailed) {
          g_transport_fail.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }

    g_sessions.fetch_add(1, std::memory_order_relaxed);
  }

  Args parse_args(int argc, char **argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      auto nexti = [&](int &out) {
        if (i + 1 < argc) {
          out = std::atoi(argv[++i]);
        }
      };
      auto nextu = [&](UINT &out) {
        if (i + 1 < argc) {
          out = static_cast<UINT>(std::strtoul(argv[++i], nullptr, 10));
        }
      };
      if (arg == "--minutes") {
        nexti(a.minutes);
      } else if (arg == "--session-seconds") {
        nexti(a.session_seconds);
      } else if (arg == "--hold-ms") {
        nexti(a.fullscreen_hold_ms);
      } else if (arg == "--session-gap-ms") {
        nexti(a.session_gap_ms);
      } else if (arg == "--mode") {
        if (i + 1 < argc) {
          std::string m = argv[++i];
          a.mode = (m == "fse" || m == "fullscreen" || m == "exclusive") ? 1 : 0;
        }
      } else if (arg == "--width") {
        nextu(a.width);
      } else if (arg == "--height") {
        nextu(a.height);
      } else if (arg == "--refresh-millihz") {
        nextu(a.refresh_millihz);
      }
    }
    return a;
  }
}  // namespace

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  g_logfile = fopen("alttab_stress.log", "w");

  const Args args = parse_args(argc, argv);

  SetConsoleCtrlHandler(ctrl_handler, TRUE);
  std::thread wd(watchdog_thread);

  logmsg("alttab_stress starting. game=%s run=%s session=%ds hold=%dms gap=%dms vd=%ux%u@%umHz",
         args.mode == 1 ? "exclusive-fullscreen" : "borderless-windowed",
         args.minutes > 0 ? "timed" : "until-ctrl-c", args.session_seconds, args.fullscreen_hold_ms,
         args.session_gap_ms, args.width, args.height, args.refresh_millihz);

  auto opened = vd::open_first_control_device();
  if (!opened.ok()) {
    logmsg("FATAL: could not open the virtual-display control device (status=%s native_error=%lu). "
           "Run as LocalSystem (PsExec -s -i); the device SDDL grants only SY + the broker.",
           vd::to_string(opened.status), (unsigned long) opened.native_error);
    g_stop.store(true, std::memory_order_release);
    if (wd.joinable()) {
      wd.join();
    }
    return 1;
  }

  vd::ControlClient client {*opened.transport};

  {
    auto version = client.query_protocol_version();
    if (version.ok()) {
      logmsg("driver protocol %u.%u.%u", version.value.major, version.value.minor, version.value.patch);
    } else {
      logmsg("WARNING: protocol query failed (status=%s); continuing.", vd::to_string(version.status));
    }
  }

  {
    vd::AdapterLuid luid {};
    if (pick_render_adapter_luid(luid)) {
      vd::SetRenderAdapterRequest sra {};
      sra.adapter_luid = luid;
      auto r = client.set_render_adapter(sra);
      logmsg("set_render_adapter(luid=%ld:%lu) -> %s", (long) luid.high_part, (unsigned long) luid.low_part,
             r.ok() ? "ok" : vd::to_string(r.status));
    }
  }

  std::mt19937_64 rng {static_cast<std::uint64_t>(GetCurrentProcessId()) ^ static_cast<std::uint64_t>(now_ns())};

  const auto run_deadline = args.minutes > 0 ? sclock::now() + std::chrono::minutes(args.minutes)
                                             : sclock::time_point::max();

  while (!g_stop.load(std::memory_order_acquire) && sclock::now() < run_deadline) {
    run_session(client, rng, args);
    if (args.session_gap_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(args.session_gap_ms));
    }
  }

  logmsg("stopping. final: sessions=%llu alt_tabs=%llu capture_reinits=%llu render_reinits=%llu frames=%llu "
         "access_lost=%llu create_fail=%llu transport_fail=%llu",
         (unsigned long long) g_sessions.load(), (unsigned long long) g_alttabs.load(),
         (unsigned long long) g_capture_reinits.load(), (unsigned long long) g_render_reinits.load(),
         (unsigned long long) g_capture_frames.load(), (unsigned long long) g_access_lost.load(),
         (unsigned long long) g_create_fail.load(), (unsigned long long) g_transport_fail.load());

  g_stop.store(true, std::memory_order_release);
  if (wd.joinable()) {
    wd.join();
  }
  if (g_logfile) {
    fclose(g_logfile);
  }
  return 0;
}
