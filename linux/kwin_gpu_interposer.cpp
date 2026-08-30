#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include <xf86drm.h>

namespace {
  using drm_get_devices2_fn = int (*)(std::uint32_t, drmDevicePtr[], int);
  using drm_get_device2_fn = int (*)(int, std::uint32_t, drmDevicePtr *);
  using drm_get_device_from_dev_id_fn = int (*)(dev_t, std::uint32_t, drmDevicePtr *);

  constexpr std::string_view kRenderPciEnvironment = "VIBESHINE_KWIN_RENDER_PCI";
  constexpr std::string_view kPreloadActiveEnvironment = "VIBESHINE_KWIN_GPU_PRELOAD_ACTIVE";

  std::atomic<drm_get_devices2_fn> real_drm_get_devices2 {nullptr};
  std::atomic<drm_get_device2_fn> real_drm_get_device2 {nullptr};
  std::atomic<drm_get_device_from_dev_id_fn> real_drm_get_device_from_dev_id {nullptr};
  std::atomic<bool> gpu_attachment_logged {false};
  std::atomic<bool> ambiguous_gpu_logged {false};
  std::mutex metadata_mutex;
  drmPciBusInfo stable_bus_info {};
  drmPciDeviceInfo stable_device_info {};
  bool stable_metadata_ready = false;

  drm_get_devices2_fn resolve_real_drm_get_devices2() {
    auto function = real_drm_get_devices2.load(std::memory_order_acquire);
    if (function) {
      return function;
    }

    function = reinterpret_cast<drm_get_devices2_fn>(dlsym(RTLD_NEXT, "drmGetDevices2"));
    if (function) {
      real_drm_get_devices2.store(function, std::memory_order_release);
    }
    return function;
  }

  template<typename Function>
  Function resolve_real_function(std::atomic<Function> &storage, const char *name) {
    auto function = storage.load(std::memory_order_acquire);
    if (function) {
      return function;
    }

    function = reinterpret_cast<Function>(dlsym(RTLD_NEXT, name));
    if (function) {
      storage.store(function, std::memory_order_release);
    }
    return function;
  }

  bool is_vibeshine_display(const drmDevicePtr device) {
    return device && device->bustype == DRM_BUS_FAUX && device->businfo.faux &&
      std::strcmp(device->businfo.faux->name, "vibeshine") == 0;
  }

  bool is_nvidia_render_device(const drmDevicePtr device) {
    return device && device->bustype == DRM_BUS_PCI && device->businfo.pci &&
      device->deviceinfo.pci && device->deviceinfo.pci->vendor_id == 0x10de &&
      (device->available_nodes & (1 << DRM_NODE_RENDER)) != 0;
  }

  std::optional<drmPciBusInfo> parse_pci_device(const char *value) {
    unsigned int domain = 0;
    unsigned int bus = 0;
    unsigned int device = 0;
    unsigned int function = 0;
    char trailing = '\0';
    if (std::sscanf(value, "%x:%x:%x.%x%c", &domain, &bus, &device, &function, &trailing) != 4 ||
        domain > 0xffff || bus > 0xff || device > 0x1f || function > 7) {
      return std::nullopt;
    }
    return drmPciBusInfo {
      .domain = static_cast<std::uint16_t>(domain),
      .bus = static_cast<std::uint8_t>(bus),
      .dev = static_cast<std::uint8_t>(device),
      .func = static_cast<std::uint8_t>(function),
    };
  }

  bool pci_matches(const drmPciBusInfo &actual, const drmPciBusInfo &expected) {
    return actual.domain == expected.domain && actual.bus == expected.bus &&
      actual.dev == expected.dev && actual.func == expected.func;
  }

  void log_gpu_attachment() {
    if (gpu_attachment_logged.exchange(true, std::memory_order_relaxed)) {
      return;
    }
    char message[192] {};
    const auto length = std::snprintf(
      message, sizeof(message),
      "[vibeshine-kwin-gpu] attached virtual KMS to NVIDIA renderer %04x:%02x:%02x.%u\n",
      stable_bus_info.domain, stable_bus_info.bus, stable_bus_info.dev, stable_bus_info.func);
    if (length > 0) {
      const auto bytes = static_cast<std::size_t>(length) < sizeof(message) ?
        static_cast<std::size_t>(length) : sizeof(message) - 1;
      (void)write(STDERR_FILENO, message, bytes);
    }
  }

