/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef VIBESHINE_DRM_CAPTURE_H
#define VIBESHINE_DRM_CAPTURE_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#endif
#define VIBESHINE_CAPTURE_MAX_READERS 4U
/* All state transitions are serialized by the output's present_lock. */
struct vibeshine_capture_state {
	unsigned int readers;
	unsigned int retiring;
};
static inline bool vibeshine_capture_acquire(struct vibeshine_capture_state *state, bool stopping)
{
	if (stopping || state->retiring || state->readers >= VIBESHINE_CAPTURE_MAX_READERS)
		return false;
	state->readers++;
	return true;
}
static inline bool vibeshine_capture_release(struct vibeshine_capture_state *state)
{
	if (!state->readers)
		return false;
	state->readers--;
	return true;
}
static inline bool vibeshine_capture_retire(struct vibeshine_capture_state *state, bool nonblock)
{
	if (state->readers && !nonblock)
		return false;
	state->retiring++;
	return true;
}
static inline bool vibeshine_capture_publish(struct vibeshine_capture_state *state)
{
	if (!state->retiring)
		return false;
	state->retiring--;
	return true;
}
#endif
