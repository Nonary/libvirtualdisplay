// SPDX-License-Identifier: GPL-2.0+
/*
 * A capture export owns a read lease on the current scanout. Atomic commit
 * admission closes acquisition before a later commit can replace scanout. A commit
 * waits until all exported objects and their importer references are gone;
 * synchronous commits are rejected while readers remain. This relies on the normal DRM producer rule:
 * do not modify a displayed buffer until its replacement has completed.
 */
#include "vkms_capture.h"
#include "vkms_drv.h"
#include <drm/drm_drv.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/kref.h>
#include <linux/slab.h>

struct vkms_capture_lease {
	struct kref refs;
	struct vkms_output *output;
};
struct vkms_capture_buffer {
	struct dma_buf *backing;
	struct vkms_capture_lease *lease;
};

struct vkms_capture_lease *vkms_capture_lease_alloc(void)
{
	struct vkms_capture_lease *lease = kzalloc(sizeof(*lease), GFP_KERNEL);
	if (lease)
		kref_init(&lease->refs);
	return lease;
}

void vkms_capture_lease_begin(struct vkms_capture_lease *lease, struct vkms_output *output)
{
	lockdep_assert_held(&output->present_lock);
	lease->output = output;
	drm_dev_get(output->crtc.dev);
}

static void vkms_capture_lease_release(struct kref *ref)
{
	struct vkms_capture_lease *lease = container_of(ref, struct vkms_capture_lease, refs);
	struct vkms_output *output = lease->output;
	if (output) {
		unsigned long flags;
		struct drm_device *dev = output->crtc.dev;
		spin_lock_irqsave(&output->present_lock, flags);
		WARN_ON_ONCE(!vibeshine_capture_release(&output->capture));
		spin_unlock_irqrestore(&output->present_lock, flags);
		wake_up_all(&output->capture_waitq);
		drm_dev_put(dev);
	}
	kfree(lease);
}

void vkms_capture_lease_put(struct vkms_capture_lease *lease)
{
	if (lease)
		kref_put(&lease->refs, vkms_capture_lease_release);
}

/* Only asynchronous commit workers can wait for readers: synchronous commits
 * with readers are rejected before DRM state submission. The reservation was
 * already established at admission, so no new readers can enter while waiting. */
void vkms_capture_wait(struct vkms_output *output)
{
	wait_event(output->capture_waitq, !READ_ONCE(output->capture.readers));
}

static int capture_attach(struct dma_buf *buf, struct dma_buf_attachment *attachment)
{
	struct vkms_capture_buffer *capture = buf->priv;
	struct dma_buf_attachment *backing = dma_buf_attach(capture->backing, attachment->dev);
	if (IS_ERR(backing))
		return PTR_ERR(backing);
	attachment->priv = backing;
	return 0;
}
static void capture_detach(struct dma_buf *buf, struct dma_buf_attachment *attachment)
{
	struct vkms_capture_buffer *capture = buf->priv;
	dma_buf_detach(capture->backing, attachment->priv);
}
static struct sg_table *capture_map(struct dma_buf_attachment *attachment,
				    enum dma_data_direction direction)
{
	return dma_buf_map_attachment(attachment->priv, direction);
}
static void capture_unmap(struct dma_buf_attachment *attachment, struct sg_table *table,
			  enum dma_data_direction direction)
{
	dma_buf_unmap_attachment(attachment->priv, table, direction);
}
/* No mmap/vmap forwarding: dma_buf_mmap(backing) replaces vm_file and
 * would let a mapping outlive the wrapper lease. GPU importers retain the
 * wrapper through their normal DMA-BUF attachment lifetime instead. */
static void capture_release(struct dma_buf *buf)
{
	struct vkms_capture_buffer *capture = buf->priv;
	dma_buf_put(capture->backing);
	vkms_capture_lease_put(capture->lease);
	kfree(capture);
}
static const struct dma_buf_ops capture_ops = {
    .attach = capture_attach,
    .detach = capture_detach,
    .map_dma_buf = capture_map,
    .unmap_dma_buf = capture_unmap,
    .release = capture_release,
};
struct dma_buf *vkms_capture_wrap(struct dma_buf *backing, struct vkms_capture_lease *lease)
{
	DEFINE_DMA_BUF_EXPORT_INFO(info);
	struct vkms_capture_buffer *capture;
	struct dma_buf *export;
	capture = kzalloc(sizeof(*capture), GFP_KERNEL);
	if (!capture) {
		dma_buf_put(backing);
		return ERR_PTR(-ENOMEM);
	}
	capture->backing = backing;
	capture->lease = lease;
	kref_get(&lease->refs);
	info.ops = &capture_ops;
	info.size = backing->size;
	info.flags = O_RDONLY;
	info.resv = backing->resv;
	info.priv = capture;
	export = dma_buf_export(&info);
	if (IS_ERR(export)) {
		dma_buf_put(backing);
		vkms_capture_lease_put(lease);
		kfree(capture);
	}
	return export;
}
