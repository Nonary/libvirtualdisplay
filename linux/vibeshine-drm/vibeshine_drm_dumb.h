/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _VIBESHINE_DRM_DUMB_H_
#define _VIBESHINE_DRM_DUMB_H_

#include "vibeshine_drm_compat.h"

struct drm_file;
struct drm_device;
struct drm_mode_create_dumb;

int vkms_dumb_create(struct drm_file *file, struct drm_device *dev,
		     struct drm_mode_create_dumb *args);

/* Shared registration for the module and the production-component tests. */
#define VIBESHINE_DRM_SHMEM_DRIVER_OPS \
	VIBESHINE_DRM_SHMEM_PRIME_DRIVER_OPS, \
	.dumb_create = vkms_dumb_create

#endif /* _VIBESHINE_DRM_DUMB_H_ */
