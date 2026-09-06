/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */

#ifndef _UAPI_VIBESHINE_DRM_H_
#define _UAPI_VIBESHINE_DRM_H_

#include <linux/types.h>

/* Driver-private DRM command indices start at DRM_COMMAND_BASE. */
#define DRM_VIBESHINE_WAIT_PRESENT 0x00
#define DRM_VIBESHINE_GET_FRAME 0x01
#define DRM_VIBESHINE_GET_PRESENT_TRACE 0x02

#define VIBESHINE_DRM_PRESENT_ABI_VERSION 1
#define VIBESHINE_DRM_PRESENT_MAX_TIMEOUT_MS 1000

#define VIBESHINE_DRM_PRESENT_CHANGED (1U << 0)
#define VIBESHINE_DRM_PRESENT_TIMEOUT (1U << 1)
#define VIBESHINE_DRM_PRESENT_PENDING (1U << 2)

#define VIBESHINE_DRM_FRAME_ABI_VERSION 1
#define VIBESHINE_DRM_FRAME_MAX_PLANES 4

#define VIBESHINE_DRM_FRAME_READY (1U << 0)
#define VIBESHINE_DRM_FRAME_EMPTY (1U << 1)

#define VIBESHINE_DRM_TRACE_ABI_VERSION 1
#define VIBESHINE_DRM_TRACE_MAX_EVENTS 64
#define VIBESHINE_DRM_TRACE_OVERFLOW (1U << 0)

struct vibeshine_drm_trace_event {
	__u64 sequence;
	__u64 timestamp_ns;
};

/**
 * struct vibeshine_drm_present_trace - drain completed presentation history
 * @abi_version: must be VIBESHINE_DRM_TRACE_ABI_VERSION
 * @crtc_id: DRM object ID of the target CRTC
 * @after_sequence: in: last trace sequence consumed by this caller
 * @newest_sequence: out: newest sequence present when the snapshot was taken
 * @count: out: number of valid entries in @events
 * @flags: out: VIBESHINE_DRM_TRACE_* flags
 * @events: out: ordered presentation events newer than @after_sequence
 * @reserved: must be zero
 *
 * The diagnostic history is bounded. OVERFLOW means events older than the
 * first returned entry were overwritten before the caller drained them.
 */
struct vibeshine_drm_present_trace {
	__u32 abi_version;
	__u32 crtc_id;
	__u64 after_sequence;
	__u64 newest_sequence;
	__u32 count;
	__u32 flags;
	struct vibeshine_drm_trace_event events[VIBESHINE_DRM_TRACE_MAX_EVENTS];
	__u64 reserved[4];
};

/**
 * struct vibeshine_drm_wait_present - wait for a changed CRTC scanout
 * @abi_version: must be VIBESHINE_DRM_PRESENT_ABI_VERSION
 * @crtc_id: DRM object ID of the target CRTC
 * @sequence: in: last observed sequence; out: newest sequence
 * @timestamp_ns: out: CLOCK_MONOTONIC timestamp for @sequence
 * @timeout_ms: maximum interruptible wait; zero performs a query
 * @flags: out: VIBESHINE_DRM_PRESENT_* result flags; PENDING means a newer
 *         submitted state has not completed presentation yet; it does not
 *         invalidate the completed sequence or framebuffer being returned
 * @reserved: must be zero
 *
 * A sequence advances once per completed atomic commit which can change the
 * selected CRTC's scanout. A wait returns as soon as a newer sequence completes,
 * even if another commit is pending. Multiple consumers may wait independently
 * and can coalesce missed sequences by always importing the newest framebuffer.
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

/**
 * struct vibeshine_drm_frame - export the latest completed primary scanout
 * @abi_version: must be VIBESHINE_DRM_FRAME_ABI_VERSION
 * @crtc_id: DRM object ID of the target CRTC
 * @sequence: out: presentation sequence associated with this exact framebuffer
 * @timestamp_ns: out: CLOCK_MONOTONIC presentation timestamp for @sequence
 * @flags: out: exactly one VIBESHINE_DRM_FRAME_* result flag
 * @width: out: framebuffer width in pixels
 * @height: out: framebuffer height in pixels
 * @fourcc: out: DRM framebuffer format
 * @modifier: out: DRM framebuffer modifier
 * @plane_count: out: number of valid entries in the per-plane arrays
 * @dma_buf_fds: out: close-on-exec DMA-BUF descriptors owned by the caller
 * @pitches: out: DRM framebuffer pitches
 * @offsets: out: DRM framebuffer offsets
 * @sync_file_fds: out: close-on-exec read fences per DMA-BUF plane, or -1
 *                 when presentation has already consumed a producer fence
 * @reserved_u32: must be zero
 * @reserved: must be zero
 *
 * The driver pins the framebuffer when its atomic commit completes. This ioctl
 * therefore exports one coherent descriptor: the sequence, timestamp, layout,
 * and DMA-BUFs all describe the same primary-plane presentation. Sequence gaps
 * are intentionally coalesced; callers always receive the newest completed
 * frame. Imported PRIME objects are returned as their original DMA-BUFs. The
 * ioctl fails rather than copying a virtual-device shmem object.
 *
 * On input every field except @abi_version and @crtc_id must be zero. Returned
 * file descriptors belong to the calling process and must be closed.
 */
struct vibeshine_drm_frame {
	__u32 abi_version;
	__u32 crtc_id;
	__u64 sequence;
	__u64 timestamp_ns;
	__u32 flags;
	__u32 width;
	__u32 height;
	__u32 fourcc;
	__u64 modifier;
	__u32 plane_count;
	__s32 dma_buf_fds[VIBESHINE_DRM_FRAME_MAX_PLANES];
	__u32 pitches[VIBESHINE_DRM_FRAME_MAX_PLANES];
	__u32 offsets[VIBESHINE_DRM_FRAME_MAX_PLANES];
	__s32 sync_file_fds[VIBESHINE_DRM_FRAME_MAX_PLANES];
	__u32 reserved_u32;
	__u64 reserved[4];
};

#endif /* _UAPI_VIBESHINE_DRM_H_ */
