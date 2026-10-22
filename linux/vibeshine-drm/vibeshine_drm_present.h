/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _VIBESHINE_DRM_PRESENT_H_
#define _VIBESHINE_DRM_PRESENT_H_

#ifndef __KERNEL__
#include <stdbool.h>
#endif

#include "vibeshine_drm_uapi.h"

#define VIBESHINE_DRM_PRESENT_MAX_WAITERS 64U

/* Per-commit timing, never the output's mutable last-vblank timestamp. */
struct vibeshine_drm_present_timing {
	__u64 timestamp_ns;
	bool pending;
};

static inline void
vibeshine_drm_present_timing_arm(struct vibeshine_drm_present_timing *timing)
{
	timing->timestamp_ns = 0;
	timing->pending = true;
}

static inline bool
vibeshine_drm_present_timing_complete(struct vibeshine_drm_present_timing *timing,
				    bool flip_complete, __u64 event_timestamp_ns)
{
	if (!timing->pending || !flip_complete || !event_timestamp_ns)
		return false;
	timing->timestamp_ns = event_timestamp_ns;
	timing->pending = false;
	return true;
}

static inline __u64
vibeshine_drm_present_timing_finish(struct vibeshine_drm_present_timing *timing,
				  __u64 fallback_timestamp_ns)
{
	/* Disarm even on a flip timeout; a later event must not alter this frame. */
	timing->pending = false;
	return timing->timestamp_ns ? timing->timestamp_ns : fallback_timestamp_ns;
}

enum vibeshine_drm_present_wait_decision_e {
	VIBESHINE_DRM_PRESENT_RETURN_CURRENT,
	VIBESHINE_DRM_PRESENT_REGISTER_WAITER,
	VIBESHINE_DRM_PRESENT_REJECT_BUSY,
};

static inline bool
vibeshine_drm_present_request_valid(const struct vibeshine_drm_wait_present *request)
{
	return request->abi_version == VIBESHINE_DRM_PRESENT_ABI_VERSION &&
	       request->timeout_ms <= VIBESHINE_DRM_PRESENT_MAX_TIMEOUT_MS &&
	       !request->reserved[0] && !request->reserved[1];
}

static inline enum vibeshine_drm_present_wait_decision_e
vibeshine_drm_present_decide_wait(const struct vibeshine_drm_wait_present *request,
				  __u64 current_sequence,
				  unsigned int waiter_count)
{
	/*
	 * A completed sequence is immediately consumable even when a newer atomic
	 * commit is pending. The exported framebuffer is pinned at completion, so
	 * waiting for the entire producer queue to drain only turns closely spaced
	 * presentations into avoidable frame gaps.
	 */
	if (!request->timeout_ms || current_sequence != request->sequence)
		return VIBESHINE_DRM_PRESENT_RETURN_CURRENT;
	if (waiter_count >= VIBESHINE_DRM_PRESENT_MAX_WAITERS)
		return VIBESHINE_DRM_PRESENT_REJECT_BUSY;
	return VIBESHINE_DRM_PRESENT_REGISTER_WAITER;
}

static inline void
vibeshine_drm_present_complete_response(struct vibeshine_drm_wait_present *request,
					__u64 requested_sequence,
					__u64 current_sequence,
					__u64 timestamp_ns,
					bool presentation_pending)
{
	request->sequence = current_sequence;
	request->timestamp_ns = timestamp_ns;
	request->flags = current_sequence != requested_sequence ?
		VIBESHINE_DRM_PRESENT_CHANGED : VIBESHINE_DRM_PRESENT_TIMEOUT;
	if (presentation_pending)
		request->flags |= VIBESHINE_DRM_PRESENT_PENDING;
}

#endif /* _VIBESHINE_DRM_PRESENT_H_ */
