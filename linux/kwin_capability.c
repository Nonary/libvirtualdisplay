#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

static const char kwin_path[] = "/usr/bin/kwin_wayland";
static const char marker_name[] = "user.vibeshine.cap_sys_nice_removed";
static const char marker_value[] = "1";

enum capability_state {
  capability_error = -1,
  capability_empty,
  capability_sys_nice,
  capability_unexpected,
};

static int fail(const char *operation) {
  fprintf(stderr, "vibeshine-kwin-capability: %s: %s\n", operation, strerror(errno));
  return 1;
}

static int fail_message(const char *message) {
  fprintf(stderr, "vibeshine-kwin-capability: %s\n", message);
  return 1;
}

static int open_trusted_kwin(struct stat *metadata) {
  const int fd = open(kwin_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return -1;
  }
  if (fstat(fd, metadata) < 0 || !S_ISREG(metadata->st_mode) ||
      metadata->st_uid != 0 || (metadata->st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
      (metadata->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
    close(fd);
    errno = EPERM;
    return -1;
  }
  return fd;
}

static enum capability_state get_capability_state(int fd, cap_t *actual_out,
                                                   cap_t *empty_out, cap_t *sys_nice_out) {
  cap_t actual = cap_get_fd(fd);
  cap_t empty = cap_init();
  cap_t sys_nice = cap_from_text("cap_sys_nice=ep");
  if (!actual || !empty || !sys_nice) {
    if (actual) {
      cap_free(actual);
    }
    if (empty) {
      cap_free(empty);
    }
    if (sys_nice) {
      cap_free(sys_nice);
    }
    return capability_error;
  }

  *actual_out = actual;
  *empty_out = empty;
  *sys_nice_out = sys_nice;
  if (cap_compare(actual, empty) == 0) {
    return capability_empty;
  }
  if (cap_compare(actual, sys_nice) == 0) {
    return capability_sys_nice;
  }
  return capability_unexpected;
}

static void free_capabilities(cap_t actual, cap_t empty, cap_t sys_nice) {
  cap_free(actual);
  cap_free(empty);
  cap_free(sys_nice);
}

static int marker_exists(int fd) {
  char value[sizeof(marker_value)] = {0};
  const ssize_t length = fgetxattr(fd, marker_name, value, sizeof(value));
  if (length < 0) {
    return errno == ENODATA ? 0 : -1;
  }
  if (length != (ssize_t)(sizeof(marker_value) - 1) ||
      memcmp(value, marker_value, sizeof(marker_value) - 1) != 0) {
    errno = EINVAL;
    return -1;
  }
  return 1;
}

static int prepare_kwin(void) {
  struct stat metadata = {0};
  const int fd = open_trusted_kwin(&metadata);
  if (fd < 0) {
    if (errno == ENOENT) {
      return 0;
    }
    return fail("verify /usr/bin/kwin_wayland");
  }

  cap_t actual = NULL;
  cap_t empty = NULL;
  cap_t sys_nice = NULL;
  const enum capability_state state = get_capability_state(fd, &actual, &empty, &sys_nice);
  if (state == capability_error) {
    close(fd);
    return fail("read KWin file capabilities");
  }
  if (state == capability_empty) {
    free_capabilities(actual, empty, sys_nice);
    close(fd);
    return 0;
  }
  if (state == capability_unexpected) {
    char *text = cap_to_text(actual, NULL);
    fprintf(stderr, "vibeshine-kwin-capability: refusing unexpected KWin capabilities: %s\n",
            text ? text : "<unavailable>");
    if (text) {
      cap_free(text);
    }
    free_capabilities(actual, empty, sys_nice);
    close(fd);
    return 1;
  }

  if (fsetxattr(fd, marker_name, marker_value, sizeof(marker_value) - 1, 0) < 0) {
    free_capabilities(actual, empty, sys_nice);
    close(fd);
    return fail("record original KWin capability");
  }
  if (cap_set_fd(fd, empty) < 0) {
    const int saved_errno = errno;
    fremovexattr(fd, marker_name);
    free_capabilities(actual, empty, sys_nice);
    close(fd);
    errno = saved_errno;
    return fail("remove optional CAP_SYS_NICE from KWin");
  }

  free_capabilities(actual, empty, sys_nice);
  close(fd);
  fprintf(stderr, "vibeshine-kwin-capability: removed optional CAP_SYS_NICE from KWin\n");
  return 0;
}

static int restore_kwin(void) {
  struct stat metadata = {0};
  const int fd = open_trusted_kwin(&metadata);
  if (fd < 0) {
    if (errno == ENOENT) {
      return 0;
    }
    return fail("verify /usr/bin/kwin_wayland");
  }
  const int has_marker = marker_exists(fd);
  if (has_marker < 0) {
    close(fd);
    return fail("read KWin capability marker");
  }
  if (!has_marker) {
    close(fd);
    return 0;
  }

  cap_t actual = NULL;
  cap_t empty = NULL;
  cap_t sys_nice = NULL;
  const enum capability_state state = get_capability_state(fd, &actual, &empty, &sys_nice);
  if (state == capability_error) {
    close(fd);
    return fail("read KWin file capabilities");
  }
  if (state == capability_unexpected) {
    free_capabilities(actual, empty, sys_nice);
    close(fd);
    return fail_message("refusing to overwrite unexpected KWin capabilities");
  }
  if (state == capability_empty && cap_set_fd(fd, sys_nice) < 0) {
    const int saved_errno = errno;
    free_capabilities(actual, empty, sys_nice);
    close(fd);
    errno = saved_errno;
    return fail("restore KWin CAP_SYS_NICE");
  }

  if (fremovexattr(fd, marker_name) < 0) {
    free_capabilities(actual, empty, sys_nice);
    close(fd);
    return fail("remove KWin capability marker");
  }
  free_capabilities(actual, empty, sys_nice);
  close(fd);
  fprintf(stderr, "vibeshine-kwin-capability: restored KWin CAP_SYS_NICE\n");
  return 0;
}

int main(int argc, char **argv) {
  if (geteuid() != 0) {
    return fail_message("must run as root");
  }
  if (argc != 2) {
    return fail_message("usage: vibeshine-kwin-capability prepare|restore");
  }
  if (strcmp(argv[1], "prepare") == 0) {
    return prepare_kwin();
  }
  if (strcmp(argv[1], "restore") == 0) {
    return restore_kwin();
  }
  return fail_message("usage: vibeshine-kwin-capability prepare|restore");
}
