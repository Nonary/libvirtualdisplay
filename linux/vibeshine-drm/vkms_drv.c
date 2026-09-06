// SPDX-License-Identifier: GPL-2.0+

/**
 * DOC: vkms (Virtual Kernel Modesetting)
 *
 * VKMS is a software-only model of a KMS driver that is useful for testing
 * and for running X (or similar) on headless machines. VKMS aims to enable
 * a virtual display with no need of a hardware display capability, releasing
 * the GPU in DRM API tests.
 */

#include <linux/module.h>
#include <linux/capability.h>
#include <linux/device/faux.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/dma-resv.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/hrtimer.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/panic.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/sync_file.h>
#include <linux/timer.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_gem.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/drm_prime.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_vblank.h>

#include "vkms_config.h"
#include "vkms_configfs.h"
#include "vkms_drv.h"
#include "vibeshine_drm_change.h"
#include "vibeshine_drm_present.h"
#include "vibeshine_drm_vrr.h"
#include "vibeshine_drm_uapi.h"
#include "vibeshine_drm_compat.h"
#include "vibeshine_drm_version.h"

#define DRIVER_NAME	"vibeshine_drm"
#define DRIVER_DESC	"Vibeshine HDR Virtual Display"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	5

static struct vkms_config *default_config;

static bool enable_cursor;
module_param_named(enable_cursor, enable_cursor, bool, 0444);
MODULE_PARM_DESC(enable_cursor, "Enable/Disable cursor support");

static bool enable_writeback;
module_param_named(enable_writeback, enable_writeback, bool, 0444);
MODULE_PARM_DESC(enable_writeback, "Enable/Disable writeback connector support");

static bool enable_overlay;
module_param_named(enable_overlay, enable_overlay, bool, 0444);
MODULE_PARM_DESC(enable_overlay, "Enable/Disable overlay support");

static bool enable_plane_pipeline;
module_param_named(enable_plane_pipeline, enable_plane_pipeline, bool, 0444);
MODULE_PARM_DESC(enable_plane_pipeline, "Enable/Disable plane pipeline support");

static bool create_default_dev = true;
module_param_named(create_default_dev, create_default_dev, bool, 0444);
MODULE_PARM_DESC(create_default_dev, "Create or not the default VKMS device");

/*
 * Seconds a power-off or halt may spend in the kernel's device walk after the
 * reboot notifier before it is bugchecked into a restart. A restart never
 * waits at all; see vkms_reboot_notifier(). Filesystems are already synced and
 * unmounted or read-only at that point, so this only bounds the walk itself.
 */
#define VKMS_POWEROFF_DEADLINE_SECS 5U

DEFINE_DRM_GEM_FOPS(vkms_driver_fops);

#define DRM_IOCTL_VIBESHINE_WAIT_PRESENT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_VIBESHINE_WAIT_PRESENT, \
		 struct vibeshine_drm_wait_present)
#define DRM_IOCTL_VIBESHINE_GET_FRAME \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_VIBESHINE_GET_FRAME, \
		 struct vibeshine_drm_frame)
#define DRM_IOCTL_VIBESHINE_GET_PRESENT_TRACE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_VIBESHINE_GET_PRESENT_TRACE, \
		 struct vibeshine_drm_present_trace)

static int vkms_wait_present_ioctl(struct drm_device *dev, void *data,
				   struct drm_file *file_priv)
{
	struct vibeshine_drm_wait_present *request = data;
	struct vkms_output *output;
	struct drm_crtc *crtc;
	u64 requested_sequence;
	bool waiter_registered = false;
	long wait_result = 0;

	if (!vibeshine_drm_present_request_valid(request))
		return -EINVAL;
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	crtc = drm_crtc_find(dev, file_priv, request->crtc_id);
	if (!crtc)
		return -ENOENT;

	output = drm_crtc_to_vkms_output(crtc);
	requested_sequence = request->sequence;
	request->flags = 0;
	request->timestamp_ns = 0;

	spin_lock_irq(&output->present_lock);
	switch (vibeshine_drm_present_decide_wait(
		request, atomic64_read(&output->present_sequence),
		output->present_waiters)) {
	case VIBESHINE_DRM_PRESENT_REJECT_BUSY:
			spin_unlock_irq(&output->present_lock);
			return -EBUSY;
	case VIBESHINE_DRM_PRESENT_REGISTER_WAITER:
		output->present_waiters++;
		waiter_registered = true;
		break;
	case VIBESHINE_DRM_PRESENT_RETURN_CURRENT:
		break;
	}
	spin_unlock_irq(&output->present_lock);

	if (waiter_registered) {
		wait_result = wait_event_interruptible_timeout(
			output->present_waitq,
			atomic64_read(&output->present_sequence) != requested_sequence,
			msecs_to_jiffies(request->timeout_ms));

		spin_lock_irq(&output->present_lock);
		output->present_waiters--;
		spin_unlock_irq(&output->present_lock);

		if (wait_result < 0)
			return wait_result;
	}

	spin_lock_irq(&output->present_lock);
	vibeshine_drm_present_complete_response(
		request, requested_sequence,
		atomic64_read(&output->present_sequence),
		output->present_timestamp_ns,
		atomic_read(&output->pending_commits) != 0);
	spin_unlock_irq(&output->present_lock);

