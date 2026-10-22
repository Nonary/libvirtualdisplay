// Exercise the production guard with real filesystem resolution, without
// loading KWin or touching a compositor. No source-text assertions.
#include "../../linux/kwin_native_abi.h"

#include <fcntl.h>
#include <unistd.h>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
  using vibeshine::kwin::native_abi_result;
  int allocator;
  void *mapped_allocator;
  void *lookup_mapped(void *, const char *) {
    return mapped_allocator;
  }
  bool symbol_available = true;
  bool address_available = true;
  const char *library_path;

  void require(bool condition, const char *message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }
  void *lookup(void *scope, const char *name) {
    require(scope == RTLD_NEXT, "must resolve the next allocator, not the bridge");
    require(std::strcmp(name, "allocator") == 0, "wrong allocator symbol");
    return symbol_available ? &allocator : nullptr;
  }
  int address(const void *symbol, Dl_info *info) {
    require(symbol == &allocator, "must identify the resolved allocator");
    info->dli_fname = library_path;
    return address_available;
  }
  struct fixture {
    char directory[64] = "/tmp/kwin-native-abi-XXXXXX";
    std::string old_library, new_library, link;
    fixture() {
      require(mkdtemp(directory) != nullptr, "mkdtemp failed");
      old_library = std::string(directory) + "/libkwin.so.6.8.0";
      new_library = std::string(directory) + "/libkwin.so.6.8.1";
      link = std::string(directory) + "/libkwin.so.6";
      for (const auto &path : {old_library, new_library}) {
        int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
        require(fd >= 0, "fixture creation failed");
        close(fd);
      }
      require(symlink(old_library.c_str(), link.c_str()) == 0, "symlink failed");
    }
    ~fixture() {
      unlink(link.c_str());
      unlink(old_library.c_str());
      unlink(new_library.c_str());
      rmdir(directory);
    }
  };
  auto probe() {
    return vibeshine::kwin::probe_native_abi("allocator", "libkwin.so.6.8.0", lookup, address);
  }
  std::string diagnostic(const vibeshine::kwin::native_abi_probe &result) {
    FILE *file = tmpfile();
    require(file != nullptr, "diagnostic capture failed");
    vibeshine::kwin::report_native_abi_failure(file, result, "libkwin.so.6.8.0");
    rewind(file);
    std::string output;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), file)) {
      output += buffer;
    }
    fclose(file);
    return output;
  }
}

int main(int argc, char **argv) {
  try {
    require(argc == 3, "expected two built DSO fixtures");
    fixture files;
    library_path = files.link.c_str();
    auto result = probe();
    require(result.result == native_abi_result::compatible, "matching symlink must be accepted");
    require(diagnostic(result).empty(), "matching ABI must not emit a failure");

    unlink(files.link.c_str());
    require(symlink(files.new_library.c_str(), files.link.c_str()) == 0, "upgrade symlink failed");
    result = probe();
    require(result.result == native_abi_result::different_library, "patch update must remain rejected");
    const auto message = diagnostic(result);
    for (const char *needle : {"built for libkwin.so.6.8.0", "resolved current pathname libkwin.so.6.8.1",
                              "CPU upload", "rebuilt", "log out", "Restarting the streaming service alone"}) {
      require(message.find(needle) != std::string::npos, "missing actionable runtime diagnostic");
    }
    // A direct old filename still compares equal; unlike the mapped test below,
    // this injected path alone says nothing about the actual loaded identity.
    library_path = files.old_library.c_str();
    require(probe().result == native_abi_result::compatible, "direct old filename comparison must remain compatible");

    library_path = files.directory; // basename must not be treated as a compatible prefix
    require(probe().result == native_abi_result::different_library, "unrelated name must fail closed");
    library_path = nullptr;
    require(probe().result == native_abi_result::unavailable_library, "missing owner path must fail closed");
    address_available = false;
    require(probe().result == native_abi_result::unavailable_library, "dladdr failure must fail closed");
    address_available = true;
    library_path = files.old_library.c_str();
    unlink(library_path);
    result = probe();
    require(result.result == native_abi_result::unresolved_library, "deleted library must fail closed");
    require(diagnostic(result).find("resolved current pathname unknown") != std::string::npos, "unknown identity must not be invented");
    symbol_available = false;
    require(probe().result == native_abi_result::missing_allocator, "missing allocator must fail closed");
    // Unlike the injected direct-filename case, real dladdr retains the SONAME
    // pathname used by dlopen, even after its symlink points at a newer DSO.
    unlink(files.link.c_str());
    require(symlink(argv[1], files.link.c_str()) == 0, "mapped fixture link failed");
    void *handle = dlopen(files.link.c_str(), RTLD_NOW | RTLD_LOCAL);
    require(handle != nullptr, "fixture dlopen failed");
    mapped_allocator = dlsym(handle, "kwin_abi_fixture");
    require(mapped_allocator != nullptr, "fixture allocator missing");
    auto value = reinterpret_cast<int (*)()>(mapped_allocator);
    require(value() == 680, "old fixture was not loaded");
    unlink(files.link.c_str());
    require(symlink(argv[2], files.link.c_str()) == 0, "mapped fixture retarget failed");
    require(value() == 680, "retarget unexpectedly replaced mapped code");
    result = vibeshine::kwin::probe_native_abi("kwin_abi_fixture", "libkwin.so.6.8.0", lookup_mapped);
    require(result.result == native_abi_result::different_library, "retarget must retain guard rejection");
    const auto mapped_message = diagnostic(result);
    require(mapped_message.find("resolved current pathname libkwin.so.6.8.1") != std::string::npos,
            "diagnostic must describe the current pathname target");
    require(mapped_message.find("mapped library identity unverified") != std::string::npos,
            "diagnostic must disclose that mapped identity is unverified");
    require(mapped_message.find("loaded libkwin.so.6.8.1") == std::string::npos,
            "diagnostic must not misidentify the old mapped code as the new library");
    dlclose(handle);
    std::cout << "KWin native ABI guard and diagnostics: PASS\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