  drmDevicePtr select_render_device(drmDevicePtr devices[], const int count) {
    const auto *configured_value = std::getenv(kRenderPciEnvironment.data());
    const auto configured = configured_value && *configured_value ?
      parse_pci_device(configured_value) : std::optional<drmPciBusInfo> {};
    if (configured_value && *configured_value && !configured) {
      if (!ambiguous_gpu_logged.exchange(true, std::memory_order_relaxed)) {
        constexpr char message[] =
          "[vibeshine-kwin-gpu] invalid VIBESHINE_KWIN_RENDER_PCI; refusing GPU association\n";
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
      }
      return nullptr;
    }
    drmDevicePtr selected = nullptr;
    int matches = 0;

    for (int index = 0; index < count; ++index) {
      auto *const candidate = devices[index];
      if (!is_nvidia_render_device(candidate) ||
          (configured && !pci_matches(*candidate->businfo.pci, *configured))) {
        continue;
      }
      selected = candidate;
      ++matches;
    }

    if (matches == 1) {
      return selected;
    }
    if (matches > 1 && !ambiguous_gpu_logged.exchange(true, std::memory_order_relaxed)) {
      constexpr char message[] =
        "[vibeshine-kwin-gpu] multiple NVIDIA render devices found; set VIBESHINE_KWIN_RENDER_PCI\n";
      (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    }
    return nullptr;
  }

  void attach_vibeshine_displays_to_nvidia(drmDevicePtr devices[], const int count) {
    if (!devices || count <= 0) {
      return;
    }

    auto *const render_device = select_render_device(devices, count);
    if (!render_device) {
      return;
    }

    std::lock_guard lock {metadata_mutex};
    if (!stable_metadata_ready) {
      stable_bus_info = *render_device->businfo.pci;
      stable_device_info = *render_device->deviceinfo.pci;
      stable_metadata_ready = true;
    } else if (!pci_matches(stable_bus_info, *render_device->businfo.pci)) {
      return;
    }

    bool attached = false;
    for (int index = 0; index < count; ++index) {
      auto *const display = devices[index];
      if (!is_vibeshine_display(display)) {
        continue;
      }

      // KWin associates a KMS device with its renderer by comparing libdrm bus
      // identity. Rewrite only the enumeration record; card nodes stay distinct.
      display->bustype = DRM_BUS_PCI;
      display->businfo.pci = &stable_bus_info;
      display->deviceinfo.pci = &stable_device_info;
      attached = true;
    }

    if (attached) {
      log_gpu_attachment();
    }
  }

  bool ensure_stable_render_metadata() {
    {
      std::lock_guard lock {metadata_mutex};
      if (stable_metadata_ready) {
        return true;
      }
    }

    const auto function = resolve_real_drm_get_devices2();
    if (!function) {
      return false;
    }
    const auto count = function(0, nullptr, 0);
    if (count <= 0) {
      return false;
    }
    std::vector<drmDevicePtr> devices(static_cast<std::size_t>(count));
    const auto actual_count = function(0, devices.data(), count);
    if (actual_count <= 0) {
      return false;
    }

    auto *const render_device = select_render_device(devices.data(), actual_count);
    if (render_device) {
      std::lock_guard lock {metadata_mutex};
      if (!stable_metadata_ready) {
        stable_bus_info = *render_device->businfo.pci;
        stable_device_info = *render_device->deviceinfo.pci;
        stable_metadata_ready = true;
      }
    }
    drmFreeDevices(devices.data(), actual_count);

    std::lock_guard lock {metadata_mutex};
    return stable_metadata_ready;
  }

  void attach_vibeshine_display_to_nvidia(drmDevicePtr device) {
    if (!is_vibeshine_display(device) || !ensure_stable_render_metadata()) {
      return;
    }

    std::lock_guard lock {metadata_mutex};
    device->bustype = DRM_BUS_PCI;
    device->businfo.pci = &stable_bus_info;
    device->deviceinfo.pci = &stable_device_info;
    log_gpu_attachment();
  }

  __attribute__((constructor)) void stop_preload_inheritance() {
    if (!std::getenv(kPreloadActiveEnvironment.data())) {
      return;
    }

    (void)unsetenv("LD_PRELOAD");
    (void)unsetenv(kPreloadActiveEnvironment.data());
  }
}

extern "C" int drmGetDevices2(const std::uint32_t flags, drmDevicePtr devices[],
                               const int max_devices) {
  const auto function = resolve_real_drm_get_devices2();
  if (!function) {
    errno = ENOSYS;
    return -1;
  }

  const auto count = function(flags, devices, max_devices);
  if (count > 0 && devices) {
    attach_vibeshine_displays_to_nvidia(devices, count);
  }
  return count;
}

extern "C" int drmGetDevice2(const int fd, const std::uint32_t flags,
                              drmDevicePtr *device) {
  const auto function = resolve_real_function(real_drm_get_device2, "drmGetDevice2");
  if (!function) {
    errno = ENOSYS;
    return -1;
  }

  const auto result = function(fd, flags, device);
  if (result == 0 && device && *device) {
    attach_vibeshine_display_to_nvidia(*device);
  }
  return result;
}

extern "C" int drmGetDeviceFromDevId(const dev_t device_id, const std::uint32_t flags,
                                      drmDevicePtr *device) {
  const auto function = resolve_real_function(
    real_drm_get_device_from_dev_id,
    "drmGetDeviceFromDevId"
  );
  if (!function) {
    errno = ENOSYS;
    return -1;
  }

  const auto result = function(device_id, flags, device);
  if (result == 0 && device && *device) {
    attach_vibeshine_display_to_nvidia(*device);
  }
  return result;
}

extern "C" unsigned int vibeshine_kwin_gpu_interposer_abi() {
  return 3;
}
