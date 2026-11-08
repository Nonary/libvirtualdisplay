/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef VKMS_FRAME_FDS_H
#define VKMS_FRAME_FDS_H

#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fcntl.h>

#define VKMS_FRAME_MAX_FDS 8

/* Reserved descriptors are invisible until the complete reply reaches userspace. */
struct vkms_frame_fds {
	struct file *files[VKMS_FRAME_MAX_FDS];
	int numbers[VKMS_FRAME_MAX_FDS];
	unsigned int count;
};

static inline int vkms_frame_reserve_fd(struct vkms_frame_fds *fds,
				      struct file *file)
{
	int number;

	if (fds->count == VKMS_FRAME_MAX_FDS)
		return -EOVERFLOW;
	number = get_unused_fd_flags(O_CLOEXEC);
	if (number < 0)
		return number;
	fds->files[fds->count] = get_file(file);
	fds->numbers[fds->count++] = number;
	return number;
}

static inline void vkms_frame_finish_fds(struct vkms_frame_fds *fds, bool install)
{
	unsigned int index;

	for (index = 0; index < fds->count; ++index) {
		if (install) {
			fd_install(fds->numbers[index], fds->files[index]);
		} else {
			put_unused_fd(fds->numbers[index]);
			fput(fds->files[index]);
		}
	}
	fds->count = 0;
}

#endif
