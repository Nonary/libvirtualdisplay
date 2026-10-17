// Exercise the production GBM hooks with DRM/GBM providers, without changing
// host displays or requiring a particular GPU. This verifies allocator routing
// and fd ownership; real console, KScreen and client cadence remain runtime tests.
#include <sys/stat.h>

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include <gbm.h>
#include <xf86drm.h>

struct gbm_device {
  int fd;
};

#include "../../linux/kwin_gpu_interposer.cpp"

namespace {
  void require(bool value, const char *message) {
    if (!value) {
      throw std::runtime_error(message);
    }
  }

  struct device_fixture {
    drmDevice record {};
    drmPciBusInfo bus {};
    drmPciDeviceInfo pci {};
    drmFauxBusInfo faux {};
    std::array<char *, DRM_NODE_MAX> nodes {};
    std::array<char, 64> path {};
    int fd;

    device_fixture(int bustype, unsigned int vendor, unsigned int pci_bus, const char *name = "") {
      std::strcpy(path.data(), "/tmp/kwin-gpu-routing-XXXXXX");
      fd = mkstemp(path.data());
      require(fd >= 0, "temporary DRM provider failed");
      record.bustype = bustype;
      record.nodes = nodes.data();
      nodes[DRM_NODE_PRIMARY] = path.data();
      record.available_nodes = 1 << DRM_NODE_PRIMARY;
      bus.bus = pci_bus;
      pci.vendor_id = vendor;
      if (bustype == DRM_BUS_FAUX) {
        std::strncpy(faux.name, name, sizeof(faux.name) - 1);
        record.businfo.faux = &faux;
      } else {
        record.businfo.pci = &bus;
        record.deviceinfo.pci = &pci;
        record.available_nodes |= 1 << DRM_NODE_RENDER;
        nodes[DRM_NODE_RENDER] = path.data();
      }
    }
    ~device_fixture() {
      close(fd);
      unlink(path.data());
    }
    bool matches_fd(int candidate) const {
      struct stat actual {}, expected {};
      return fstat(candidate, &actual) == 0 && fstat(fd, &expected) == 0 &&
        actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino;
    }
  };

  std::vector<device_fixture *> native_devices;
  device_fixture *virtual_device;
  device_fixture *console_device;
  device_fixture *renumbered_device;
  std::vector<int> created_fds;
  std::vector<int> destroyed_fds;
  int enumerations;
  int lookups;
  int last_node_fd;
  bool reject_render_gbm;
  bool reject_render_type;
  bool legacy_allocator_symbol;
  bool native_allocator_symbol;
#ifdef VIBESHINE_KWIN_NATIVE_ALLOCATOR
  std::function<void *(const void *, drmDevicePtr)> render_device_body;
  void *native_render_device(const void *manager, drmDevicePtr device) {
    return render_device_body(manager, device);
  }
  std::function<KWin::GraphicsBuffer *(void *, const KWin::GraphicsBufferOptions &)> allocate_body;
  KWin::GraphicsBuffer *native_allocate(void *allocator, const KWin::GraphicsBufferOptions &options) {
    return allocate_body(allocator, options);
  }
#endif

  void *lookup_allocator(void *scope, const char *name) {
    require(scope == RTLD_DEFAULT, "allocator lookup used a different symbol scope");
    static int symbol;
    if (std::strcmp(name, "_ZN4KWin26GbmGraphicsBufferAllocatorC1EP10gbm_devicem") == 0) {
      return legacy_allocator_symbol ? &symbol : nullptr;
    }
    if (std::strcmp(name, "_ZN4KWin26GbmGraphicsBufferAllocatorC1EPNS_9DrmDeviceE") == 0) {
      return native_allocator_symbol ? &symbol : nullptr;
    }
    throw std::runtime_error("unexpected KWin ABI lookup");
  }

  int get_devices(std::uint32_t, drmDevicePtr devices[], int capacity) {
    ++enumerations;
    if (!devices) {
      return static_cast<int>(native_devices.size());
    }
    const int count = std::min(capacity, static_cast<int>(native_devices.size()));
    for (int i = 0; i < count; ++i) {
      devices[i] = &native_devices[i]->record;
    }
    return count;
  }