	return 0;
}

static bool vkms_frame_request_valid(const struct vibeshine_drm_frame *request)
{
	struct vibeshine_drm_frame expected = {
		.abi_version = VIBESHINE_DRM_FRAME_ABI_VERSION,
		.crtc_id = request->crtc_id,
	};

	return !memcmp(request, &expected, sizeof(expected));
}

static bool
vkms_present_trace_request_valid(const struct vibeshine_drm_present_trace *request)
{
	struct vibeshine_drm_present_trace expected = {
		.abi_version = VIBESHINE_DRM_TRACE_ABI_VERSION,
		.crtc_id = request->crtc_id,
		.after_sequence = request->after_sequence,
	};

	return !memcmp(request, &expected, sizeof(expected));
}

static int vkms_get_present_trace_ioctl(struct drm_device *dev, void *data,
					struct drm_file *file_priv)
{
	struct vibeshine_drm_present_trace *request = data;
	struct vkms_output *output;
	struct drm_crtc *crtc;
	u64 current_sequence;
	u64 first_sequence;
	u64 sequence;

	if (!vkms_present_trace_request_valid(request))
		return -EINVAL;
	if (request->after_sequence == U64_MAX)
		return -EINVAL;
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	crtc = drm_crtc_find(dev, file_priv, request->crtc_id);
	if (!crtc)
		return -ENOENT;

	output = drm_crtc_to_vkms_output(crtc);
	spin_lock_irq(&output->present_lock);
	current_sequence = atomic64_read(&output->present_sequence);
	first_sequence = request->after_sequence + 1;
	if (current_sequence >= VIBESHINE_DRM_TRACE_HISTORY_SIZE) {
		u64 oldest_sequence = current_sequence - VIBESHINE_DRM_TRACE_HISTORY_SIZE + 1;

		if (first_sequence < oldest_sequence) {
			first_sequence = oldest_sequence;
			request->flags |= VIBESHINE_DRM_TRACE_OVERFLOW;
		}
	}

	request->newest_sequence = current_sequence;
	for (sequence = first_sequence;
	     sequence <= current_sequence &&
	     request->count < VIBESHINE_DRM_TRACE_MAX_EVENTS;
	     ++sequence) {
		const struct vibeshine_drm_trace_event *event =
			&output->present_trace[(sequence - 1) %
					       VIBESHINE_DRM_TRACE_HISTORY_SIZE];

		if (event->sequence != sequence) {
			request->flags |= VIBESHINE_DRM_TRACE_OVERFLOW;
			continue;
		}
		request->events[request->count++] = *event;
	}
	spin_unlock_irq(&output->present_lock);

	return 0;
}

static void vkms_close_frame_fds(struct vibeshine_drm_frame *frame)
{
	unsigned int i;

	for (i = 0; i < VIBESHINE_DRM_FRAME_MAX_PLANES; ++i) {
		if (frame->dma_buf_fds[i] >= 0) {
			close_fd(frame->dma_buf_fds[i]);
			frame->dma_buf_fds[i] = -1;
		}
	}
	for (i = 0; i < VIBESHINE_DRM_FRAME_MAX_PLANES; ++i) {
		if (frame->sync_file_fds[i] >= 0) {
			close_fd(frame->sync_file_fds[i]);
			frame->sync_file_fds[i] = -1;
		}
	}
}

static int vkms_export_read_fence(struct dma_buf *dma_buf)
{
	struct dma_fence *fence;
	struct sync_file *sync_file;
	int fd;
	int ret;

	ret = dma_resv_get_singleton(dma_buf->resv, DMA_RESV_USAGE_WRITE,
				     &fence);
	if (ret)
		return ret;
	if (!fence)
		return -1;

	sync_file = sync_file_create(fence);
	dma_fence_put(fence);
	if (!sync_file)
		return -ENOMEM;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(sync_file->file);
		return fd;
	}
	fd_install(fd, sync_file->file);
	return fd;
}

static struct dma_buf *vkms_get_frame_dmabuf(struct drm_file *file_priv,
					  struct drm_gem_object *obj)
{
	struct dma_buf *dma_buf;
	u32 handle;
	int ret;

	if (!obj)
		return ERR_PTR(-EINVAL);

	if (obj->import_attach) {
		dma_buf = obj->import_attach->dmabuf;
		if (!dma_buf)
			return ERR_PTR(-EINVAL);
		get_dma_buf(dma_buf);
		return dma_buf;
	}

	/*
	 * Software compositors scan out native shmem objects rather than imported
	 * GPU buffers. Use PRIME's handle path so its locking, export cache and
	 * GEM/device references also apply to these completed framebuffers. The
	 * temporary handle is private to this call; the returned DMA-BUF owns its
	 * own reference after the handle is deleted.
	 */
	ret = drm_gem_handle_create(file_priv, obj, &handle);
	if (ret)
		return ERR_PTR(ret);

	dma_buf = drm_gem_prime_handle_to_dmabuf(obj->dev, file_priv, handle,
					       DRM_CLOEXEC | DRM_RDWR);
	drm_gem_handle_delete(file_priv, handle);
	return dma_buf;
}

