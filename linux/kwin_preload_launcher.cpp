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
  constexpr char kInterposerPath[] = VIBESHINE_KWIN_GPU_LIBRARY_PATH;
  constexpr char kInterposerMarker[] = "vibeshine_kwin_gpu_interposer_abi";
  constexpr char kPreloadActiveEnvironment[] = "VIBESHINE_KWIN_GPU_PRELOAD_ACTIVE";
  constexpr char kParentPreloadEnvironment[] = "VIBESHINE_KWIN_PARENT_LD_PRELOAD";
  constexpr unsigned int kInterposerAbi = 3;
  constexpr off_t kMaximumKwinSize = 256 * 1024 * 1024;

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
    const auto *parent_preload_value = std::getenv("LD_PRELOAD");
    const std::string parent_preload = parent_preload_value ? parent_preload_value : "";
    if (!parent_preload.empty()) {
      if (setenv(kParentPreloadEnvironment, parent_preload.c_str(), 1) < 0) {
        return false;
      }
    } else {
      (void)unsetenv(kParentPreloadEnvironment);
    }

    std::string preload {kInterposerPath};
    if (!parent_preload.empty()) {
      preload.push_back(' ');
      preload.append(parent_preload);
    }
    return setenv(kPreloadActiveEnvironment, "1", 1) == 0 &&
      setenv("LD_PRELOAD", preload.c_str(), 1) == 0;
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
}

int main(const int argc, char **argv) {
  if (argc < 1 || !argv) {
    return fail_message("invalid argument vector");
  }
  if (!interposer_is_ready()) {
    return fail_message("required Vibeshine KWin GPU bridge is missing, unsafe, or has an incompatible ABI");
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
  const auto shadow_path = shadow_directory + "/kwin_wayland";
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
  close(executable);
  if (rename(temporary_name.data(), shadow_path.c_str()) < 0) {
    const auto saved_errno = errno;
    unlink(temporary_name.data());
    errno = saved_errno;
    return fail("publish KWin shadow");
  }

  std::vector<char *> arguments;
  arguments.reserve(static_cast<std::size_t>(argc) + 1);
  arguments.push_back(const_cast<char *>(kSystemKwin));
  for (int index = 1; index < argc; ++index) {
    arguments.push_back(argv[index]);
  }
  arguments.push_back(nullptr);

  if (!configure_interposer_environment()) {
    return fail("configure KWin GPU bridge environment");
  }
  std::fprintf(stderr, "vibeshine-kwin-launcher: starting verified capability-free KWin shadow\n");
  execve(shadow_path.c_str(), arguments.data(), environ);
  return fail("execute KWin shadow");
}
