// VK_LAYER_SUNSHINE_virtual_hdr
//
// Implicit Vulkan layer that appends HDR surface formats
// (A2B10G10R10/HDR10_ST2084 and RGBA16F/EXTENDED_SRGB_LINEAR) to
// vkGetPhysicalDeviceSurfaceFormats(2)KHR results when the surface's monitor
// reports HDR support in Windows but the ICD does not advertise HDR
// colorspaces.
//
// Vendor ICDs (NVIDIA, Intel) only advertise HDR colorspaces for displays
// owned by their own display pipeline. IddCx indirect displays are owned by
// the indirect display adapter, so windowed Vulkan applications never see HDR
// surface formats there even when Windows runs the display in HDR mode. The
// ICD present path honors these colorspaces on indirect displays anyway;
// verified by presenting PQ-encoded content through a forced HDR10 swapchain
// and reading back the composited FP16 scRGB desktop via Desktop Duplication
// (1000 nit PQ white captured as 12.48 scRGB on a Sunshine virtual display,
// NVIDIA 596.49).
//
// Environment variables:
//   ENABLE_SUNSHINE_VIRTUAL_HDR=1   enable the implicit layer for this process.
//   DISABLE_SUNSHINE_VIRTUAL_HDR=1  disable the layer (loader-level).
//   SUNSHINE_VHDR_FORCE=1           inject regardless of monitor HDR support.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

// ---- Vulkan ABI (subset; kept local so the layer builds without an SDK) ----
using VkResult = std::int32_t;
using VkInstance = struct VkInstance_T *;
using VkPhysicalDevice = struct VkPhysicalDevice_T *;
using VkSurfaceKHR = std::uint64_t;

constexpr std::int32_t kVkSuccess = 0;
constexpr std::int32_t kVkIncomplete = 5;
constexpr std::int32_t kVkErrorInitializationFailed = -3;

constexpr std::int32_t kFormatA2B10G10R10UnormPack32 = 64;
constexpr std::int32_t kFormatR16G16B16A16Sfloat = 97;
constexpr std::int32_t kColorSpaceHdr10St2084 = 1000104008;
constexpr std::int32_t kColorSpaceExtendedSrgbLinear = 1000104002;
constexpr std::uint32_t kStructureTypeSurfaceFormat2 = 1000119002;

struct VkSurfaceFormatKHR {
  std::int32_t format;
  std::int32_t colorSpace;
};

struct VkSurfaceFormat2KHR {
  std::uint32_t sType;
  void *pNext;
  VkSurfaceFormatKHR surfaceFormat;
};

struct VkPhysicalDeviceSurfaceInfo2KHR {
  std::uint32_t sType;
  const void *pNext;
  VkSurfaceKHR surface;
};

struct VkWin32SurfaceCreateInfoKHR {
  std::uint32_t sType;
  const void *pNext;
  std::uint32_t flags;
  HINSTANCE hinstance;
  HWND hwnd;
};

struct VkInstanceCreateInfo {
  std::uint32_t sType;
  const void *pNext;
  std::uint32_t flags;
  const void *pApplicationInfo;
  std::uint32_t enabledLayerCount;
  const char *const *ppEnabledLayerNames;
  std::uint32_t enabledExtensionCount;
  const char *const *ppEnabledExtensionNames;
};

struct VkAllocationCallbacks;

