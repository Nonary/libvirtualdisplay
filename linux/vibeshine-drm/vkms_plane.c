// SPDX-License-Identifier: GPL-2.0+

#include "vkms_config.h"
#include <linux/iosys-map.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_print.h>

#include "vkms_drv.h"
#include "vkms_formats.h"
#include "vibeshine_drm_compat.h"

static const u32 vkms_formats[] = {
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_BGRA8888,
	DRM_FORMAT_RGBA8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_XRGB2101010,
	DRM_FORMAT_XBGR2101010,
	DRM_FORMAT_ARGB2101010,
	DRM_FORMAT_ABGR2101010,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_XRGB16161616,
	DRM_FORMAT_XBGR16161616,
	DRM_FORMAT_ARGB16161616,
	DRM_FORMAT_ABGR16161616,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_BGR565,
	DRM_FORMAT_NV12,
	DRM_FORMAT_NV16,
	DRM_FORMAT_NV24,
	DRM_FORMAT_NV21,
	DRM_FORMAT_NV61,
	DRM_FORMAT_NV42,
	DRM_FORMAT_YUV420,
	DRM_FORMAT_YUV422,
	DRM_FORMAT_YUV444,
	DRM_FORMAT_YVU420,
	DRM_FORMAT_YVU422,
	DRM_FORMAT_YVU444,
	DRM_FORMAT_P010,
	DRM_FORMAT_P012,
	DRM_FORMAT_P016,
	DRM_FORMAT_R1,
	DRM_FORMAT_R2,
	DRM_FORMAT_R4,
	DRM_FORMAT_R8,
};

/*
 * Keep the virtual scanout buffer on the NVIDIA render GPU. The NVIDIA GBM
 * backend requires explicit block-linear modifiers; advertising only LINEAR
 * makes KWin fall back to a CPU dumb-buffer copy for every frame.
 *
 * The six block heights below are the uncompressed Turing+ layout advertised
 * by nvidia-drm. The PRIME importer preserves the producer DMA-BUF, so the
 * virtual device never needs to understand or touch the tiled pixels.
 */
static const u64 vkms_format_modifiers[] = {
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 5),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 4),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 3),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 2),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 1),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 0),
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

static bool vkms_format_mod_supported(struct drm_plane *plane, u32 format,
				      u64 modifier)
{
	if (modifier == DRM_FORMAT_MOD_LINEAR)
		return true;

	switch (modifier) {
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 5):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 4):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 3):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 2):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 1):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 6, 0):
		break;
	default:
		return false;
	}

	/* Match the RGB scanout formats shared by KWin and nvidia-drm. */
	switch (format) {
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_XBGR2101010:
		return true;
	default:
		return false;
	}
}

static struct drm_plane_state *
vkms_plane_duplicate_state(struct drm_plane *plane)
{
	struct vkms_plane_state *vkms_state;
	struct vkms_frame_info *frame_info;

	vkms_state = kzalloc_obj(*vkms_state);
	if (!vkms_state)
		return NULL;

	frame_info = kzalloc_obj(*frame_info);
	if (!frame_info) {
		DRM_DEBUG_KMS("Couldn't allocate frame_info\n");
		kfree(vkms_state);
		return NULL;
	}

	vkms_state->frame_info = frame_info;

	__drm_gem_duplicate_shadow_plane_state(plane, &vkms_state->base);

	return &vkms_state->base.base;
}

static void vkms_plane_destroy_state(struct drm_plane *plane,
				     struct drm_plane_state *old_state)
{
	struct vkms_plane_state *vkms_state = to_vkms_plane_state(old_state);
	struct drm_crtc *crtc = vkms_state->base.base.crtc;

	if (crtc && vkms_state->frame_info->fb) {
		/* dropping the reference we acquired in
		 * vkms_primary_plane_update()
		 */
		if (drm_framebuffer_read_refcount(vkms_state->frame_info->fb))
			drm_framebuffer_put(vkms_state->frame_info->fb);
	}

	kfree(vkms_state->frame_info);
	vkms_state->frame_info = NULL;

	__drm_gem_destroy_shadow_plane_state(&vkms_state->base);
	kfree(vkms_state);
}

static void vkms_plane_reset(struct drm_plane *plane)
{
	struct vkms_plane_state *vkms_state;

	if (plane->state) {
		vkms_plane_destroy_state(plane, plane->state);
		plane->state = NULL; /* must be set to NULL here */
	}

	vkms_state = kzalloc_obj(*vkms_state);
	if (!vkms_state) {
		DRM_ERROR("Cannot allocate vkms_plane_state\n");
		return;
	}

	__drm_gem_reset_shadow_plane(plane, &vkms_state->base);
}

static const struct drm_plane_funcs vkms_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.reset			= vkms_plane_reset,
	.atomic_duplicate_state = vkms_plane_duplicate_state,
	.atomic_destroy_state	= vkms_plane_destroy_state,
	.format_mod_supported	= vkms_format_mod_supported,
};

