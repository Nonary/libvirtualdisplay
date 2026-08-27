/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _VKMS_CONNECTOR_H_
#define _VKMS_CONNECTOR_H_

#include "vkms_drv.h"

#define drm_connector_to_vkms_connector(target) \
	container_of(target, struct vkms_connector, base)

/**
 * struct vkms_connector - VKMS custom type wrapping around the DRM connector
 *
 * @drm: Base DRM connector
 * @status: Runtime connector state copied from configfs at device creation.
 */
struct vkms_connector {
	struct drm_connector base;
	enum drm_connector_status status;
};

/**
 * vkms_connector_init() - Initialize a connector
 * @vkmsdev: VKMS device containing the connector
 *
 * Returns:
 * The connector or an error on failure.
 */
struct vkms_connector *vkms_connector_init(struct vkms_device *vkmsdev,
					   enum drm_connector_status status);

/**
 * vkms_connector_set_status() - Update an instantiated connector's status
 * @connector: Runtime connector to update
 * @status: New connector status
 */
void vkms_connector_set_status(struct vkms_connector *connector,
			       enum drm_connector_status status);

/**
 * vkms_trigger_connector_hotplug() - Update the device's connectors status
 * @vkmsdev: VKMS device to update
 */
void vkms_trigger_connector_hotplug(struct vkms_device *vkmsdev);

#endif /* _VKMS_CONNECTOR_H_ */