using PFN_vkVoidFunction = void (*)();
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(__stdcall *)(VkInstance, const char *);
using PFN_vkCreateInstance = VkResult(__stdcall *)(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
using PFN_vkDestroyInstance = void(__stdcall *)(VkInstance, const VkAllocationCallbacks *);
using PFN_vkCreateWin32SurfaceKHR = VkResult(__stdcall *)(VkInstance, const VkWin32SurfaceCreateInfoKHR *, const VkAllocationCallbacks *, VkSurfaceKHR *);
using PFN_vkDestroySurfaceKHR = void(__stdcall *)(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks *);
using PFN_vkGetPhysicalDeviceSurfaceFormatsKHR = VkResult(__stdcall *)(VkPhysicalDevice, VkSurfaceKHR, std::uint32_t *, VkSurfaceFormatKHR *);
using PFN_vkGetPhysicalDeviceSurfaceFormats2KHR = VkResult(__stdcall *)(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR *, std::uint32_t *, VkSurfaceFormat2KHR *);

// ---- loader/layer negotiation ----
constexpr std::uint32_t kStructureTypeLoaderInstanceCreateInfo = 47;
constexpr std::int32_t kLayerLinkInfo = 0;

struct VkLayerInstanceLink {
  VkLayerInstanceLink *pNext;
  PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
  PFN_vkVoidFunction pfnNextGetPhysicalDeviceProcAddr;
};

struct VkLayerInstanceCreateInfo {
  std::uint32_t sType;
  const void *pNext;
  std::int32_t function;
  union {
    VkLayerInstanceLink *pLayerInfo;
    void *other;
  } u;
};

// ---- layer state ----
struct InstanceData {
  PFN_vkGetInstanceProcAddr next_gipa = nullptr;
  PFN_vkDestroyInstance next_destroy_instance = nullptr;
  PFN_vkCreateWin32SurfaceKHR next_create_win32_surface = nullptr;
  PFN_vkDestroySurfaceKHR next_destroy_surface = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceFormatsKHR next_get_formats = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceFormats2KHR next_get_formats2 = nullptr;
};

std::mutex g_lock;
std::unordered_map<void *, InstanceData> g_instances;
std::unordered_map<std::uint64_t, HWND> g_surface_hwnd;

void *dispatch_key(const void *handle) {
  return *reinterpret_cast<void *const *>(handle);
}

bool instance_data(const void *handle, InstanceData &data) {
  if (!handle) {
    return false;
  }
  std::lock_guard<std::mutex> hold {g_lock};
  const auto it = g_instances.find(dispatch_key(handle));
  if (it == g_instances.end()) {
    return false;
  }
  data = it->second;
  return true;
}

// ---- Windows advanced color query ----
bool monitor_reports_hdr(HWND hwnd) {
  if (!hwnd) {
    return false;
  }
  char force[8] {};
  if (GetEnvironmentVariableA("SUNSHINE_VHDR_FORCE", force, sizeof(force)) > 0 && force[0] == '1') {
    return true;
  }
  HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFOEXW info {};
  info.cbSize = sizeof(info);
  if (!monitor || !GetMonitorInfoW(monitor, &info)) {
    return false;
  }

  UINT32 path_count = 0;
  UINT32 mode_count = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
    return false;
  }
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) != ERROR_SUCCESS) {
    return false;
  }
  for (UINT32 i = 0; i < path_count; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = paths[i].sourceInfo.adapterId;
    source.header.id = paths[i].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
      continue;
    }
    if (wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) {
      continue;
    }

    // DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2 (Windows 11 24H2+)
    // reports HDR support directly; defined locally so older SDKs build.
    struct GetAdvancedColorInfo2 {
      DISPLAYCONFIG_DEVICE_INFO_HEADER header;
      union {
        struct {
          UINT32 advancedColorSupported : 1;
          UINT32 advancedColorActive : 1;
          UINT32 reserved1 : 1;
          UINT32 advancedColorLimitedByPolicy : 1;
          UINT32 highDynamicRangeSupported : 1;
          UINT32 highDynamicRangeUserEnabled : 1;
          UINT32 wideColorSupported : 1;
          UINT32 wideColorUserEnabled : 1;
          UINT32 reserved : 24;
        };
        UINT32 value;
      };
      DISPLAYCONFIG_COLOR_ENCODING colorEncoding;
      UINT32 bitsPerColorChannel;
      UINT32 activeColorMode;
    } aci2 {};
    aci2.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15);
    aci2.header.size = sizeof(aci2);
    aci2.header.adapterId = paths[i].targetInfo.adapterId;
    aci2.header.id = paths[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&aci2.header) == ERROR_SUCCESS) {
      return aci2.highDynamicRangeSupported != 0;
    }

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO aci1 {};
    aci1.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    aci1.header.size = sizeof(aci1);
    aci1.header.adapterId = paths[i].targetInfo.adapterId;
    aci1.header.id = paths[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&aci1.header) == ERROR_SUCCESS) {
      return aci1.advancedColorSupported != 0;
    }
  }
  return false;
}

