/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _VIBESHINE_DRM_COMPAT_H_
#define _VIBESHINE_DRM_COMPAT_H_

#include <linux/version.h>
#include <linux/slab.h>

/*
 * Vibeshine tracks current VKMS, while the packaged driver also supports the
 * Linux 6.16 DRM API used by SteamOS 3.8. Keep compatibility decisions here
 * so that the imported code stays readable and each kernel-era fallback has
 * one documented boundary.
 *
 * Include this only after a translation unit's Linux and DRM headers. Linux
 * 7.2 renamed the DRM atomic transaction type without changing the callback
 * contract used by VKMS.
 */
#define VIBESHINE_DRM_MIN_KERNEL_VERSION KERNEL_VERSION(6, 16, 0)

/* Full color-pipeline and programmable background support use the 7.1 API. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0)
#define VIBESHINE_DRM_HAS_COLOR_PIPELINE 1
#define VIBESHINE_DRM_HAS_BACKGROUND_COLOR 1
#else
#define VIBESHINE_DRM_HAS_COLOR_PIPELINE 0
#define VIBESHINE_DRM_HAS_BACKGROUND_COLOR 0
#endif

/* Vibeshine's VRR-aware DRM vblank-helper path requires Linux 7.0. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#define VIBESHINE_DRM_HAS_VBLANK_HELPER 1
#define VIBESHINE_DRM_HAS_SHARPNESS_STRENGTH 1
#else
#define VIBESHINE_DRM_HAS_VBLANK_HELPER 0
#define VIBESHINE_DRM_HAS_SHARPNESS_STRENGTH 0
#endif

/* The typed zero-allocation helpers were added in Linux 7.0. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 0, 0)
#ifndef kzalloc_obj
#define kzalloc_obj(P, ...) kzalloc(sizeof(P), GFP_KERNEL)
#endif
#ifndef kzalloc_objs
#define kzalloc_objs(P, COUNT, ...) kcalloc((COUNT), sizeof(P), GFP_KERNEL)
#endif
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 2, 0)
#define drm_atomic_commit drm_atomic_state
#endif

#endif /* _VIBESHINE_DRM_COMPAT_H_ */