  int get_device(int fd, std::uint32_t, drmDevicePtr *device) {
    ++lookups;
    for (auto *candidate : native_devices) {
      if (candidate->matches_fd(fd)) {
        // Simulate renderD* being reused by a different GPU after enumeration.
        *device = renumbered_device && fd != candidate->fd ?
          &renumbered_device->record : &candidate->record;
        return 0;
      }
    }
    *device = nullptr;
    return -1;
  }

  gbm_device *create_device(int fd) {
    require(fcntl(fd, F_GETFD) >= 0, "GBM received a closed fd");
    created_fds.push_back(fd);
    if (reject_render_gbm && fd != virtual_device->fd) {
      errno = EIO;
      return nullptr;
    }
    return new gbm_device {fd};
  }

  void destroy_device(gbm_device *device) {
    require(fcntl(device->fd, F_GETFD) >= 0, "render fd was closed before GBM destruction");
    destroyed_fds.push_back(device->fd);
    delete device;
  }

  void reset() {
    require(owned_render_fds.empty(), "render fd ownership leaked across tests");
    hooks_enabled = true;
    native_hooks_enabled = true;
    real_gbm_create_device = create_device;
    real_gbm_device_destroy = destroy_device;
    real_gbm_device_get_fd = +[](gbm_device *device) { return device->fd; };
#ifdef VIBESHINE_KWIN_NATIVE_ALLOCATOR
    real_native_render_device = native_render_device;
    real_native_allocate = native_allocate;
    software_allocation = false;
#endif
    real_drm_get_devices2 = get_devices;
    real_drm_get_device2 = get_device;
    gpu_attachment_logged = false;
    ambiguous_gpu_logged = false;
    created_fds.clear();
    destroyed_fds.clear();
    enumerations = lookups = 0;
    last_node_fd = -1;
    renumbered_device = nullptr;
    reject_render_gbm = reject_render_type = false;
    legacy_allocator_symbol = native_allocator_symbol = false;
    unsetenv("VIBESHINE_KWIN_RENDER_PCI");
  }

  void require_original_devices() {
    require(virtual_device->record.bustype == DRM_BUS_FAUX &&
      virtual_device->record.businfo.faux == &virtual_device->faux,
      "virtual DRM identity changed");
    require(console_device->record.bustype == DRM_BUS_PCI &&
      console_device->record.businfo.pci == &console_device->bus &&
      console_device->record.deviceinfo.pci->vendor_id == 0x1af4,
      "Virtio console identity changed");
    for (auto *candidate : native_devices) {
      require(candidate->record.nodes == candidate->nodes.data(), "DRM node list changed");
    }
  }

  void require_routed(gbm_device *device, device_fixture &renderer) {
    require(device && device->fd != virtual_device->fd && renderer.matches_fd(device->fd),
      "virtual allocator did not use the selected NVIDIA render node");
    require((fcntl(device->fd, F_GETFD) & FD_CLOEXEC) != 0, "render fd can leak into children");
    const int render_fd = device->fd;
    const int kms_fd = owned_render_fds.at(device).kms;
    require(virtual_device->matches_fd(kms_fd) && (fcntl(kms_fd, F_GETFD) & FD_CLOEXEC),
      "software allocator lost its owned KMS descriptor");
    gbm_device_destroy(device);
    require(fcntl(render_fd, F_GETFD) == -1 && errno == EBADF,
      "render fd survived GBM destruction");
    require(fcntl(kms_fd, F_GETFD) == -1 && errno == EBADF,
      "owned KMS fd survived GBM destruction");
    require(fcntl(virtual_device->fd, F_GETFD) >= 0, "caller-owned virtual KMS fd was closed");
    require_original_devices();
  }

