// SPDX-License-Identifier: GPL-2.0+

#include <linux/dma-fence.h>
#include <linux/timekeeping.h>
#include <linux/version.h>
#include <linux/slab.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#include <drm/drm_vblank_helper.h>
#endif

#include "vkms_drv.h"
#include "vibeshine_drm_compat.h"
#include "vibeshine_drm_vrr.h"

static bool vkms_crtc_handle_vblank(struct drm_crtc *crtc)
{
	struct vkms_output *output = drm_crtc_to_vkms_output(crtc);
	struct vkms_crtc_state *state;
	bool ret, fence_cookie;

	fence_cookie = dma_fence_begin_signalling();

	spin_lock(&output->lock);
	atomic64_set(&output->vrr_vblank_timestamp_ns, ktime_get_ns());
	atomic64_inc(&output->synthetic_vblank_counter);
	ret = drm_crtc_handle_vblank(crtc);
	if (!ret) {
		spin_unlock(&output->lock);
		dma_fence_end_signalling(fence_cookie);
		return false;
	}

	state = output->composer_state;
	spin_unlock(&output->lock);

	if (state && output->composer_enabled) {
		u64 frame = drm_crtc_accurate_vblank_count(crtc);

		/* update frame_start only if a queued vkms_composer_worker()
		 * has read the data
		 */
		spin_lock(&output->composer_lock);
		if (!state->crc_pending)
			state->frame_start = frame;
		else
			DRM_DEBUG_DRIVER("crc worker falling behind, frame_start: %llu, frame_end: %llu\n",
					 state->frame_start, frame);
		state->frame_end = frame;
		state->crc_pending = true;
		spin_unlock(&output->composer_lock);

		ret = queue_work(output->composer_workq, &state->composer_work);
		if (!ret)
			DRM_DEBUG_DRIVER("Composer worker already queued\n");
	}

	dma_fence_end_signalling(fence_cookie);
	return true;
}

#if VIBESHINE_DRM_HAS_VBLANK_HELPER
static bool vkms_crtc_handle_vblank_timeout(struct drm_crtc *crtc)
{
	return vkms_crtc_handle_vblank(crtc);
}

static enum hrtimer_restart vkms_vrr_vblank_simulate(struct hrtimer *timer)
{
	struct vkms_output *output = container_of(timer, struct vkms_output,
						  vrr_hrtimer);

	vkms_crtc_handle_vblank(&output->crtc);
	return HRTIMER_NORESTART;
}

static void vkms_schedule_vrr_vblank(struct drm_crtc *crtc)
{
	struct drm_vblank_crtc *vblank = drm_crtc_vblank_crtc(crtc);
	struct vkms_output *output = drm_crtc_to_vkms_output(crtc);
	u64 now_ns = ktime_get_ns();
	u64 deadline_ns;
	u64 period_ns;

	period_ns = max_t(int, READ_ONCE(vblank->framedur_ns), 0);
	deadline_ns = vibeshine_drm_vrr_presentation_deadline_ns(
		atomic64_read(&output->vrr_vblank_timestamp_ns), now_ns,
		period_ns);
	if (deadline_ns <= now_ns) {
		vkms_crtc_handle_vblank_timeout(crtc);
		return;
	}

	hrtimer_start(&output->vrr_hrtimer, ns_to_ktime(deadline_ns),
		      HRTIMER_MODE_ABS);
}
#else
static enum hrtimer_restart vkms_vblank_simulate(struct hrtimer *timer)
{
	struct vkms_output *output = container_of(timer, struct vkms_output,
						  vblank_hrtimer);
	u64 ret_overrun;

	ret_overrun = hrtimer_forward_now(&output->vblank_hrtimer,
					  output->period_ns);
	if (ret_overrun != 1)
		pr_warn("%s: vblank timer overrun\n", __func__);

	return vkms_crtc_handle_vblank(&output->crtc) ?
		HRTIMER_RESTART : HRTIMER_NORESTART;
}

static int vkms_enable_vblank(struct drm_crtc *crtc)
{
	struct drm_vblank_crtc *vblank = drm_crtc_vblank_crtc(crtc);
	struct vkms_output *output = drm_crtc_to_vkms_output(crtc);

	drm_calc_timestamping_constants(crtc, &crtc->mode);
	hrtimer_setup(&output->vblank_hrtimer, &vkms_vblank_simulate,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	output->period_ns = ktime_set(0, vblank->framedur_ns);
	hrtimer_start(&output->vblank_hrtimer, output->period_ns, HRTIMER_MODE_REL);

	return 0;
}

static void vkms_disable_vblank(struct drm_crtc *crtc)
{
	struct vkms_output *output = drm_crtc_to_vkms_output(crtc);

	hrtimer_cancel(&output->vblank_hrtimer);
}
#endif

static struct drm_crtc_state *
vkms_atomic_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct vkms_crtc_state *vkms_state;

	if (WARN_ON(!crtc->state))
		return NULL;

	vkms_state = kzalloc_obj(*vkms_state);
	if (!vkms_state)
		return NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &vkms_state->base);

	INIT_WORK(&vkms_state->composer_work, vkms_composer_worker);

	return &vkms_state->base;
}