static int vkms_get_frame_ioctl(struct drm_device *dev, void *data,
				struct drm_file *file_priv)
{
	struct vibeshine_drm_frame *request = data;
	struct drm_framebuffer *fb = NULL;
	struct vkms_output *output;
	struct drm_crtc *crtc;
	unsigned int plane_count;
	unsigned int i;
	int ret = 0;

	if (!vkms_frame_request_valid(request))
		return -EINVAL;
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	crtc = drm_crtc_find(dev, file_priv, request->crtc_id);
	if (!crtc)
		return -ENOENT;

	output = drm_crtc_to_vkms_output(crtc);
	for (i = 0; i < VIBESHINE_DRM_FRAME_MAX_PLANES; ++i) {
		request->dma_buf_fds[i] = -1;
		request->sync_file_fds[i] = -1;
	}

	spin_lock_irq(&output->present_lock);
	request->sequence = atomic64_read(&output->present_sequence);
	request->timestamp_ns = output->present_timestamp_ns;
	if (output->present_fb) {
		fb = output->present_fb;
		drm_framebuffer_get(fb);
	}
	spin_unlock_irq(&output->present_lock);

	if (!fb) {
		request->flags = VIBESHINE_DRM_FRAME_EMPTY;
		return 0;
	}

	request->flags = VIBESHINE_DRM_FRAME_READY;
	request->width = fb->width;
	request->height = fb->height;
	request->fourcc = fb->format->format;
	request->modifier = fb->modifier;
	plane_count = fb->format->num_planes;
	if (!plane_count || plane_count > VIBESHINE_DRM_FRAME_MAX_PLANES) {
		ret = -EINVAL;
		goto out;
	}
	request->plane_count = plane_count;

	for (i = 0; i < plane_count; ++i) {
		struct drm_gem_object *obj = drm_gem_fb_get_obj(fb, i);
		struct dma_buf *dma_buf;

		dma_buf = vkms_get_frame_dmabuf(file_priv, obj);
		if (IS_ERR(dma_buf)) {
			ret = PTR_ERR(dma_buf);
			goto out;
		}

		request->dma_buf_fds[i] = dma_buf_fd(dma_buf, O_CLOEXEC);
		if (request->dma_buf_fds[i] < 0) {
			dma_buf_put(dma_buf);
			ret = request->dma_buf_fds[i];
			goto out;
		}
		request->pitches[i] = fb->pitches[i];
		request->offsets[i] = fb->offsets[i];

		/*
		 * Atomic helpers have already waited for producer fences before the
		 * presentation is published. Return a per-plane snapshot of any
		 * remaining implicit write fence for explicit-sync importers; -1 means
		 * no wait is required.
		 */
		request->sync_file_fds[i] = vkms_export_read_fence(dma_buf);
		if (request->sync_file_fds[i] < -1) {
			ret = request->sync_file_fds[i];
			request->sync_file_fds[i] = -1;
			goto out;
		}
	}

out:
	drm_framebuffer_put(fb);
	if (ret)
		vkms_close_frame_fds(request);
	return ret;
}

static const struct drm_ioctl_desc vkms_ioctls[] = {
	DRM_IOCTL_DEF_DRV(VIBESHINE_WAIT_PRESENT, vkms_wait_present_ioctl, 0),
	DRM_IOCTL_DEF_DRV(VIBESHINE_GET_FRAME, vkms_get_frame_ioctl, 0),
	DRM_IOCTL_DEF_DRV(VIBESHINE_GET_PRESENT_TRACE, vkms_get_present_trace_ioctl, 0),
};

static bool vkms_commit_touches_crtc(struct drm_atomic_commit *state,
				     struct drm_crtc *target)
{
	struct drm_connector_state *old_connector_state, *new_connector_state;
	struct drm_plane_state *old_plane_state, *new_plane_state;
	struct drm_crtc_state *new_crtc_state;
	struct drm_connector *connector;
	struct drm_plane *plane;
	struct drm_crtc *crtc;
	int i;

	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		if (crtc == target)
			return true;
	}

	for_each_oldnew_plane_in_state(state, plane, old_plane_state,
				       new_plane_state, i) {
		if ((old_plane_state && old_plane_state->crtc == target) ||
		    (new_plane_state && new_plane_state->crtc == target))
			return true;
	}

	for_each_oldnew_connector_in_state(state, connector,
					   old_connector_state,
					   new_connector_state, i) {
		if ((old_connector_state && old_connector_state->crtc == target) ||
		    (new_connector_state && new_connector_state->crtc == target))
			return true;
	}

	return false;
}

