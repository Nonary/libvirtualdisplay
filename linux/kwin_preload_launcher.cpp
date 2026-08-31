#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern char **environ;

namespace {
  constexpr char kSystemKwin[] = "/usr/bin/kwin_wayland";
  constexpr char kPlasmaLoginUnit[] = "/usr/lib/systemd/user/plasma-login-kwin_wayland.service";
  constexpr char kExpectedLoginExec[] =
    "/usr/bin/kwin_wayland --no-lockscreen --no-global-shortcuts --no-kactivities "
    "--inputmethod plasma-keyboard --locale1";
  constexpr char kInterposerPath[] = VIBESHINE_KWIN_GPU_LIBRARY_PATH;
  constexpr char kInterposerMarker[] = "vibeshine_kwin_gpu_interposer_abi";
  constexpr char kPreloadActiveEnvironment[] = "VIBESHINE_KWIN_GPU_PRELOAD_ACTIVE";
  constexpr unsigned int kInterposerAbi = 3;
  constexpr off_t kMaximumKwinSize = 256 * 1024 * 1024;
  constexpr off_t kMaximumUnitSize = 64 * 1024;

  using interposer_abi_fn = unsigned int (*)();

  int fail(const char *operation) {
    std::fprintf(stderr, "vibeshine-kwin-launcher: %s: %s\n", operation, std::strerror(errno));
    return 1;
  }

  int fail_message(const char *message) {
    std::fprintf(stderr, "vibeshine-kwin-launcher: %s\n", message);
    return 1;
  }

  bool interposer_is_ready() {
    struct stat metadata {};
    if (lstat(kInterposerPath, &metadata) < 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_uid != 0 || (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
      return false;
    }

    const auto library = dlopen(kInterposerPath, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
      return false;
    }
    dlerror();
    const auto symbol = dlsym(library, kInterposerMarker);
    const auto error = dlerror();
    if (error || !symbol) {
      dlclose(library);
      return false;
    }
    const auto abi = reinterpret_cast<interposer_abi_fn>(symbol);
    const bool ready = abi() == kInterposerAbi;
    dlclose(library);
    return ready;
  }

  bool configure_interposer_environment() {
    constexpr std::array loader_environment {
      "LD_AUDIT",
      "LD_DEBUG",
      "LD_DEBUG_OUTPUT",
      "LD_LIBRARY_PATH",
      "LD_ORIGIN_PATH",
      "LD_PRELOAD",
      "LD_PROFILE",
      "LD_SHOW_AUXV",
    };
    for (const auto *name : loader_environment) {
      if (unsetenv(name) < 0) {
        return false;
      }
    }
    return setenv(kPreloadActiveEnvironment, "1", 1) == 0 &&
      setenv("LD_PRELOAD", kInterposerPath, 1) == 0;
  }

  bool protected_runtime_directory(const std::string &path) {
    struct stat metadata {};
    return lstat(path.c_str(), &metadata) == 0 &&
      S_ISDIR(metadata.st_mode) && metadata.st_uid == getuid() &&
      (metadata.st_mode & (S_IRWXG | S_IRWXO)) == 0;
  }

  std::string prepare_shadow_directory() {
    const auto *runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/' || !protected_runtime_directory(runtime)) {
      errno = EPERM;
      return {};
    }

    auto directory = std::string {runtime} + "/vibeshine-kwin";
    if (mkdir(directory.c_str(), S_IRWXU) < 0 && errno != EEXIST) {
      return {};
    }
    if (!protected_runtime_directory(directory)) {
      errno = EPERM;
      return {};
    }
    return directory;
  }

  bool copy_exactly(const int source, const int destination, const off_t expected_size) {
    std::array<char, 64 * 1024> buffer {};
    off_t copied = 0;
    while (copied < expected_size) {
      const auto remaining = expected_size - copied;
      const auto requested = static_cast<std::size_t>(
        remaining < static_cast<off_t>(buffer.size()) ? remaining : buffer.size());
      const auto bytes_read = read(source, buffer.data(), requested);
      if (bytes_read < 0 && errno == EINTR) {
        continue;
      }
      if (bytes_read <= 0) {
        errno = bytes_read == 0 ? EIO : errno;
        return false;
      }

      ssize_t written = 0;
      while (written < bytes_read) {
        const auto result = write(
          destination,
          buffer.data() + written,
          static_cast<std::size_t>(bytes_read - written));
        if (result < 0 && errno == EINTR) {
          continue;
        }
        if (result <= 0) {
          errno = result == 0 ? EIO : errno;
          return false;
        }
        written += result;
      }
      copied += bytes_read;
    }
    return true;
  }

  bool plasma_login_unit_matches() {
    const auto unit = open(kPlasmaLoginUnit, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (unit < 0) {
      return false;
    }
    struct stat metadata {};
    if (fstat(unit, &metadata) < 0 || !S_ISREG(metadata.st_mode) || metadata.st_uid != 0 ||
        (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0 || metadata.st_size <= 0 ||
        metadata.st_size > kMaximumUnitSize) {
      close(unit);
      return false;
    }
    std::string contents(static_cast<std::size_t>(metadata.st_size), '\0');
    off_t offset = 0;
    while (offset < metadata.st_size) {
      const auto count = read(unit, contents.data() + offset,
                              static_cast<std::size_t>(metadata.st_size - offset));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        close(unit);
        return false;
      }
      offset += count;
    }
    close(unit);

    bool service_section = false;
    unsigned int exec_start_count = 0;
    std::size_t line_start = 0;
    while (line_start <= contents.size()) {
      const auto line_end = contents.find('\n', line_start);
      auto line = contents.substr(line_start, line_end == std::string::npos ?
        std::string::npos : line_end - line_start);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
      }
      const auto first = line.find_first_not_of(" \t");
      line = first == std::string::npos ? std::string {} : line.substr(first);
      if (!line.empty() && line.front() == '[') {
        service_section = line == "[Service]";
      } else if (service_section && line.rfind("ExecStart=", 0) == 0) {
        ++exec_start_count;
        if (line.substr(std::strlen("ExecStart=")) != kExpectedLoginExec) {
          return false;
        }
      }
      if (line_end == std::string::npos) {
        break;
      }
      line_start = line_end + 1;
    }
    return exec_start_count == 1;
  }
}

