/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _VIBESHINE_DRM_COMPAT_H_
#define _VIBESHINE_DRM_COMPAT_H_

#include <linux/version.h>

/* Include this only after the translation unit's Linux and DRM headers.
 * Linux 7.2 renamed the DRM atomic transaction type without changing the
 * callback contract used by VKMS. Keep the imported 7.2 source compatible
 * with the 7.1 DRM API shipped by current Arch Linux kernels without
 * rewriting the drm_atomic_commit() function declared by those headers.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 2, 0)
#define drm_atomic_commit drm_atomic_state
#endif

#endif /* _VIBESHINE_DRM_COMPAT_H_ */