static void vkms_snapshot_plane_state(
	struct drm_plane_state *state,
	struct vibeshine_drm_plane_snapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	if (!state)
		return;

	snapshot->present = true;
	snapshot->visible = state->visible;
	snapshot->content_update =
		to_vkms_plane_state(state)->presentation_content_update ||
		state->fb_damage_clips || state->color_mgmt_changed;
	snapshot->crtc = (unsigned long)state->crtc;
	snapshot->framebuffer = (unsigned long)state->fb;
#if VIBESHINE_DRM_HAS_COLOR_PIPELINE
	snapshot->color_pipeline = (unsigned long)state->color_pipeline;
#endif
	snapshot->crtc_x = state->crtc_x;
	snapshot->crtc_y = state->crtc_y;
	snapshot->hotspot_x = state->hotspot_x;
	snapshot->hotspot_y = state->hotspot_y;
	snapshot->crtc_w = state->crtc_w;
	snapshot->crtc_h = state->crtc_h;
	snapshot->src_x = state->src_x;
	snapshot->src_y = state->src_y;
	snapshot->src_w = state->src_w;
	snapshot->src_h = state->src_h;
	snapshot->alpha = state->alpha;
	snapshot->pixel_blend_mode = state->pixel_blend_mode;
	snapshot->rotation = state->rotation;
	snapshot->zpos = state->zpos;
	snapshot->normalized_zpos = state->normalized_zpos;
	snapshot->color_encoding = state->color_encoding;
	snapshot->color_range = state->color_range;
	snapshot->scaling_filter = state->scaling_filter;
}

static void vkms_snapshot_crtc_state(
	const struct drm_crtc_state *state,
	struct vibeshine_drm_crtc_snapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	if (!state)
		return;

	snapshot->present = true;
	snapshot->enable = state->enable;
	snapshot->active = state->active;
	snapshot->color_mgmt_changed = state->color_mgmt_changed;
	snapshot->plane_mask = state->plane_mask;
	snapshot->connector_mask = state->connector_mask;
	snapshot->encoder_mask = state->encoder_mask;
	snapshot->mode_blob = (unsigned long)state->mode_blob;
	snapshot->degamma_lut = (unsigned long)state->degamma_lut;
	snapshot->ctm = (unsigned long)state->ctm;
	snapshot->gamma_lut = (unsigned long)state->gamma_lut;
#if VIBESHINE_DRM_HAS_BACKGROUND_COLOR
	snapshot->background_color = state->background_color;
#endif
	snapshot->scaling_filter = state->scaling_filter;
#if VIBESHINE_DRM_HAS_SHARPNESS_STRENGTH
	snapshot->sharpness_strength = state->sharpness_strength;
#endif
	snapshot->vrr_enabled = state->vrr_enabled;
}

static void vkms_snapshot_connector_state(
	const struct drm_connector_state *state,
	struct vibeshine_drm_connector_snapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	if (!state)
		return;

	snapshot->present = true;
	snapshot->self_refresh_aware = state->self_refresh_aware;
	snapshot->crtc = (unsigned long)state->crtc;
	snapshot->best_encoder = (unsigned long)state->best_encoder;
	snapshot->hdr_output_metadata = (unsigned long)state->hdr_output_metadata;
	snapshot->link_status = state->link_status;
	snapshot->picture_aspect_ratio = state->picture_aspect_ratio;
	snapshot->content_type = state->content_type;
	snapshot->hdcp_content_type = state->hdcp_content_type;
	snapshot->scaling_mode = state->scaling_mode;
	snapshot->content_protection = state->content_protection;
	snapshot->colorspace = state->colorspace;
	snapshot->max_requested_bpc = state->max_requested_bpc;
	snapshot->max_bpc = state->max_bpc;
	snapshot->privacy_screen_sw_state = state->privacy_screen_sw_state;
}

static bool vkms_commit_changes_crtc(struct drm_atomic_commit *state,
				     struct drm_crtc *target)
{
	struct drm_connector_state *old_connector_state, *new_connector_state;
	struct drm_plane_state *old_plane_state, *new_plane_state;
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;
	struct drm_connector *connector;
	struct drm_plane *plane;
	struct drm_crtc *crtc;
	int i;

	for_each_oldnew_crtc_in_state(state, crtc, old_crtc_state,
				      new_crtc_state, i) {
		struct vibeshine_drm_crtc_snapshot old_snapshot, new_snapshot;

		if (crtc != target)
			continue;
		vkms_snapshot_crtc_state(old_crtc_state, &old_snapshot);
		vkms_snapshot_crtc_state(new_crtc_state, &new_snapshot);
		if (vibeshine_drm_crtc_changes_scanout(&old_snapshot, &new_snapshot))
			return true;
	}

	for_each_oldnew_plane_in_state(state, plane, old_plane_state,
				       new_plane_state, i) {
		struct vibeshine_drm_plane_snapshot old_snapshot, new_snapshot;

		if ((!old_plane_state || old_plane_state->crtc != target) &&
		    (!new_plane_state || new_plane_state->crtc != target))
			continue;
		vkms_snapshot_plane_state(old_plane_state, &old_snapshot);
		vkms_snapshot_plane_state(new_plane_state, &new_snapshot);
		if (vibeshine_drm_plane_changes_scanout(&old_snapshot, &new_snapshot))
			return true;
	}

