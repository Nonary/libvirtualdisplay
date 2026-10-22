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
      link = std::string(directory) + "/libkwin.so";
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

int main() {
  try {
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
    for (const char *needle : {"built for libkwin.so.6.8.0", "loaded libkwin.so.6.8.1",
                              "CPU upload", "rebuilt", "log out", "Restarting the streaming service alone"}) {
      require(message.find(needle) != std::string::npos, "missing actionable runtime diagnostic");
    }
    // The allocator can still belong to the old library after package upgrade.
    // Do not infer its identity from the newly retargeted development symlink.
    library_path = files.old_library.c_str();
    require(probe().result == native_abi_result::compatible, "loaded allocator identity must win");

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
    require(diagnostic(result).find("loaded unknown") != std::string::npos, "unknown identity must not be invented");
    symbol_available = false;
    require(probe().result == native_abi_result::missing_allocator, "missing allocator must fail closed");
    std::cout << "KWin native ABI guard and diagnostics: PASS\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