static void vkms_atomic_crtc_destroy_state(struct drm_crtc *crtc,
					   struct drm_crtc_state *state)
{
	struct vkms_crtc_state *vkms_state = to_vkms_crtc_state(state);

	__drm_atomic_helper_crtc_destroy_state(state);

	WARN_ON(work_pending(&vkms_state->composer_work));
	kfree(vkms_state->active_planes);
	kfree(vkms_state);
}

static void vkms_atomic_crtc_reset(struct drm_crtc *crtc)
{
	struct vkms_crtc_state *vkms_state = kzalloc_obj(*vkms_state);

	if (!vkms_state)
		return;

	if (crtc->state)
		vkms_atomic_crtc_destroy_state(crtc, crtc->state);

	__drm_atomic_helper_crtc_reset(crtc, &vkms_state->base);
	INIT_WORK(&vkms_state->composer_work, vkms_composer_worker);
}

static u32 vkms_crtc_get_vblank_counter(struct drm_crtc *crtc)
{
	struct vkms_output *output = drm_crtc_to_vkms_output(crtc);

	return (u32)atomic64_read(&output->synthetic_vblank_counter);
}

static bool vkms_crtc_get_vblank_timestamp(struct drm_crtc *crtc,
					   int *max_error,
					   ktime_t *vblank_time,
					   bool in_vblank_irq)
{
#if VIBESHINE_DRM_HAS_VBLANK_HELPER
	struct vkms_output *output = drm_crtc_to_vkms_output(crtc);
	u64 timestamp_ns;

	if (!READ_ONCE(crtc->state->vrr_enabled))
		return drm_crtc_vblank_helper_get_vblank_timestamp_from_timer(
			crtc, max_error, vblank_time, in_vblank_irq);

	/* The explicit synthetic counter advances exactly once per generated
	 * event attempt, so the real immediate timestamp cannot make DRM infer zero or
	 * several nominal-period vblanks.
	 */
	(void)in_vblank_irq;
	timestamp_ns = atomic64_read(&output->vrr_vblank_timestamp_ns);
	if (!timestamp_ns)
		return false;
	*max_error = 0;
	*vblank_time = ns_to_ktime(timestamp_ns);

	return true;
#else
	struct vkms_output *output = drm_crtc_to_vkms_output(crtc);
	struct drm_vblank_crtc *vblank = drm_crtc_vblank_crtc(crtc);

	(void)max_error;
	(void)in_vblank_irq;

	if (!READ_ONCE(vblank->enabled)) {
		*vblank_time = ktime_get();
		return true;
	}

	*vblank_time = READ_ONCE(output->vblank_hrtimer.node.expires);
	if (WARN_ON(*vblank_time == vblank->time))
		return true;

	/* The timer is advanced before drm_crtc_handle_vblank() runs. */
	*vblank_time -= output->period_ns;

	return true;
#endif
}

static const struct drm_crtc_funcs vkms_crtc_funcs = {
	.set_config             = drm_atomic_helper_set_config,
	.page_flip              = drm_atomic_helper_page_flip,
	.reset                  = vkms_atomic_crtc_reset,
	.atomic_duplicate_state = vkms_atomic_crtc_duplicate_state,
	.atomic_destroy_state   = vkms_atomic_crtc_destroy_state,
	.get_vblank_counter     = vkms_crtc_get_vblank_counter,
#if VIBESHINE_DRM_HAS_VBLANK_HELPER
	.enable_vblank          = drm_crtc_vblank_helper_enable_vblank_timer,
	.disable_vblank         = drm_crtc_vblank_helper_disable_vblank_timer,
#else
	.enable_vblank          = vkms_enable_vblank,
	.disable_vblank         = vkms_disable_vblank,
#endif
	.get_vblank_timestamp   = vkms_crtc_get_vblank_timestamp,
};

