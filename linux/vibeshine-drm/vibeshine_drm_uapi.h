/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */

#ifndef _UAPI_VIBESHINE_DRM_H_
#define _UAPI_VIBESHINE_DRM_H_

#include <linux/types.h>

/* Driver-private DRM command indices start at DRM_COMMAND_BASE. */
#define DRM_VIBESHINE_WAIT_PRESENT 0x00

#define VIBESHINE_DRM_PRESENT_ABI_VERSION 1
#define VIBESHINE_DRM_PRESENT_MAX_TIMEOUT_MS 1000

#define VIBESHINE_DRM_PRESENT_CHANGED (1U << 0)
#define VIBESHINE_DRM_PRESENT_TIMEOUT (1U << 1)
#define VIBESHINE_DRM_PRESENT_PENDING (1U << 2)

/**
 * struct vibeshine_drm_wait_present - wait for a changed CRTC scanout
 * @abi_version: must be VIBESHINE_DRM_PRESENT_ABI_VERSION
 * @crtc_id: DRM object ID of the target CRTC
 * @sequence: in: last observed sequence; out: newest sequence
 * @timestamp_ns: out: CLOCK_MONOTONIC timestamp for @sequence
 * @timeout_ms: maximum interruptible wait; zero performs a query
 * @flags: out: VIBESHINE_DRM_PRESENT_* result flags; PENDING means a newer
 *         submitted state has not completed presentation yet
 * @reserved: must be zero
 *
 * A sequence advances once per completed atomic commit which can change the
 * selected CRTC's scanout. Multiple consumers may wait independently and can
 * coalesce missed sequences by always importing the newest framebuffer.
 */
struct vibeshine_drm_wait_present {
	__u32 abi_version;
	__u32 crtc_id;
	__u64 sequence;
	__u64 timestamp_ns;
	__u32 timeout_ms;
	__u32 flags;
	__u64 reserved[2];
};

#endif /* _UAPI_VIBESHINE_DRM_H_ */