static void vkms_plane_atomic_update(struct drm_plane *plane,
				     struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state,
									   plane);
	struct vkms_plane_state *vkms_plane_state;
	struct drm_shadow_plane_state *shadow_plane_state;
	struct drm_framebuffer *fb = new_state->fb;
	struct vkms_frame_info *frame_info;
	u32 fmt;

	if (!new_state->crtc || !fb)
		return;

	fmt = fb->format->format;
	vkms_plane_state = to_vkms_plane_state(new_state);
	shadow_plane_state = &vkms_plane_state->base;

	frame_info = vkms_plane_state->frame_info;
	memcpy(&frame_info->src, &new_state->src, sizeof(struct drm_rect));
	memcpy(&frame_info->dst, &new_state->dst, sizeof(struct drm_rect));
	frame_info->fb = fb;
	memcpy(&frame_info->map, &shadow_plane_state->data, sizeof(frame_info->map));
	drm_framebuffer_get(frame_info->fb);
	frame_info->rotation = new_state->rotation;

	vkms_plane_state->pixel_read_line = get_pixel_read_line_function(fmt);
	get_conversion_matrix_to_argb_u16(fmt, new_state->color_encoding, new_state->color_range,
					  &vkms_plane_state->conversion_matrix);
}

static int vkms_plane_atomic_check(struct drm_plane *plane,
				   struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state,
										 plane);
	struct drm_crtc_state *crtc_state;
	int ret;

	if (!new_plane_state->fb || WARN_ON(!new_plane_state->crtc))
		return 0;

	crtc_state = drm_atomic_get_crtc_state(state,
					       new_plane_state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	ret = drm_atomic_helper_check_plane_state(new_plane_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  true, true);
	if (ret != 0)
		return ret;

	return 0;
}

static int vkms_prepare_fb(struct drm_plane *plane,
			   struct drm_plane_state *state)
{
	struct vkms_plane_state *vkms_state = to_vkms_plane_state(state);
	struct drm_shadow_plane_state *shadow_plane_state;
	struct drm_framebuffer *fb = state->fb;
	int ret;

	if (!fb)
		return 0;

	shadow_plane_state = to_drm_shadow_plane_state(state);

	ret = drm_gem_plane_helper_prepare_fb(plane, state);
	if (ret)
		return ret;

	/*
	 * A non-linear PRIME framebuffer belongs to the render GPU. Mapping it on
	 * the virtual device would force the very CPU migration this driver exists
	 * to avoid. Vblank/page-flip emulation does not access pixel memory. The
	 * product path disables CRC and writeback for these pass-through buffers.
	 */
	vkms_state->frame_mapped = false;
	if (fb->modifier != DRM_FORMAT_MOD_LINEAR)
		return 0;

	ret = drm_gem_fb_vmap(fb, shadow_plane_state->map,
			      shadow_plane_state->data);
	if (!ret)
		vkms_state->frame_mapped = true;
	return ret;
}

static void vkms_cleanup_fb(struct drm_plane *plane,
			    struct drm_plane_state *state)
{
	struct vkms_plane_state *vkms_state = to_vkms_plane_state(state);
	struct drm_shadow_plane_state *shadow_plane_state;
	struct drm_framebuffer *fb = state->fb;

	if (!fb || !vkms_state->frame_mapped)
		return;

	shadow_plane_state = to_drm_shadow_plane_state(state);

	drm_gem_fb_vunmap(fb, shadow_plane_state->map);
	vkms_state->frame_mapped = false;
}

static const struct drm_plane_helper_funcs vkms_plane_helper_funcs = {
	.atomic_update		= vkms_plane_atomic_update,
	.atomic_check		= vkms_plane_atomic_check,
	.prepare_fb		= vkms_prepare_fb,
	.cleanup_fb		= vkms_cleanup_fb,
};

struct vkms_plane *vkms_plane_init(struct vkms_device *vkmsdev,
				   struct vkms_config_plane *plane_cfg)
{
	struct drm_device *dev = &vkmsdev->drm;
	struct vkms_plane *plane;

	plane = drmm_universal_plane_alloc(dev, struct vkms_plane, base, 0,
					   &vkms_plane_funcs,
					   vkms_formats, ARRAY_SIZE(vkms_formats),
					   vkms_format_modifiers,
					   vkms_config_plane_get_type(plane_cfg),
					   NULL);
	if (IS_ERR(plane))
		return plane;

	drm_plane_helper_add(&plane->base, &vkms_plane_helper_funcs);

	drm_plane_create_rotation_property(&plane->base, DRM_MODE_ROTATE_0,
					   DRM_MODE_ROTATE_MASK | DRM_MODE_REFLECT_MASK);

	drm_plane_create_color_properties(&plane->base,
					  BIT(DRM_COLOR_YCBCR_BT601) |
					  BIT(DRM_COLOR_YCBCR_BT709) |
					  BIT(DRM_COLOR_YCBCR_BT2020),
					  BIT(DRM_COLOR_YCBCR_LIMITED_RANGE) |
					  BIT(DRM_COLOR_YCBCR_FULL_RANGE),
					  DRM_COLOR_YCBCR_BT601,
					  DRM_COLOR_YCBCR_FULL_RANGE);

	if (vkms_config_plane_get_default_pipeline(plane_cfg))
		vkms_initialize_colorops(&plane->base);

	return plane;
}
