/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _VIBESHINE_DRM_MODE_H_
#define _VIBESHINE_DRM_MODE_H_

#include <linux/types.h>

#define VIBESHINE_DRM_MIN_REFRESH_MILLIHZ 1000U
#define VIBESHINE_DRM_MAX_REFRESH_MILLIHZ 1000000U

struct vibeshine_drm_requested_mode {
	__u32 width;
	__u32 height;
	__u32 refresh_millihz;
};

static inline int vibeshine_drm_requested_mode_valid(
	const struct vibeshine_drm_requested_mode *mode)
{
	return mode->width >= 64 && mode->width <= 8192 &&
	       mode->height >= 64 && mode->height <= 8192 &&
	       mode->refresh_millihz >= VIBESHINE_DRM_MIN_REFRESH_MILLIHZ &&
	       mode->refresh_millihz <= VIBESHINE_DRM_MAX_REFRESH_MILLIHZ;
}

static inline int vibeshine_drm_doubled_mode(
	const struct vibeshine_drm_requested_mode *mode,
	struct vibeshine_drm_requested_mode *doubled)
{
	if (!vibeshine_drm_requested_mode_valid(mode) ||
	    mode->refresh_millihz > VIBESHINE_DRM_MAX_REFRESH_MILLIHZ / 2)
		return 0;

	*doubled = *mode;
	doubled->refresh_millihz *= 2;
	return 1;
}

/* This virtual connector has no physical timing restriction. Fixed blanking
 * preserves exact client dimensions, unlike CVT's width alignment. Round only
 * the kHz pixel clock; the worst refresh error within the bounds is 0.021 Hz.
 */
static inline __u32 vibeshine_drm_requested_mode_clock_khz(
	const struct vibeshine_drm_requested_mode *mode)
{
	__u64 pixels = (__u64)(mode->width + 160) * (mode->height + 45);

	return (pixels * mode->refresh_millihz + 500000) / 1000000;
}

#endif
