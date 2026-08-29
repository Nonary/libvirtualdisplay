#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

int fail(const char *message) {
  std::fprintf(stderr, "vibeshine-vkms-peercred: %s: %s\n", message, std::strerror(errno));
  return 1;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2 || argv[1][0] != '/') {
    std::fputs("vibeshine-vkms-peercred: an absolute broker path is required\n", stderr);
    return 2;
  }

  sockaddr_storage local_address {};
  socklen_t local_address_size = sizeof(local_address);
  if (getsockname(STDIN_FILENO, reinterpret_cast<sockaddr *>(&local_address),
                  &local_address_size) != 0) {
    return fail("standard input is not a socket");
  }
  if (local_address.ss_family != AF_UNIX) {
    std::fputs("vibeshine-vkms-peercred: standard input is not an AF_UNIX socket\n", stderr);
    return 1;
  }

  ucred peer {};
  socklen_t peer_size = sizeof(peer);
  if (getsockopt(STDIN_FILENO, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0) {
    return fail("could not obtain peer credentials");
  }
  if (peer_size != sizeof(peer) || peer.pid <= 0 || peer.uid == static_cast<uid_t>(-1)) {
    std::fputs("vibeshine-vkms-peercred: incomplete peer credentials\n", stderr);
    return 1;
  }

  std::string peer_uid = std::to_string(peer.uid);
  std::vector<char *> broker_argv;
  broker_argv.reserve(static_cast<std::size_t>(argc) + 1);
  for (int index = 1; index < argc; ++index) {
    broker_argv.push_back(argv[index]);
  }
  broker_argv.push_back(peer_uid.data());
  broker_argv.push_back(nullptr);
  execv(argv[1], broker_argv.data());
  return fail("could not execute the control broker");
}