static int vkms_crtc_atomic_check(struct drm_crtc *crtc,
				  struct drm_atomic_commit *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state,
									  crtc);
	struct vkms_crtc_state *vkms_state = to_vkms_crtc_state(crtc_state);
	struct drm_plane *plane;
	struct drm_plane_state *plane_state;
	int i = 0, ret;

	if (vkms_state->active_planes)
		return 0;

	ret = drm_atomic_add_affected_planes(crtc_state->state, crtc);
	if (ret < 0)
		return ret;

	drm_for_each_plane_mask(plane, crtc->dev, crtc_state->plane_mask) {
		plane_state = drm_atomic_get_new_plane_state(crtc_state->state, plane);
		WARN_ON(!plane_state);

		if (!plane_state->visible)
			continue;

		i++;
	}

	vkms_state->active_planes = kzalloc_objs(*vkms_state->active_planes, i);
	if (!vkms_state->active_planes)
		return -ENOMEM;
	vkms_state->num_active_planes = i;

	i = 0;
	drm_for_each_plane_mask(plane, crtc->dev, crtc_state->plane_mask) {
		plane_state = drm_atomic_get_new_plane_state(crtc_state->state, plane);

		if (!plane_state->visible)
			continue;

		vkms_state->active_planes[i++] =
			to_vkms_plane_state(plane_state);
	}

	return 0;
}

static void vkms_crtc_atomic_begin(struct drm_crtc *crtc,
				   struct drm_atomic_commit *state)
	__acquires(&vkms_output->lock)
{
	struct vkms_output *vkms_output = drm_crtc_to_vkms_output(crtc);

	/* This lock is held across the atomic commit to block vblank timer
	 * from scheduling vkms_composer_worker until the composer is updated
	 */
	spin_lock_irq(&vkms_output->lock);
}

static void vkms_crtc_atomic_flush(struct drm_crtc *crtc,
				   struct drm_atomic_commit *state)
	__releases(&vkms_output->lock)
{
	struct vkms_output *vkms_output = drm_crtc_to_vkms_output(crtc);
#if VIBESHINE_DRM_HAS_VBLANK_HELPER
	struct drm_crtc_state *old_crtc_state;
	bool vrr_flip = false;
	bool vrr_disabled = false;

	old_crtc_state = drm_atomic_get_old_crtc_state(state, crtc);
	if (old_crtc_state)
		vrr_disabled = old_crtc_state->vrr_enabled &&
			       !crtc->state->vrr_enabled;
#endif

	if (crtc->state->event) {
		spin_lock(&crtc->dev->event_lock);

		if (drm_crtc_vblank_get(crtc) != 0)
			drm_crtc_send_vblank_event(crtc, crtc->state->event);
		else {
			drm_crtc_arm_vblank_event(crtc, crtc->state->event);
#if VIBESHINE_DRM_HAS_VBLANK_HELPER
			vrr_flip = crtc->state->vrr_enabled;
#endif
		}

		spin_unlock(&crtc->dev->event_lock);

		crtc->state->event = NULL;
	}

	vkms_output->composer_state = to_vkms_crtc_state(crtc->state);

	spin_unlock_irq(&vkms_output->lock);

#if VIBESHINE_DRM_HAS_VBLANK_HELPER
	/*
	 * Adaptive sync removes the fixed cadence, not the mode's maximum refresh
	 * rate. Stop the periodic timer, but defer an early flip until one nominal
	 * mode period has elapsed since the previous synthetic vblank. Slower flips
	 * still complete immediately. Restore the periodic timer when VRR is off.
	 */
	if (crtc->state->vrr_enabled && old_crtc_state &&
	    old_crtc_state->active)
		drm_crtc_vblank_cancel_timer(crtc);
	if (vrr_flip) {
		vkms_schedule_vrr_vblank(crtc);
	} else if (vrr_disabled) {
		hrtimer_cancel(&vkms_output->vrr_hrtimer);
		atomic64_set(&vkms_output->vrr_vblank_timestamp_ns, 0);
		drm_crtc_vblank_start_timer(crtc);
	}
#endif
}

static void vkms_crtc_atomic_enable(struct drm_crtc *crtc,
				    struct drm_atomic_commit *state)
{
#if VIBESHINE_DRM_HAS_VBLANK_HELPER
	drm_crtc_vblank_atomic_enable(crtc, state);

	/* drm_crtc_vblank_on() starts the timer after the initial atomic flush. */
	if (crtc->state->vrr_enabled) {
		atomic64_set(&drm_crtc_to_vkms_output(crtc)->vrr_vblank_timestamp_ns,
			     ktime_get_ns());
		drm_crtc_vblank_cancel_timer(crtc);
	}
#else
	(void)state;
	drm_crtc_vblank_on(crtc);
#endif
}

static void vkms_crtc_atomic_disable(struct drm_crtc *crtc,
				     struct drm_atomic_commit *state)
{
#if VIBESHINE_DRM_HAS_VBLANK_HELPER
	struct vkms_output *output = drm_crtc_to_vkms_output(crtc);

