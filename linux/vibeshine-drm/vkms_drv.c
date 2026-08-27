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
#include <linux/device/faux.h>
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/string.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_gem.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_colorop.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_vblank.h>

#include "vkms_config.h"
#include "vkms_configfs.h"
#include "vkms_drv.h"
#include "vibeshine_drm_change.h"
#include "vibeshine_drm_present.h"
#include "vibeshine_drm_uapi.h"

#define DRIVER_NAME	"vibeshine_drm"
#define DRIVER_DESC	"Vibeshine HDR Virtual Display"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	1

static struct vkms_config *default_config;

static bool enable_cursor = true;
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

DEFINE_DRM_GEM_FOPS(vkms_driver_fops);

#define DRM_IOCTL_VIBESHINE_WAIT_PRESENT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_VIBESHINE_WAIT_PRESENT, \
		 struct vibeshine_drm_wait_present)

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
		atomic_read(&output->pending_commits) != 0,
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
			atomic64_read(&output->present_sequence) != requested_sequence &&
			atomic_read(&output->pending_commits) == 0,
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

static const struct drm_ioctl_desc vkms_ioctls[] = {
	DRM_IOCTL_DEF_DRV(VIBESHINE_WAIT_PRESENT, vkms_wait_present_ioctl, 0),
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
	snapshot->color_pipeline = (unsigned long)state->color_pipeline;
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
	snapshot->background_color = state->background_color;
	snapshot->scaling_filter = state->scaling_filter;
	snapshot->sharpness_strength = state->sharpness_strength;
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

static void vkms_signal_presented_crtcs(struct drm_atomic_commit *state)
{
	struct drm_crtc *crtc;

	drm_for_each_crtc(crtc, state->dev) {
		struct vkms_output *output;
		bool scanout_changed;

		if (!vkms_commit_touches_crtc(state, crtc))
			continue;

		scanout_changed = vkms_commit_changes_crtc(state, crtc);
		output = drm_crtc_to_vkms_output(crtc);
		spin_lock_irq(&output->present_lock);
		if (scanout_changed) {
			output->present_timestamp_ns = ktime_get_ns();
			atomic64_inc(&output->present_sequence);
		}
		if (WARN_ON_ONCE(atomic_read(&output->pending_commits) <= 0))
			atomic_set(&output->pending_commits, 0);
		else
			atomic_dec(&output->pending_commits);
		spin_unlock_irq(&output->present_lock);
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
	 * drm_atomic_helper_wait_for_fences() consumes new_plane_state->fence
	 * before commit_tail runs. Preserve its presence in the per-commit plane
	 * state so same-framebuffer explicit-fence updates still emit an event.
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
	vkms_device->accepting_commits = true;
	vkms_device->shutdown_owner = NULL;
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

	ret = drm_vblank_init(&vkms_device->drm,
			      vkms_config_get_num_crtcs(config));
	if (ret) {
		DRM_ERROR("Failed to vblank\n");
		goto out_devres;
	}

	ret = vkms_modeset_init(vkms_device);
	if (ret)
		goto out_devres;

	ret = drm_dev_register(&vkms_device->drm, 0);
	if (ret)
		goto out_devres;

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

	mutex_lock(&vkmsdev->commit_lock);
	vkmsdev->accepting_commits = false;
	vkmsdev->shutdown_owner = current;
	mutex_unlock(&vkmsdev->commit_lock);

	drm_atomic_helper_shutdown(&vkmsdev->drm);

	mutex_lock(&vkmsdev->commit_lock);
	vkmsdev->shutdown_owner = NULL;
	mutex_unlock(&vkmsdev->commit_lock);
	drm_dev_unplug(&vkmsdev->drm);
	config->dev = NULL;
	devres_release_group(&fdev->dev, NULL);
	faux_device_destroy(fdev);
}

static void __exit vkms_exit(void)
{
	vkms_configfs_unregister();

	if (!default_config)
		return;

	vkms_destroy(default_config);
	vkms_config_destroy(default_config);
}

module_init(vkms_init);
module_exit(vkms_exit);

MODULE_AUTHOR("Haneen Mohammed <hamohammed.sa@gmail.com>");
MODULE_AUTHOR("Rodrigo Siqueira <rodrigosiqueiramelo@gmail.com>");
MODULE_AUTHOR("Vibeshine contributors");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION("1.1.0");
MODULE_LICENSE("GPL");