	for_each_oldnew_connector_in_state(state, connector,
					   old_connector_state,
					   new_connector_state, i) {
		struct vibeshine_drm_connector_snapshot old_snapshot, new_snapshot;

		if ((!old_connector_state || old_connector_state->crtc != target) &&
		    (!new_connector_state || new_connector_state->crtc != target))
			continue;
		vkms_snapshot_connector_state(old_connector_state, &old_snapshot);
		vkms_snapshot_connector_state(new_connector_state, &new_snapshot);
		if (vibeshine_drm_connector_changes_scanout(&old_snapshot, &new_snapshot))
			return true;
	}

	return false;
}

static void vkms_wait_for_vrr_presentation_slot(struct drm_crtc *crtc,
						 struct vkms_output *output)
{
	struct drm_vblank_crtc *vblank = drm_crtc_vblank_crtc(crtc);
	ktime_t expires;
	u64 deadline_ns;
	u64 previous_ns;
	u64 period_ns;
	u64 now_ns;

	period_ns = max_t(int, READ_ONCE(vblank->framedur_ns), 0);
	spin_lock_irq(&output->present_lock);
	previous_ns = output->present_timestamp_ns;
	spin_unlock_irq(&output->present_lock);

	now_ns = ktime_get_ns();
	deadline_ns = vibeshine_drm_vrr_presentation_deadline_ns(
		previous_ns, now_ns, period_ns);
	while (deadline_ns > now_ns) {
		expires = ns_to_ktime(deadline_ns);
		schedule_hrtimeout(&expires, HRTIMER_MODE_ABS);
		now_ns = ktime_get_ns();
	}
}

static void vkms_signal_presented_crtcs(struct drm_atomic_commit *state)
{
	struct drm_crtc *crtc;

	drm_for_each_crtc(crtc, state->dev) {
		struct vkms_output *output;
		struct drm_framebuffer *new_present_fb = NULL;
		struct drm_framebuffer *old_present_fb = NULL;
		struct drm_plane_state *primary_state;
		struct drm_crtc_state *crtc_state;
		bool scanout_changed;

		if (!vkms_commit_touches_crtc(state, crtc))
			continue;

		scanout_changed = vkms_commit_changes_crtc(state, crtc);
		output = drm_crtc_to_vkms_output(crtc);
		primary_state = drm_atomic_get_new_plane_state(state, crtc->primary);
		crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
		if (scanout_changed && crtc_state && crtc_state->active &&
		    crtc_state->vrr_enabled)
			vkms_wait_for_vrr_presentation_slot(crtc, output);
		if (primary_state && primary_state->fb && primary_state->visible) {
			new_present_fb = primary_state->fb;
			drm_framebuffer_get(new_present_fb);
		}
		spin_lock_irq(&output->present_lock);
		if (scanout_changed) {
			u64 completed_sequence;
			struct vibeshine_drm_trace_event *trace_event;

			if (primary_state || (crtc_state && !crtc_state->active)) {
				old_present_fb = output->present_fb;
				output->present_fb = new_present_fb;
				new_present_fb = NULL;
			}
			output->present_timestamp_ns = ktime_get_ns();
			completed_sequence = atomic64_inc_return(&output->present_sequence);
			trace_event = &output->present_trace[(completed_sequence - 1) %
							 VIBESHINE_DRM_TRACE_HISTORY_SIZE];
			trace_event->sequence = completed_sequence;
			trace_event->timestamp_ns = output->present_timestamp_ns;
		}
		if (WARN_ON_ONCE(atomic_read(&output->pending_commits) <= 0))
			atomic_set(&output->pending_commits, 0);
		else
			atomic_dec(&output->pending_commits);
		spin_unlock_irq(&output->present_lock);
		if (old_present_fb)
			drm_framebuffer_put(old_present_fb);
		if (new_present_fb)
			drm_framebuffer_put(new_present_fb);
		wake_up_interruptible(&output->present_waitq);
	}
}

static void vkms_update_pending_crtcs(struct drm_atomic_commit *state, bool add)
{
	struct drm_crtc *crtc;

	drm_for_each_crtc(crtc, state->dev) {
		struct vkms_output *output;

		if (!vkms_commit_touches_crtc(state, crtc))
			continue;

		output = drm_crtc_to_vkms_output(crtc);
		if (add) {
			atomic_inc(&output->pending_commits);
		} else {
			if (WARN_ON_ONCE(atomic_read(&output->pending_commits) <= 0))
				atomic_set(&output->pending_commits, 0);
			else
				atomic_dec(&output->pending_commits);
			wake_up_interruptible(&output->present_waitq);
		}
	}
}

static void vkms_mark_presentation_content_updates(struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_plane_state;
	struct drm_plane *plane;
	int i;

	/*
	 * Record explicit fences before prepare_fb. The GEM helper can attach an
	 * implicit fence later; vkms_prepare_fb() preserves that independently.
	 * Both are consumed before commit_tail decides whether scanout changed.
	 */
	for_each_new_plane_in_state(state, plane, new_plane_state, i)
		to_vkms_plane_state(new_plane_state)->presentation_content_update =
			new_plane_state->fence != NULL;
}

