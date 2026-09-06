/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _VIBESHINE_DRM_VRR_H_
#define _VIBESHINE_DRM_VRR_H_

/*
 * Return the earliest legal presentation time for a VRR output whose mode
 * period is also its maximum refresh-rate limit.  A zero previous timestamp
 * permits the first presentation immediately.  Saturating the addition keeps
 * a corrupt/stale timestamp from wrapping into the past.
 */
static inline unsigned long long
vibeshine_drm_vrr_presentation_deadline_ns(unsigned long long previous_ns,
					  unsigned long long now_ns,
					  unsigned long long period_ns)
{
	unsigned long long deadline_ns;

	if (!previous_ns || !period_ns)
		return now_ns;
	if (previous_ns > ~0ULL - period_ns)
		return ~0ULL;

	deadline_ns = previous_ns + period_ns;
	return deadline_ns > now_ns ? deadline_ns : now_ns;
}

#endif /* _VIBESHINE_DRM_VRR_H_ */
