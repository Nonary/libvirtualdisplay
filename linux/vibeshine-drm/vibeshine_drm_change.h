/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _VIBESHINE_DRM_CHANGE_H_
#define _VIBESHINE_DRM_CHANGE_H_

#if !defined(__KERNEL__) && !defined(__cplusplus)
#include <stdbool.h>
#endif

/*
 * Pointer/object values in these normalized records are identity tokens only.
 * Keeping the comparison pointer-free makes the policy executable in userland
 * tests and avoids comparing padding or transient DRM bookkeeping fields.
 */
struct vibeshine_drm_plane_snapshot {
	bool present;
	bool visible;
	bool content_update;
	unsigned long crtc;
	unsigned long framebuffer;
	unsigned long color_pipeline;
	int crtc_x;
	int crtc_y;
	int hotspot_x;
	int hotspot_y;
	unsigned int crtc_w;
	unsigned int crtc_h;
	unsigned int src_x;
	unsigned int src_y;
	unsigned int src_w;
	unsigned int src_h;
	unsigned int alpha;
	unsigned int pixel_blend_mode;
	unsigned int rotation;
	unsigned int zpos;
	unsigned int normalized_zpos;
	unsigned int color_encoding;
	unsigned int color_range;
	unsigned int scaling_filter;
};

struct vibeshine_drm_crtc_snapshot {
	bool present;
	bool enable;
	bool active;
	bool color_mgmt_changed;
	unsigned int plane_mask;
	unsigned int connector_mask;
	unsigned int encoder_mask;
	unsigned long mode_blob;
	unsigned long degamma_lut;
	unsigned long ctm;
	unsigned long gamma_lut;
	unsigned long long background_color;
	unsigned int scaling_filter;
	unsigned int sharpness_strength;
	bool vrr_enabled;
};

struct vibeshine_drm_connector_snapshot {
	bool present;
	bool self_refresh_aware;
	unsigned long crtc;
	unsigned long best_encoder;
	unsigned long hdr_output_metadata;
	unsigned int link_status;
	unsigned int picture_aspect_ratio;
	unsigned int content_type;
	unsigned int hdcp_content_type;
	unsigned int scaling_mode;
	unsigned int content_protection;
	unsigned int colorspace;
	unsigned int max_requested_bpc;
	unsigned int max_bpc;
	unsigned int privacy_screen_sw_state;
};

static inline bool
vibeshine_drm_plane_changes_scanout(
	const struct vibeshine_drm_plane_snapshot *old_state,
	const struct vibeshine_drm_plane_snapshot *new_state)
{
	if (old_state->present != new_state->present ||
	    old_state->visible != new_state->visible ||
	    old_state->crtc != new_state->crtc ||
	    old_state->framebuffer != new_state->framebuffer ||
	    old_state->color_pipeline != new_state->color_pipeline ||
	    old_state->crtc_x != new_state->crtc_x ||
	    old_state->crtc_y != new_state->crtc_y ||
	    old_state->hotspot_x != new_state->hotspot_x ||
	    old_state->hotspot_y != new_state->hotspot_y ||
	    old_state->crtc_w != new_state->crtc_w ||
	    old_state->crtc_h != new_state->crtc_h ||
	    old_state->src_x != new_state->src_x ||
	    old_state->src_y != new_state->src_y ||
	    old_state->src_w != new_state->src_w ||
	    old_state->src_h != new_state->src_h ||
	    old_state->alpha != new_state->alpha ||
	    old_state->pixel_blend_mode != new_state->pixel_blend_mode ||
	    old_state->rotation != new_state->rotation ||
	    old_state->zpos != new_state->zpos ||
	    old_state->normalized_zpos != new_state->normalized_zpos ||
	    old_state->color_encoding != new_state->color_encoding ||
	    old_state->color_range != new_state->color_range ||
	    old_state->scaling_filter != new_state->scaling_filter)
		return true;

	/* A new fence or damage blob can describe new pixels in the same FB. */
	return new_state->content_update;
}

static inline bool
vibeshine_drm_crtc_changes_scanout(
	const struct vibeshine_drm_crtc_snapshot *old_state,
	const struct vibeshine_drm_crtc_snapshot *new_state)
{
	return old_state->present != new_state->present ||
	       old_state->enable != new_state->enable ||
	       old_state->active != new_state->active ||
	       old_state->plane_mask != new_state->plane_mask ||
	       old_state->connector_mask != new_state->connector_mask ||
	       old_state->encoder_mask != new_state->encoder_mask ||
	       old_state->mode_blob != new_state->mode_blob ||
	       old_state->degamma_lut != new_state->degamma_lut ||
	       old_state->ctm != new_state->ctm ||
	       old_state->gamma_lut != new_state->gamma_lut ||
	       old_state->background_color != new_state->background_color ||
	       old_state->scaling_filter != new_state->scaling_filter ||
	       old_state->sharpness_strength != new_state->sharpness_strength ||
	       old_state->vrr_enabled != new_state->vrr_enabled ||
	       new_state->color_mgmt_changed;
}

static inline bool
vibeshine_drm_connector_changes_scanout(
	const struct vibeshine_drm_connector_snapshot *old_state,
	const struct vibeshine_drm_connector_snapshot *new_state)
{
	return old_state->present != new_state->present ||
	       old_state->self_refresh_aware != new_state->self_refresh_aware ||
	       old_state->crtc != new_state->crtc ||
	       old_state->best_encoder != new_state->best_encoder ||
	       old_state->hdr_output_metadata != new_state->hdr_output_metadata ||
	       old_state->link_status != new_state->link_status ||
	       old_state->picture_aspect_ratio != new_state->picture_aspect_ratio ||
	       old_state->content_type != new_state->content_type ||
	       old_state->hdcp_content_type != new_state->hdcp_content_type ||
	       old_state->scaling_mode != new_state->scaling_mode ||
	       old_state->content_protection != new_state->content_protection ||
	       old_state->colorspace != new_state->colorspace ||
	       old_state->max_requested_bpc != new_state->max_requested_bpc ||
	       old_state->max_bpc != new_state->max_bpc ||
	       old_state->privacy_screen_sw_state != new_state->privacy_screen_sw_state;
}

#endif /* _VIBESHINE_DRM_CHANGE_H_ */