static void vkms_atomic_commit_tail(struct drm_atomic_commit *old_state)
{
	struct drm_device *dev = old_state->dev;
	struct drm_crtc *crtc;
	struct drm_crtc_state *old_crtc_state;
	int i;

	drm_atomic_helper_commit_modeset_disables(dev, old_state);

	drm_atomic_helper_commit_planes(dev, old_state, 0);

	drm_atomic_helper_commit_modeset_enables(dev, old_state);

	drm_atomic_helper_fake_vblank(old_state);

	drm_atomic_helper_wait_for_flip_done(dev, old_state);

	/* Keep later commits serialized until this presentation is published. */
	vkms_signal_presented_crtcs(old_state);
	drm_atomic_helper_commit_hw_done(old_state);

	for_each_old_crtc_in_state(old_state, crtc, old_crtc_state, i) {
		struct vkms_crtc_state *vkms_state = to_vkms_crtc_state(old_crtc_state);

		flush_work(&vkms_state->composer_work);
	}

	drm_atomic_helper_cleanup_planes(dev, old_state);
}

static const struct drm_driver vkms_driver = {
	.driver_features	= DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_GEM,
	.fops			= &vkms_driver_fops,
	.ioctls			= vkms_ioctls,
	.num_ioctls		= ARRAY_SIZE(vkms_ioctls),
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,

	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	.major			= DRIVER_MAJOR,
	.minor			= DRIVER_MINOR,
};

static int vkms_atomic_check(struct drm_device *dev, struct drm_atomic_commit *state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *new_crtc_state;
	int i;

	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		if (!new_crtc_state->gamma_lut || !new_crtc_state->color_mgmt_changed)
			continue;

		if (new_crtc_state->gamma_lut->length / sizeof(struct drm_color_lut *)
		    > VKMS_LUT_SIZE)
			return -EINVAL;
	}

	return drm_atomic_helper_check(dev, state);
}

static int vkms_atomic_commit(struct drm_device *dev,
			      struct drm_atomic_commit *state, bool nonblock)
{
	struct vkms_device *vkmsdev = drm_device_to_vkms_device(dev);
	int ret;

	mutex_lock(&vkmsdev->commit_lock);
	if (!vkmsdev->accepting_commits && current != vkmsdev->shutdown_owner) {
		mutex_unlock(&vkmsdev->commit_lock);
		return -ENODEV;
	}

	vkms_update_pending_crtcs(state, true);
	vkms_mark_presentation_content_updates(state);
	ret = drm_atomic_helper_commit(dev, state, nonblock);
	if (ret)
		vkms_update_pending_crtcs(state, false);
	mutex_unlock(&vkmsdev->commit_lock);

	return ret;
}

static const struct drm_mode_config_funcs vkms_mode_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = vkms_atomic_check,
	.atomic_commit = vkms_atomic_commit,
};

static const struct drm_mode_config_helper_funcs vkms_mode_config_helpers = {
	.atomic_commit_tail = vkms_atomic_commit_tail,
};

static void vkms_config_put_action(struct drm_device *dev, void *data)
{
	(void)dev;
	vkms_config_destroy(data);
}

static int vkms_modeset_init(struct vkms_device *vkmsdev)
{
	struct drm_device *dev = &vkmsdev->drm;
	int ret;

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;

	dev->mode_config.funcs = &vkms_mode_funcs;
	dev->mode_config.min_width = XRES_MIN;
	dev->mode_config.min_height = YRES_MIN;
	dev->mode_config.max_width = XRES_MAX;
	dev->mode_config.max_height = YRES_MAX;
	dev->mode_config.cursor_width = 512;
	dev->mode_config.cursor_height = 512;
	/*
	 * FIXME: There's a confusion between bpp and depth between this and
	 * fbdev helpers. We have to go with 0, meaning "pick the default",
	 * which is XRGB8888 in all cases.
	 */
	dev->mode_config.preferred_depth = 0;
	dev->mode_config.helper_private = &vkms_mode_config_helpers;

	return vkms_output_init(vkmsdev);
}

static void vkms_release_presented_frames(struct vkms_device *vkmsdev)
{
	struct drm_crtc *crtc;

	drm_for_each_crtc(crtc, &vkmsdev->drm)
		vkms_crtc_release_presented_frame(
			drm_crtc_to_vkms_output(crtc));
}

static void vkms_quiesce(struct vkms_device *vkmsdev)
{
	mutex_lock(&vkmsdev->shutdown_lock);

	mutex_lock(&vkmsdev->commit_lock);
	if (!vkmsdev->accepting_commits) {
		mutex_unlock(&vkmsdev->commit_lock);
		mutex_unlock(&vkmsdev->shutdown_lock);
		return;
	}
	vkmsdev->accepting_commits = false;
	vkmsdev->shutdown_owner = current;
	mutex_unlock(&vkmsdev->commit_lock);

	/*
	 * First drain and disable KMS while all producer devices are still alive,
	 * then drop the extra framebuffer references held for direct capture. The
	 * latter may own imported NVIDIA DMA-BUFs and must not survive into the
	 * physical GPU driver's device-shutdown phase.
	 */
	drm_atomic_helper_shutdown(&vkmsdev->drm);
	vkms_release_presented_frames(vkmsdev);

	mutex_lock(&vkmsdev->commit_lock);
	vkmsdev->shutdown_owner = NULL;
	mutex_unlock(&vkmsdev->commit_lock);

	mutex_unlock(&vkmsdev->shutdown_lock);
}