	hrtimer_cancel(&output->vrr_hrtimer);
	atomic64_set(&output->vrr_vblank_timestamp_ns, 0);
	drm_crtc_vblank_atomic_disable(crtc, state);
#else
	(void)state;
	drm_crtc_vblank_off(crtc);
#endif
}

static const struct drm_crtc_helper_funcs vkms_crtc_helper_funcs = {
	.atomic_check	= vkms_crtc_atomic_check,
	.atomic_begin	= vkms_crtc_atomic_begin,
	.atomic_flush	= vkms_crtc_atomic_flush,
	.atomic_enable	= vkms_crtc_atomic_enable,
	.atomic_disable	= vkms_crtc_atomic_disable,
#if VIBESHINE_DRM_HAS_VBLANK_HELPER
	.handle_vblank_timeout = vkms_crtc_handle_vblank_timeout,
#endif
};

void vkms_crtc_release_presented_frame(struct vkms_output *output)
{
	struct drm_framebuffer *fb;

	spin_lock_irq(&output->present_lock);
	fb = output->present_fb;
	output->present_fb = NULL;
	spin_unlock_irq(&output->present_lock);
	if (fb)
		drm_framebuffer_put(fb);
}

static void vkms_present_fb_cleanup(struct drm_device *dev, void *data)
{
	(void)dev;
	vkms_crtc_release_presented_frame(data);
}

static void vkms_present_trace_cleanup(struct drm_device *dev, void *data)
{
	(void)dev;
	kvfree(data);
}

#if VIBESHINE_DRM_HAS_VBLANK_HELPER
static void vkms_vrr_timer_cleanup(struct drm_device *dev, void *data)
{
	struct vkms_output *output = data;

	(void)dev;
	hrtimer_cancel(&output->vrr_hrtimer);
}
#endif

struct vkms_output *vkms_crtc_init(struct drm_device *dev, struct drm_plane *primary,
				   struct drm_plane *cursor)
{
	struct vkms_output *vkms_out;
	struct drm_crtc *crtc;
	int ret;

	vkms_out = drmm_crtc_alloc_with_planes(dev, struct vkms_output, crtc,
					       primary, cursor,
					       &vkms_crtc_funcs, NULL);
	if (IS_ERR(vkms_out)) {
		DRM_DEV_ERROR(dev->dev, "Failed to init CRTC\n");
		return vkms_out;
	}
	vkms_out->present_trace = kvcalloc(VIBESHINE_DRM_TRACE_HISTORY_SIZE,
					    sizeof(*vkms_out->present_trace), GFP_KERNEL);
	if (!vkms_out->present_trace)
		return ERR_PTR(-ENOMEM);
	ret = drmm_add_action_or_reset(dev, vkms_present_trace_cleanup,
				       vkms_out->present_trace);
	if (ret)
		return ERR_PTR(ret);

	crtc = &vkms_out->crtc;

	drm_crtc_helper_add(crtc, &vkms_crtc_helper_funcs);

	ret = drm_mode_crtc_set_gamma_size(crtc, VKMS_LUT_SIZE);
	if (ret) {
		DRM_ERROR("Failed to set gamma size\n");
		return ERR_PTR(ret);
	}

	drm_crtc_enable_color_mgmt(crtc, 0, false, VKMS_LUT_SIZE);

#if VIBESHINE_DRM_HAS_BACKGROUND_COLOR
	drm_crtc_attach_background_color_property(crtc);
#endif

	spin_lock_init(&vkms_out->lock);
	spin_lock_init(&vkms_out->composer_lock);
#if VIBESHINE_DRM_HAS_VBLANK_HELPER
	hrtimer_setup(&vkms_out->vrr_hrtimer, &vkms_vrr_vblank_simulate,
		      CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	ret = drmm_add_action_or_reset(dev, vkms_vrr_timer_cleanup, vkms_out);
	if (ret)
		return ERR_PTR(ret);
#endif
	atomic64_set(&vkms_out->synthetic_vblank_counter, 0);
	atomic64_set(&vkms_out->vrr_vblank_timestamp_ns, 0);
	atomic64_set(&vkms_out->present_sequence, 0);
	atomic_set(&vkms_out->pending_commits, 0);
	vkms_out->present_timestamp_ns = 0;
	vkms_out->present_fb = NULL;
	vkms_out->present_waiters = 0;
	spin_lock_init(&vkms_out->present_lock);
	init_waitqueue_head(&vkms_out->present_waitq);
	ret = drmm_add_action_or_reset(dev, vkms_present_fb_cleanup, vkms_out);
	if (ret)
		return ERR_PTR(ret);

	vkms_out->composer_workq = drmm_alloc_ordered_workqueue(dev, "vkms_composer", 0);
	if (IS_ERR(vkms_out->composer_workq))
		return ERR_CAST(vkms_out->composer_workq);

	return vkms_out;
}
