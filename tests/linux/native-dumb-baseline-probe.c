// SPDX-License-Identifier: GPL-2.0+
/* Compile the complete, unmodified historical driver with real kernel headers.
 * Only expose its private registered allocation callback to the test process.
 */
#include "vkms_drv.c"

int vibeshine_baseline_dumb_create(struct drm_file *, struct drm_device *,
                                  struct drm_mode_create_dumb *);

int vibeshine_baseline_dumb_create(struct drm_file *file, struct drm_device *dev,
                                  struct drm_mode_create_dumb *args)
{
    return vkms_driver.dumb_create(file, dev, args);
}