/*
 * Quiescing only stops scanout; it leaves the device registered and leaves the
 * imported producer DMA-BUFs attached. Those imports may be owned by the
 * physical GPU driver, so they have to be gone before device_shutdown() walks
 * it -- otherwise the restart wedges with the buffers still attached.
 *
 * Unplugging cannot happen from the userspace stop path, because the pool is
 * torn down concurrently with the graphical session and a compositor may still
 * own the modeset device. The reboot notifier has no such problem: it runs from
 * kernel_restart_prepare(), after every userspace process is gone and before
 * device_shutdown(), which makes it the one safe point to do the real release.
 */
static void vkms_shutdown_release(struct vkms_device *vkmsdev)
{
	vkms_quiesce(vkmsdev);

	mutex_lock(&vkmsdev->shutdown_lock);
	if (vkmsdev->unplugged) {
		mutex_unlock(&vkmsdev->shutdown_lock);
		return;
	}
	vkmsdev->unplugged = true;

	/*
	 * Drop the direct-capture framebuffer references again. A commit cannot
	 * republish one behind us -- vkms_quiesce() has already cleared
	 * accepting_commits -- but this keeps the release self-contained rather
	 * than relying on the earlier quiesce having been the one that ran.
	 */
	vkms_release_presented_frames(vkmsdev);

	/*
	 * drm_dev_unplug() sleeps in synchronize_srcu(). That is safe here: the
	 * reboot notifier chain is a blocking chain called from process context,
	 * and no drm_dev_enter() reader can still be in flight once userspace is
	 * gone. Unregistering also tears down the internal client, releasing the
	 * framebuffers it holds.
	 */
	drm_dev_unplug(&vkmsdev->drm);
	mutex_unlock(&vkmsdev->shutdown_lock);
}

static ssize_t quiesce_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct vkms_device *vkmsdev = dev_get_drvdata(dev);
	bool requested;
	int ret;

	(void)attr;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	ret = kstrtobool(buf, &requested);
	if (ret)
		return ret;
	if (!requested || !vkmsdev)
		return -EINVAL;

	DRM_INFO("quiescing virtual scanout at userspace request\n");
	vkms_quiesce(vkmsdev);
	return count;
}

static DEVICE_ATTR_WO(quiesce);

static struct attribute *vkms_device_attrs[] = {
	&dev_attr_quiesce.attr,
	NULL,
};

static const struct attribute_group vkms_device_attr_group = {
	.attrs = vkms_device_attrs,
};

/*
 * The reboot notifier is the last point at which this module runs code with
 * the system still alive. Everything after it -- device_shutdown() of the
 * physical GPU drivers in particular -- runs on PID 1 with no timeout and no
 * way back to userspace. A GPU whose firmware has already faulted (NVIDIA
 * Xid 38 followed by a locked GPU is the observed case) makes that walk block
 * forever, and hardware watchdogs are not reliably present or functional.
 *
 * A restart therefore does not enter that walk at all: the notifier bugchecks
 * immediately. panic() records the failure through kmsg_dump()/pstore (and a
 * loaded crash kernel, if any) and then resets through the same emergency
 * path as SysRq-B. Filesystems are already synced and unmounted or read-only
 * by the time reboot notifiers run, so nothing is lost by skipping the walk.
 *
 * A power-off or halt still has to reach the firmware, so it is given a short
 * deadline instead. The timer keeps ticking while PID 1 is stuck in a driver
 * callback and bugchecks the same way, so a stuck power-off ends in a restart
 * rather than a hang, which is the recoverable outcome for a streaming host.
 */
static void __noreturn vkms_bugcheck(const char *reason)
{
	/*
	 * kernel.panic=0 would leave the panic sitting on the console forever,
	 * which is the one outcome this path exists to prevent; -1 restarts
	 * immediately. An explicit positive delay set by the administrator is
	 * honoured.
	 */
	if (panic_timeout == 0)
		panic_timeout = -1;
	panic("vibeshine_drm: %s\n", reason);
}

static void vkms_shutdown_deadline_expired(struct timer_list *timer)
{
	(void)timer;
	vkms_bugcheck("kernel power-off did not complete within the deadline");
}

static DEFINE_TIMER(vkms_shutdown_deadline_timer, vkms_shutdown_deadline_expired);

static void vkms_arm_shutdown_deadline(void)
{
	DRM_INFO("bugchecking into a restart if kernel power-off exceeds %u s\n",
		 VKMS_POWEROFF_DEADLINE_SECS);
	mod_timer(&vkms_shutdown_deadline_timer,
		  jiffies + secs_to_jiffies(VKMS_POWEROFF_DEADLINE_SECS));
}

static int vkms_reboot_notifier(struct notifier_block *notifier,
				unsigned long action, void *data)
{
	struct vkms_device *vkmsdev = container_of(notifier,
						    struct vkms_device,
						    reboot_notifier);

	(void)data;
	if (action == SYS_RESTART)
		vkms_bugcheck("restarting through bugcheck instead of the device shutdown walk");

	/*
	 * Arm before releasing anything: drm_dev_unplug() below sleeps in
	 * synchronize_srcu(), and the deadline has to cover that too.
	 */
	vkms_arm_shutdown_deadline();
	DRM_INFO("releasing virtual scanout before system shutdown\n");
	vkms_shutdown_release(vkmsdev);

	return NOTIFY_DONE;
}

