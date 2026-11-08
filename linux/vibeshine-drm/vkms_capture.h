/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef VKMS_CAPTURE_H
#define VKMS_CAPTURE_H
struct dma_buf;
struct vkms_output;
struct vkms_capture_lease;
/* Called with present_lock held; allocation must precede taking that lock. */
struct vkms_capture_lease *vkms_capture_lease_alloc(void);
void vkms_capture_lease_begin(struct vkms_capture_lease *, struct vkms_output *);
void vkms_capture_lease_put(struct vkms_capture_lease *);
/* Consumes backing even on error. The exported object owns a lease reference. */
void vkms_capture_wait(struct vkms_output *);
struct dma_buf *vkms_capture_wrap(struct dma_buf *backing, struct vkms_capture_lease *);
#endif