int main(const int argc, char **argv) {
  if (argc < 1 || !argv) {
    return fail_message("invalid argument vector");
  }
  if (!interposer_is_ready()) {
    return fail_message("required Vibeshine KWin GPU bridge is missing, unsafe, or has an incompatible ABI");
  }
  if (argc == 2 && std::strcmp(argv[1], "--verify-plasma-login-unit") == 0) {
    return plasma_login_unit_matches() ? 0 :
      fail_message("Plasma Login KWin command changed; refusing to mask unknown vendor arguments");
  }

  const auto source = open(kSystemKwin, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (source < 0) {
    return fail("open system KWin");
  }

  struct stat metadata {};
  if (fstat(source, &metadata) < 0) {
    close(source);
    return fail("inspect system KWin");
  }
  if (!S_ISREG(metadata.st_mode) || metadata.st_uid != 0 ||
      (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
      metadata.st_size <= 0 || metadata.st_size > kMaximumKwinSize) {
    close(source);
    return fail_message("system KWin is not a trusted root-owned executable");
  }

  const auto shadow_directory = prepare_shadow_directory();
  if (shadow_directory.empty()) {
    close(source);
    return fail("prepare protected KWin runtime directory");
  }
  auto temporary_path = shadow_directory + "/.kwin_wayland.XXXXXX";
  std::vector<char> temporary_name(temporary_path.begin(), temporary_path.end());
  temporary_name.push_back('\0');
  const auto executable = mkostemp(temporary_name.data(), O_CLOEXEC);
  if (executable < 0) {
    close(source);
    return fail("create temporary KWin shadow");
  }
  if (!copy_exactly(source, executable, metadata.st_size)) {
    const auto saved_errno = errno;
    close(source);
    close(executable);
    unlink(temporary_name.data());
    errno = saved_errno;
    return fail("copy system KWin");
  }
  close(source);

  if (fchmod(executable, S_IRUSR | S_IXUSR) < 0) {
    const auto saved_errno = errno;
    close(executable);
    unlink(temporary_name.data());
    errno = saved_errno;
    return fail("mark KWin image executable");
  }
  if (fsync(executable) < 0) {
    const auto saved_errno = errno;
    close(executable);
    unlink(temporary_name.data());
    errno = saved_errno;
    return fail("sync KWin shadow");
  }
  struct stat shadow_metadata {};
  if (fstat(executable, &shadow_metadata) < 0) {
    const auto saved_errno = errno;
    close(executable);
    unlink(temporary_name.data());
    errno = saved_errno;
    return fail("inspect KWin shadow");
  }

  // Linux refuses to execute an inode while any descriptor still has it open
  // for writing (ETXTBSY). Reopen the already-verified anonymous image through
  // procfs before unlinking it, then close the writable description before
  // fexecve(). This retains the pathname-race protection without trying to
  // execute the mkostemp() O_RDWR descriptor.
  const auto descriptor_path = std::string {"/proc/self/fd/"} + std::to_string(executable);
  const auto launch_image = open(descriptor_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (launch_image < 0) {
    const auto saved_errno = errno;
    close(executable);
    unlink(temporary_name.data());
    errno = saved_errno;
    return fail("reopen KWin shadow read-only");
  }
  struct stat launch_metadata {};
  if (fstat(launch_image, &launch_metadata) < 0 ||
      launch_metadata.st_dev != shadow_metadata.st_dev ||
      launch_metadata.st_ino != shadow_metadata.st_ino ||
      launch_metadata.st_size != metadata.st_size || !S_ISREG(launch_metadata.st_mode)) {
    const auto saved_errno = errno == 0 ? EIO : errno;
    close(launch_image);
    close(executable);
    unlink(temporary_name.data());
    errno = saved_errno;
    return fail("verify read-only KWin shadow");
  }
  if (unlink(temporary_name.data()) < 0) {
    const auto saved_errno = errno;
    close(launch_image);
    close(executable);
    errno = saved_errno;
    return fail("unlink KWin shadow");
  }
  close(executable);

  std::vector<char *> arguments;
  arguments.reserve(static_cast<std::size_t>(argc) + 1);
  arguments.push_back(const_cast<char *>(kSystemKwin));
  for (int index = 1; index < argc; ++index) {
    arguments.push_back(argv[index]);
  }
  arguments.push_back(nullptr);

  if (!configure_interposer_environment()) {
    close(launch_image);
    return fail("configure KWin GPU bridge environment");
  }
  std::fprintf(stderr, "vibeshine-kwin-launcher: starting verified capability-free KWin shadow\n");
  fexecve(launch_image, arguments.data(), environ);
  close(launch_image);
  return fail("execute KWin shadow");
}