int vkms_create(struct vkms_config *config)
{
	int ret;
	struct faux_device *fdev;
	struct vkms_device *vkms_device;
	const char *dev_name;

	dev_name = vkms_config_get_device_name(config);
	fdev = faux_device_create(dev_name, NULL, NULL);
	if (!fdev)
		return -ENODEV;

	if (!devres_open_group(&fdev->dev, NULL, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto out_unregister;
	}

	vkms_device = devm_drm_dev_alloc(&fdev->dev, &vkms_driver,
					 struct vkms_device, drm);
	if (IS_ERR(vkms_device)) {
		ret = PTR_ERR(vkms_device);
		goto out_devres;
	}
	vkms_device->faux_dev = fdev;
	vkms_device->config = config;
	mutex_init(&vkms_device->commit_lock);
	mutex_init(&vkms_device->shutdown_lock);
	vkms_device->accepting_commits = true;
	vkms_device->unplugged = false;
	vkms_device->shutdown_owner = NULL;
	vkms_device->reboot_notifier.notifier_call = vkms_reboot_notifier;
	dev_set_drvdata(&fdev->dev, vkms_device);
	vkms_config_get(config);
	ret = drmm_add_action_or_reset(&vkms_device->drm,
				       vkms_config_put_action, config);
	if (ret)
		goto out_devres;
	config->dev = vkms_device;

	ret = dma_coerce_mask_and_coherent(vkms_device->drm.dev,
					   DMA_BIT_MASK(64));

	if (ret) {
		DRM_ERROR("Could not initialize DMA support\n");
		goto out_devres;
	}

	/* Every CRTC exposes the same synthetic U32 counter. Configure the
	 * static device-wide maximum before vblank initialization; the per-CRTC
	 * runtime setter is only valid while that CRTC is in modeset/off state.
	 */
	vkms_device->drm.max_vblank_count = U32_MAX;
	ret = drm_vblank_init(&vkms_device->drm,
			      vkms_config_get_num_crtcs(config));
	if (ret) {
		DRM_ERROR("Failed to vblank\n");
		goto out_devres;
	}

	ret = vkms_modeset_init(vkms_device);
	if (ret)
		goto out_devres;

	ret = devm_device_add_group(&fdev->dev, &vkms_device_attr_group);
	if (ret)
		goto out_devres;

	ret = drm_dev_register(&vkms_device->drm, 0);
	if (ret)
		goto out_devres;

	ret = devm_register_reboot_notifier(&fdev->dev,
					    &vkms_device->reboot_notifier);
	if (ret) {
		drm_dev_unregister(&vkms_device->drm);
		goto out_devres;
	}

	drm_client_setup(&vkms_device->drm, NULL);

	return 0;

out_devres:
	config->dev = NULL;
	devres_release_group(&fdev->dev, NULL);
out_unregister:
	faux_device_destroy(fdev);
	return ret;
}

static int __init vkms_init(void)
{
	int ret;
	struct vkms_config *config;

	ret = vkms_configfs_register();
	if (ret)
		return ret;

	if (!create_default_dev)
		return 0;

	config = vkms_config_default_create(enable_cursor, enable_writeback,
					    enable_overlay, enable_plane_pipeline);
	if (IS_ERR(config))
		return PTR_ERR(config);

	ret = vkms_create(config);
	if (ret) {
		vkms_config_destroy(config);
		return ret;
	}

	default_config = config;

	return 0;
}

void vkms_destroy(struct vkms_config *config)
{
	struct faux_device *fdev;
	struct vkms_device *vkmsdev;

	if (!config->dev) {
		DRM_INFO("vkms_device is NULL.\n");
		return;
	}

	vkmsdev = config->dev;
	fdev = vkmsdev->faux_dev;

	vkms_shutdown_release(vkmsdev);
	config->dev = NULL;
	devres_release_group(&fdev->dev, NULL);
	faux_device_destroy(fdev);
}

static void __exit vkms_exit(void)
{
	vkms_configfs_unregister();

	if (default_config) {
		vkms_destroy(default_config);
		vkms_config_destroy(default_config);
	}

	/*
	 * Every reboot notifier is unregistered by now, so nothing can re-arm
	 * the deadline; shut the timer down so an unload during a shutdown that
	 * is still completing cannot leave a callback behind.
	 */
	timer_shutdown_sync(&vkms_shutdown_deadline_timer);
}

module_init(vkms_init);
module_exit(vkms_exit);

MODULE_AUTHOR("Haneen Mohammed <hamohammed.sa@gmail.com>");
MODULE_AUTHOR("Rodrigo Siqueira <rodrigosiqueiramelo@gmail.com>");
MODULE_AUTHOR("Vibeshine contributors");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION(VIBESHINE_DRM_VERSION);
MODULE_IMPORT_NS("DMA_BUF");
MODULE_LICENSE("GPL");
