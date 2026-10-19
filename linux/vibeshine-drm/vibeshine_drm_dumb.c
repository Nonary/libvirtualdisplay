// SPDX-License-Identifier: GPL-2.0+

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/overflow.h>

#include <drm/drm_gem_shmem_helper.h>

#include "vibeshine_drm_dumb.h"

int vkms_dumb_create(struct drm_file *file, struct drm_device *dev,
			    struct drm_mode_create_dumb *args)
{
	struct drm_mode_create_dumb allocation = *args;
	u32 pitch_bits, size;
	int ret;

	/* KWin's native software scanout uses 32-bit RGB dumb buffers. AMD's
	 * linear DMA-BUF import needs 256-byte rows on GFX9-11 and 128 on GFX12.
	 * Pad the private allocation, not the logical dimensions returned to the
	 * compositor. CREATE_DUMB's pitch then governs both row writes and export.
	 * Other pixel depths retain the kernel helper's existing behavior.
	 */
	if (args->bpp != 32)
		return drm_gem_shmem_dumb_create(file, dev, args);

	if (!args->width || !args->height ||
	    check_add_overflow(args->width, 63U, &allocation.width))
		return -EINVAL;
	allocation.width &= ~63U;

	/* Linux 6.16's shmem helper computes width * bpp and pitch * height
	 * in u32. Recheck those bounds after padding, including PAGE_ALIGN's
	 * addition, before asking any supported kernel helper to allocate.
	 */
	if (check_mul_overflow(allocation.width, args->bpp, &pitch_bits) ||
	    check_mul_overflow(pitch_bits / 8, args->height, &size) ||
	    size > U32_MAX - (PAGE_SIZE - 1))
		return -EINVAL;

	allocation.pitch = 0;
	allocation.size = 0;
	allocation.handle = 0;
	ret = drm_gem_shmem_dumb_create(file, dev, &allocation);
	if (ret)
		return ret;

	args->pitch = allocation.pitch;
	args->size = allocation.size;
	args->handle = allocation.handle;
	return 0;
}