bool should_inject(VkSurfaceKHR surface) {
  HWND hwnd = nullptr;
  {
    std::lock_guard<std::mutex> hold {g_lock};
    const auto it = g_surface_hwnd.find(surface);
    if (it == g_surface_hwnd.end()) {
      return false;
    }
    hwnd = it->second;
  }
  return monitor_reports_hdr(hwnd);
}

void append_missing(std::vector<VkSurfaceFormatKHR> &formats) {
  bool has_hdr10 = false;
  bool has_scrgb = false;
  for (const auto &format : formats) {
    if (format.colorSpace == kColorSpaceHdr10St2084) {
      has_hdr10 = true;
    }
    if (format.colorSpace == kColorSpaceExtendedSrgbLinear) {
      has_scrgb = true;
    }
  }
  if (!has_scrgb) {
    formats.push_back({kFormatR16G16B16A16Sfloat, kColorSpaceExtendedSrgbLinear});
  }
  if (!has_hdr10) {
    formats.push_back({kFormatA2B10G10R10UnormPack32, kColorSpaceHdr10St2084});
  }
}

// ---- intercepted entry points ----
VkResult __stdcall layer_CreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR *create_info,
                                               const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface) {
  InstanceData data;
  if (!instance_data(instance, data) || !data.next_create_win32_surface) {
    return kVkErrorInitializationFailed;
  }
  const VkResult result = data.next_create_win32_surface(instance, create_info, allocator, surface);
  if (result == kVkSuccess && create_info) {
    std::lock_guard<std::mutex> hold {g_lock};
    g_surface_hwnd[*surface] = create_info->hwnd;
  }
  return result;
}

void __stdcall layer_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *allocator) {
  InstanceData data;
  const bool have_data = instance_data(instance, data);
  {
    std::lock_guard<std::mutex> hold {g_lock};
    g_surface_hwnd.erase(surface);
  }
  if (have_data && data.next_destroy_surface) {
    data.next_destroy_surface(instance, surface, allocator);
  }
}

VkResult __stdcall layer_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                                            std::uint32_t *count, VkSurfaceFormatKHR *out) {
  InstanceData data;
  if (!instance_data(physical_device, data) || !data.next_get_formats) {
    return kVkErrorInitializationFailed;
  }
  if (!should_inject(surface)) {
    return data.next_get_formats(physical_device, surface, count, out);
  }

  std::uint32_t base_count = 0;
  VkResult result = data.next_get_formats(physical_device, surface, &base_count, nullptr);
  if (result != kVkSuccess) {
    return data.next_get_formats(physical_device, surface, count, out);
  }
  std::vector<VkSurfaceFormatKHR> formats(base_count);
  result = data.next_get_formats(physical_device, surface, &base_count, formats.data());
  if (result != kVkSuccess && result != kVkIncomplete) {
    return data.next_get_formats(physical_device, surface, count, out);
  }
  formats.resize(base_count);
  append_missing(formats);

  if (!out) {
    *count = static_cast<std::uint32_t>(formats.size());
    return kVkSuccess;
  }
  const auto total = static_cast<std::uint32_t>(formats.size());
  const std::uint32_t to_copy = *count < total ? *count : total;
  std::memcpy(out, formats.data(), to_copy * sizeof(VkSurfaceFormatKHR));
  *count = to_copy;
  return to_copy < total ? kVkIncomplete : kVkSuccess;
}

VkResult __stdcall layer_GetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physical_device,
                                                             const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
                                                             std::uint32_t *count, VkSurfaceFormat2KHR *out) {
  InstanceData data;
  if (!instance_data(physical_device, data) || !data.next_get_formats2) {
    return kVkErrorInitializationFailed;
  }
  if (!surface_info || !should_inject(surface_info->surface)) {
    return data.next_get_formats2(physical_device, surface_info, count, out);
  }

  std::uint32_t base_count = 0;
  VkResult result = data.next_get_formats2(physical_device, surface_info, &base_count, nullptr);
  if (result != kVkSuccess) {
    return data.next_get_formats2(physical_device, surface_info, count, out);
  }
  std::vector<VkSurfaceFormat2KHR> base(base_count);
  for (auto &entry : base) {
    entry.sType = kStructureTypeSurfaceFormat2;
    entry.pNext = nullptr;
  }
  result = data.next_get_formats2(physical_device, surface_info, &base_count, base.data());
  if (result != kVkSuccess && result != kVkIncomplete) {
    return data.next_get_formats2(physical_device, surface_info, count, out);
  }
  std::vector<VkSurfaceFormatKHR> formats;
  formats.reserve(base_count + 2);
  for (std::uint32_t i = 0; i < base_count; ++i) {
    formats.push_back(base[i].surfaceFormat);
  }
  append_missing(formats);

  if (!out) {
    *count = static_cast<std::uint32_t>(formats.size());
    return kVkSuccess;
  }
  const auto total = static_cast<std::uint32_t>(formats.size());
  const std::uint32_t to_copy = *count < total ? *count : total;
  for (std::uint32_t i = 0; i < to_copy; ++i) {
    out[i].surfaceFormat = formats[i];
  }
  *count = to_copy;
  return to_copy < total ? kVkIncomplete : kVkSuccess;
}

