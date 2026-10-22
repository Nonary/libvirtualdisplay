#pragma once

#include <dlfcn.h>
#include <limits.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vibeshine::kwin {
  enum class native_abi_result {
    compatible,
    missing_allocator,
    unavailable_library,
    unresolved_library,
    different_library,
  };

  struct native_abi_probe {
    native_abi_result result;
    std::array<char, PATH_MAX> runtime_library {};
  };

  // Resolve the library that owns the actual allocator, not a package version
  // or the current libkwin.so symlink (which may change under a running KWin).
  inline native_abi_probe probe_native_abi(
    const char *symbol, const char *expected_library,
    void *(*lookup)(void *, const char *) = dlsym,
    int (*address)(const void *, Dl_info *) = dladdr,
    char *(*resolve)(const char *, char *) = realpath) {
    native_abi_probe probe {native_abi_result::missing_allocator};
    const auto allocator = lookup(RTLD_NEXT, symbol);
    if (!allocator) {
      return probe;
    }
    Dl_info info {};
    probe.result = native_abi_result::unavailable_library;
    if (!address(allocator, &info) || !info.dli_fname) {
      return probe;
    }
    std::array<char, PATH_MAX> resolved {};
    probe.result = native_abi_result::unresolved_library;
    if (!resolve(info.dli_fname, resolved.data())) {
      return probe;
    }
    const auto *name = std::strrchr(resolved.data(), '/');
    if (!name) {
      return probe;
    }
    std::snprintf(probe.runtime_library.data(), probe.runtime_library.size(), "%s", name + 1);
    probe.result = std::strcmp(name + 1, expected_library) == 0 ?
      native_abi_result::compatible : native_abi_result::different_library;
    return probe;
  }

  inline void report_native_abi_failure(FILE *output, const native_abi_probe &probe,
                                       const char *expected_library) {
    if (probe.result == native_abi_result::compatible) {
      return;
    }
    const char *reason = "allocator symbol unavailable";
    switch (probe.result) {
      case native_abi_result::different_library:
        reason = "library version mismatch";
        break;
      case native_abi_result::unavailable_library:
        reason = "allocator library unavailable";
        break;
      case native_abi_result::unresolved_library:
        reason = "allocator library path unresolved";
        break;
      default:
        break;
    }
    std::fprintf(output,
      "[vibeshine-kwin-gpu] native GPU bridge disabled: %s (built for %s; loaded %s). "
      "Keeping native KMS allocation; virtual capture may fall back to CPU upload. "
      "Install a bridge package rebuilt against the installed KWin, then log out and back in or reboot. "
      "Restarting the streaming service alone does not reload the compositor bridge.\n",
      reason, expected_library,
      probe.runtime_library[0] ? probe.runtime_library.data() : "unknown");
  }
}