  void require_fallback() {
    auto *device = gbm_create_device(virtual_device->fd);
    require(device && device->fd == virtual_device->fd, "virtual fallback used a different fd");
    require(owned_render_fds.empty(), "fallback retained a renderer fd");
    if (last_node_fd >= 0) {
      require(fcntl(last_node_fd, F_GETFD) == -1 && errno == EBADF,
        "failed renderer association leaked an fd");
    }
    gbm_device_destroy(device);
    require(fcntl(virtual_device->fd, F_GETFD) >= 0, "fallback closed caller-owned fd");
    require_original_devices();
  }
}

// Providers own these fixtures; native libdrm owns allocations in production.
extern "C" void drmFreeDevice(drmDevicePtr *device) {
  *device = nullptr;
}
extern "C" void drmFreeDevices(drmDevicePtr devices[], int count) {
  for (int i = 0; i < count; ++i) {
    devices[i] = nullptr;
  }
}
extern "C" int drmGetNodeTypeFromFd(int fd) {
  last_node_fd = fd;
  return reject_render_type ? DRM_NODE_PRIMARY : DRM_NODE_RENDER;
}

int main() {
  try {
    device_fixture virtual_kms(DRM_BUS_FAUX, 0, 0, "vibeshine");
    device_fixture virtio(DRM_BUS_PCI, 0x1af4, 3);
    device_fixture nvidia_one(DRM_BUS_PCI, 0x10de, 1);
    device_fixture nvidia_two(DRM_BUS_PCI, 0x10de, 2);
    device_fixture unrelated_faux(DRM_BUS_FAUX, 0, 0, "unrelated");
    virtual_device = &virtual_kms;
    console_device = &virtio;
    const auto run = [&](const char *name, const std::function<void()> &test) {
      reset();
      native_devices = {&virtual_kms, &virtio, &nvidia_one};
      test();
      require(owned_render_fds.empty(), "owned render fd leaked");
      std::cout << "PASS: " << name << '\n';
    };
    run("outside KWin all GBM calls pass through without DRM enumeration", [&] {
      hooks_enabled = false;
      auto *device = gbm_create_device(virtual_kms.fd);
      require(device->fd == virtual_kms.fd && lookups == 0 && enumerations == 0,
        "non-compositor GBM call was inspected or routed");
      gbm_device_destroy(device);
      require_original_devices();
    });
    run("a preload marker cannot enable hooks in a wrapper or another process", [&] {
      hooks_enabled = false;
      setenv("VIBESHINE_KWIN_GPU_PRELOAD_ACTIVE", "1", 1);
      setenv("LD_PRELOAD", "libvibeshine-kwin-gpu.so", 1);
      stop_kwin_preload_inheritance();
      require(!hooks_enabled && std::getenv("LD_PRELOAD"), "hook enabled outside actual KWin");
      unsetenv("LD_PRELOAD");
      unsetenv("VIBESHINE_KWIN_GPU_PRELOAD_ACTIVE");
    });
    run("Virtio console GBM keeps its original fd despite an explicit NVIDIA choice", [&] {
      setenv("VIBESHINE_KWIN_RENDER_PCI", "0000:01:00.0", 1);
      errno = EBUSY;
      auto *device = gbm_create_device(virtio.fd);
      require(device->fd == virtio.fd && enumerations == 0 && errno == EBUSY,
        "physical seat allocator was routed or changed errno");
      gbm_device_destroy(device);
      require(fcntl(virtio.fd, F_GETFD) >= 0, "caller-owned console fd was closed");
      require_original_devices();
    });
    run("recognized legacy allocator retains NVIDIA routing", [&] {
      legacy_allocator_symbol = true;
      configure_allocator_routing(lookup_allocator);
      require(hooks_enabled, "legacy KWin lost its virtual rendering bridge");
      require_routed(gbm_create_device(virtual_kms.fd), nvidia_one);
    });
    run("unmatched native KWin library retains its complete native allocation path", [&] {
      native_allocator_symbol = true;
      setenv("VIBESHINE_KWIN_GPU_PRELOAD_ACTIVE", "1", 1);
      setenv("LD_PRELOAD", "libvibeshine-kwin-gpu.so", 1);
      configure_allocator_routing(lookup_allocator, false);
      require(!hooks_enabled, "native scanout allocator was routed to a render node");
      require(!std::getenv("LD_PRELOAD") && !std::getenv("VIBESHINE_KWIN_GPU_PRELOAD_ACTIVE"),
        "native allocator path leaked preload into compositor children");
      require_fallback();
      require(lookups == 0 && enumerations == 0, "native KMS allocation inspected other GPUs");
      auto *physical = gbm_create_device(nvidia_one.fd);
      require(physical->fd == nvidia_one.fd, "native physical allocator changed");
      gbm_device_destroy(physical);
      require(fcntl(nvidia_one.fd, F_GETFD) >= 0, "native path closed physical caller fd");
    });
    run("native allocator wins when a compatibility legacy symbol remains", [&] {
      legacy_allocator_symbol = native_allocator_symbol = true;
      configure_allocator_routing(lookup_allocator);
      require(!hooks_enabled, "legacy compatibility symbol bypassed native allocator guard");
      require_fallback();
    });
    run("unknown KWin allocator ABI preserves native allocation", [&] {
      configure_allocator_routing(lookup_allocator);
      require(!hooks_enabled, "unrecognized KWin ABI enabled allocator substitution");
      require_fallback();
    });
    run("single NVIDIA association works with an unrelated Virtio console", [&] {
      require_routed(gbm_create_device(virtual_kms.fd), nvidia_one);
    });
    run("explicit PCI chooses one of two NVIDIA devices without changing seat metadata", [&] {
      native_devices = {&nvidia_two, &virtio, &virtual_kms, &nvidia_one};
      setenv("VIBESHINE_KWIN_RENDER_PCI", "0000:01:00.0", 1);
      require_routed(gbm_create_device(virtual_kms.fd), nvidia_one);
    });
    run("the second NVIDIA can be chosen independently of enumeration order", [&] {
      native_devices.push_back(&nvidia_two);
      setenv("VIBESHINE_KWIN_RENDER_PCI", "0000:02:00.0", 1);
      require_routed(gbm_create_device(virtual_kms.fd), nvidia_two);
    });
    run("multiple NVIDIA devices automatically use the lowest PCI address", [&] {
      native_devices.push_back(&nvidia_two);
      require_routed(gbm_create_device(virtual_kms.fd), nvidia_one);
    });
    run("automatic PCI selection is independent of DRM enumeration order", [&] {
      native_devices = {&nvidia_two, &virtio, &virtual_kms, &nvidia_one};
      require_routed(gbm_create_device(virtual_kms.fd), nvidia_one);
    });
    run("automatic selection orders the full PCI tuple", [&] {
      device_fixture higher_domain(DRM_BUS_PCI, 0x10de, 0);
      higher_domain.bus.domain = 1;
      device_fixture higher_device(DRM_BUS_PCI, 0x10de, 1);
      higher_device.bus.dev = 1;
      device_fixture higher_function(DRM_BUS_PCI, 0x10de, 1);
      higher_function.bus.func = 1;
      native_devices = {&higher_domain, &higher_function, &higher_device, &virtual_kms, &virtio, &nvidia_one};
      require_routed(gbm_create_device(virtual_kms.fd), nvidia_one);
    });
    run("empty override uses the stable automatic multi-GPU selection", [&] {
      native_devices = {&nvidia_two, &virtual_kms, &virtio, &nvidia_one};
      setenv("VIBESHINE_KWIN_RENDER_PCI", "", 1);
      require_routed(gbm_create_device(virtual_kms.fd), nvidia_one);
    });
    run("invalid PCI override safely preserves original virtual and seat allocators", [&] {
      setenv("VIBESHINE_KWIN_RENDER_PCI", "0000:01:00.0junk", 1);
      require_fallback();
    });
    run("an unmatched explicit PCI override does not silently pick another GPU", [&] {
      setenv("VIBESHINE_KWIN_RENDER_PCI", "0000:02:00.0", 1);
      require_fallback();
    });
    run("an unmatched explicit override also refuses the automatic multi-GPU default", [&] {
      native_devices.push_back(&nvidia_two);
      setenv("VIBESHINE_KWIN_RENDER_PCI", "0000:03:00.0", 1);
      require_fallback();
    });
    run("render-node renumbering is rejected after open", [&] {
      renumbered_device = &nvidia_two;
      require_fallback();
    });
    run("a primary node masquerading as the render path is rejected", [&] {
      reject_render_type = true;
      require_fallback();
    });
    run("GBM creation failure closes the owned render fd and restores original path", [&] {
      reject_render_gbm = true;
      require_fallback();
      require(created_fds.size() == 2, "failed renderer did not restore original GBM path");
    });
    run("each virtual allocator owns its render fd until its own destruction", [&] {
      auto *first = gbm_create_device(virtual_kms.fd);
      auto *second = gbm_create_device(virtual_kms.fd);
      require(first->fd != second->fd, "independent GBM allocators share an owned fd");
      require_routed(first, nvidia_one);
      require(fcntl(second->fd, F_GETFD) >= 0, "first destruction closed second renderer fd");
      require_routed(second, nvidia_one);
    });
    run("unrelated faux device is never routed", [&] {
      native_devices.push_back(&unrelated_faux);
      auto *device = gbm_create_device(unrelated_faux.fd);
      require(device->fd == unrelated_faux.fd && enumerations == 0, "unrelated faux GBM was routed");
      gbm_device_destroy(device);
    });
    run("no NVIDIA renderer preserves the original virtual allocator", [&] {
      native_devices = {&virtual_kms, &virtio};
      require_fallback();
    });
#ifdef VIBESHINE_KWIN_NATIVE_ALLOCATOR
    run("native compatibility selects NVIDIA only for the managed virtual output", [&] {
      int manager, selected;
      render_device_body = [&](const void *actual, drmDevicePtr device) -> void * {
        require(actual == &manager, "compatibility query changed the GPU manager");
        require(device == &nvidia_one.record, "virtual output did not query the real NVIDIA renderer");
        return &selected;
      };
      require(vibeshine_kwin_render_device(&manager, &virtual_kms.record) == &selected,
        "virtual output lost its NVIDIA render association");
      require_original_devices();
      require(owned_render_fds.empty() && fcntl(last_node_fd, F_GETFD) == -1,
        "compatibility query leaked its temporary render fd");
      for (auto *physical : {&virtio, &nvidia_one, &unrelated_faux}) {
        render_device_body = [&](const void *, drmDevicePtr device) -> void * {
          require(device == &physical->record, "physical or unrelated output was rerouted");
          return &selected;
        };
        const int before = enumerations;
        require(vibeshine_kwin_render_device(&manager, &physical->record) == &selected &&
          enumerations == before, "unrelated compatibility query inspected other GPUs");
      }
    });
    run("native compatibility honors PCI choice and falls back when renderer is absent", [&] {
      native_devices.push_back(&nvidia_two);
      setenv("VIBESHINE_KWIN_RENDER_PCI", "0000:02:00.0", 1);
      int original;
      unsigned calls = 0;
      render_device_body = [&](const void *, drmDevicePtr device) -> void * {
        ++calls;
        if (device == &nvidia_two.record)
          return nullptr;
        require(device == &virtual_kms.record, "missing render device selected another GPU");
        return &original;
      };
      require(vibeshine_kwin_render_device(nullptr, &virtual_kms.record) == &original && calls == 2,
        "missing render device did not preserve native virtual compatibility");
      setenv("VIBESHINE_KWIN_RENDER_PCI", "bad", 1);
      calls = 0;
      require(vibeshine_kwin_render_device(nullptr, &virtual_kms.record) == &original && calls == 1,
        "invalid PCI choice did not preserve native compatibility");
      require_original_devices();
    });
    run("unmatched native ABI leaves render compatibility unchanged", [&] {
      native_hooks_enabled = false;
      int original;
      render_device_body = [&](const void *, drmDevicePtr device) -> void * {
        require(device == &virtual_kms.record, "ABI fallback changed renderer lookup");
        return &original;
      };
      require(vibeshine_kwin_render_device(nullptr, &virtual_kms.record) == &original &&
        enumerations == 0, "ABI fallback routed render compatibility");
    });
    run("matching native allocator uses NVIDIA hardware and original KMS software allocation", [&] {
      native_allocator_symbol = true;
      configure_allocator_routing(lookup_allocator, true);
      require(hooks_enabled, "reviewed native allocator remained disabled");
      auto *device = gbm_create_device(virtual_kms.fd);
      allocate_body = [&](void *, const KWin::GraphicsBufferOptions &options) -> KWin::GraphicsBuffer * {
        const int fd = gbm_device_get_fd(device);
        require(options.software ? virtual_kms.matches_fd(fd) : nvidia_one.matches_fd(fd),
          "hardware and software allocation fds were conflated");
        return nullptr;
      };
      KWin::GraphicsBufferOptions options;
      options.software = true;
      vibeshine_kwin_allocate(nullptr, options);
      require(!software_allocation && nvidia_one.matches_fd(gbm_device_get_fd(device)),
        "software scope escaped into EGL rendering");
      options.software = false;
      vibeshine_kwin_allocate(nullptr, options);
      require_routed(device, nvidia_one);
    });
    run("native software scope leaves physical seat and unrelated threads unchanged", [&] {
      auto *device = gbm_create_device(virtual_kms.fd);
      auto *physical = gbm_create_device(virtio.fd);
      allocate_body = [&](void *, const KWin::GraphicsBufferOptions &) -> KWin::GraphicsBuffer * {
        require(gbm_device_get_fd(physical) == virtio.fd, "software scope changed physical seat fd");
        bool other_thread_rendered = false;
        std::thread other([&] { other_thread_rendered = nvidia_one.matches_fd(gbm_device_get_fd(device)); });
        other.join();
        require(other_thread_rendered, "software scope leaked into another rendering thread");
        return nullptr;
      };
      KWin::GraphicsBufferOptions options;
      options.software = true;
      vibeshine_kwin_allocate(nullptr, options);
      gbm_device_destroy(physical);
      require_routed(device, nvidia_one);
    });
    run("nested native hardware allocation restores outer software scope", [&] {
      auto *device = gbm_create_device(virtual_kms.fd);
      allocate_body = [&](void *, const KWin::GraphicsBufferOptions &options) -> KWin::GraphicsBuffer * {
        if (options.software) {
          require(virtual_kms.matches_fd(gbm_device_get_fd(device)), "outer KMS scope missing");
          KWin::GraphicsBufferOptions hardware;
          hardware.software = false;
          vibeshine_kwin_allocate(nullptr, hardware);
          require(virtual_kms.matches_fd(gbm_device_get_fd(device)), "nested allocation lost outer KMS scope");
        } else {
          require(nvidia_one.matches_fd(gbm_device_get_fd(device)), "nested hardware allocation used KMS fd");
        }
        return nullptr;
      };
      KWin::GraphicsBufferOptions options;
      options.software = true;
      vibeshine_kwin_allocate(nullptr, options);
      require(!software_allocation, "nested software scope leaked");
      require_routed(device, nvidia_one);
    });
    run("native allocation exceptions restore rendering scope", [&] {
      allocate_body = [&](void *, const KWin::GraphicsBufferOptions &) -> KWin::GraphicsBuffer * {
        throw std::runtime_error("allocation failed");
      };
      KWin::GraphicsBufferOptions options;
      options.software = true;
      try {
        vibeshine_kwin_allocate(nullptr, options);
        require(false, "allocation unexpectedly succeeded");
      } catch (const std::runtime_error &) {
        require(!software_allocation, "failed allocation leaked software scope");
      }
    });
#endif
    require(vibeshine_kwin_gpu_interposer_abi() == 7, "unexpected bridge ABI");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