void __stdcall layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator) {
  PFN_vkDestroyInstance next = nullptr;
  {
    std::lock_guard<std::mutex> hold {g_lock};
    const auto it = g_instances.find(dispatch_key(instance));
    if (it != g_instances.end()) {
      next = it->second.next_destroy_instance;
      g_instances.erase(it);
    }
  }
  if (next) {
    next(instance, allocator);
  }
}

VkResult __stdcall layer_CreateInstance(const VkInstanceCreateInfo *create_info,
                                        const VkAllocationCallbacks *allocator, VkInstance *instance) {
  auto *chain = const_cast<VkLayerInstanceCreateInfo *>(
    reinterpret_cast<const VkLayerInstanceCreateInfo *>(create_info->pNext));
  while (chain && !(chain->sType == kStructureTypeLoaderInstanceCreateInfo && chain->function == kLayerLinkInfo)) {
    chain = const_cast<VkLayerInstanceCreateInfo *>(
      reinterpret_cast<const VkLayerInstanceCreateInfo *>(chain->pNext));
  }
  if (!chain || !chain->u.pLayerInfo) {
    return kVkErrorInitializationFailed;
  }
  PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

  const auto next_create = reinterpret_cast<PFN_vkCreateInstance>(next_gipa(nullptr, "vkCreateInstance"));
  if (!next_create) {
    return kVkErrorInitializationFailed;
  }
  const VkResult result = next_create(create_info, allocator, instance);
  if (result != kVkSuccess) {
    return result;
  }

  InstanceData data;
  data.next_gipa = next_gipa;
  data.next_destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(next_gipa(*instance, "vkDestroyInstance"));
  data.next_create_win32_surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(next_gipa(*instance, "vkCreateWin32SurfaceKHR"));
  data.next_destroy_surface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(next_gipa(*instance, "vkDestroySurfaceKHR"));
  data.next_get_formats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(next_gipa(*instance, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
  data.next_get_formats2 = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormats2KHR>(next_gipa(*instance, "vkGetPhysicalDeviceSurfaceFormats2KHR"));
  {
    std::lock_guard<std::mutex> hold {g_lock};
    g_instances[dispatch_key(*instance)] = data;
  }
  return kVkSuccess;
}

}  // namespace

extern "C" __declspec(dllexport) PFN_vkVoidFunction __stdcall vkGetInstanceProcAddr(VkInstance instance, const char *name) {
  if (!name) {
    return nullptr;
  }
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
  }
  if (std::strcmp(name, "vkCreateInstance") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(&layer_CreateInstance);
  }
  if (std::strcmp(name, "vkDestroyInstance") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(&layer_DestroyInstance);
  }
  if (std::strcmp(name, "vkCreateWin32SurfaceKHR") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(&layer_CreateWin32SurfaceKHR);
  }
  if (std::strcmp(name, "vkDestroySurfaceKHR") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(&layer_DestroySurfaceKHR);
  }
  if (std::strcmp(name, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(&layer_GetPhysicalDeviceSurfaceFormatsKHR);
  }
  if (std::strcmp(name, "vkGetPhysicalDeviceSurfaceFormats2KHR") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(&layer_GetPhysicalDeviceSurfaceFormats2KHR);
  }
  if (!instance) {
    return nullptr;
  }
  InstanceData data;
  return instance_data(instance, data) && data.next_gipa ? data.next_gipa(instance, name) : nullptr;
}
