// SPDX-License-Identifier: GPL-2.0
/* Rotate RGB888 bytes as Y400; DISP reads the result as three interleaved planes. */
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <drm/drm_plane.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include "scaler.h"
#include "soph_rot.h"

struct soph_rot {
	struct device *dev;
	struct resource *region;
	void __iomem *regs;
	struct clk_bulk_data *clks;
	int nclks;
	bool clocks_on, failed, ready;
	unsigned int width, height, pitch, rows, angle, front;
	size_t size;
	void *buf[2];
	dma_addr_t dma[2];
	struct completion latched;
};

static void soph_rot_destroy(void *data)
{
	struct soph_rot *r = data;
	unsigned int i;

	/* No safe reset exists. A timed-out engine may still access both buffers
	 * and the source GEM (whose job reference is retained). Keep its clocks
	 * and exclusive register reservation too, even across driver unbind. */
	if (r->failed)
		return;
	if (r->regs) {
		writel(0, r->regs + 0x50);
		iounmap(r->regs);
	}
	for (i = 0; i < 2; i++)
		if (r->buf[i])
			dma_free_coherent(r->dev, r->size, r->buf[i], r->dma[i]);
	if (r->clocks_on)
		clk_bulk_disable_unprepare(r->nclks, r->clks);
	if (r->nclks > 0)
		clk_bulk_put_all(r->nclks, r->clks);
	if (r->region)
		release_mem_region(r->region->start, resource_size(r->region));
	put_device(r->dev);
	kfree(r);
}

struct soph_rot *soph_rot_create(struct platform_device *pdev, unsigned int w,
				 unsigned int h, unsigned int angle)
{
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	struct soph_rot *r;
	unsigned int i;
	int ret = -ENOMEM;

	/* LDC dimensions are 12-bit, in bytes, with 64-byte/tall padding. */
	if ((angle != 90 && angle != 270) || !w || !h ||
	    w > 4032 / 3 || h > 4032 || !res)
		return ERR_PTR(-EINVAL);
	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return ERR_PTR(-ENOMEM);
	r->dev = get_device(&pdev->dev);
	r->width = w;
	r->height = h;
	r->angle = angle;
	r->pitch = ALIGN(w * 3, 64);
	r->rows = ALIGN(h, 64);
	r->size = r->pitch * r->rows;
	init_completion(&r->latched);
	complete_all(&r->latched);
	r->region = request_mem_region(res->start, resource_size(res), "soph-rot");
	if (!r->region) {
		ret = -EBUSY;
		goto fail;
	}
	r->nclks = clk_bulk_get_all(r->dev, &r->clks);
	if (r->nclks <= 0) {
		ret = r->nclks ?: -ENODEV;
		goto fail;
	}
	ret = clk_bulk_prepare_enable(r->nclks, r->clks);
	if (ret)
		goto fail;
	r->clocks_on = true;
	r->regs = ioremap(res->start, resource_size(res));
	ret = -ENOMEM;
	if (!r->regs)
		goto fail;
	for (i = 0; i < 2; i++) {
		r->buf[i] = dma_alloc_coherent(r->dev, r->size, &r->dma[i], GFP_KERNEL);
		if (!r->buf[i])
			goto fail;
	}
	ret = devm_add_action_or_reset(r->dev, soph_rot_destroy, r);
	return ret ? ERR_PTR(ret) : r;
fail:
	soph_rot_destroy(r);
	return ERR_PTR(ret);
}

int soph_rot_check(struct soph_rot *r, struct drm_plane_state *state)
{
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_dma_object *gem = drm_fb_dma_get_gem_obj(fb, 0);

	/* The engine reads the padding as well. A normal, unpadded import is
	 * unsuitable even if all its visible pixels would fit. */
	if (fb->format->format != DRM_FORMAT_RGB888 ||
	    fb->width != r->width || fb->height != r->height ||
	    fb->pitches[0] != r->pitch || fb->offsets[0] ||
	    !gem || gem->base.size < r->size ||
	    !IS_ALIGNED(gem->dma_addr, 64) ||
	    (u64)gem->dma_addr + r->size > BIT_ULL(32))
		return -EINVAL;
	return 0;
}

int soph_rot_run(struct soph_rot *r, struct drm_plane_state *state)
{
	struct drm_gem_dma_object *gem = drm_fb_dma_get_gem_obj(state->fb, 0);
	unsigned int next = !r->front;
	void __iomem *b = r->regs;
	u32 status;
	int ret;

	if (r->failed)
		return -EIO;
	/* Synthetic vblanks cannot release a scanout buffer. Only a real frame
	 * start or the explicit park/enable latch completes this wait. */
	if (!wait_for_completion_timeout(&r->latched, msecs_to_jiffies(50))) {
		dev_err_ratelimited(r->dev, "rotation: no display latch; keeping front buffer\n");
		return -ETIMEDOUT;
	}
	drm_gem_object_get(&gem->base);
	wmb();
	writel(0, b + 0x50); /* Poll raw status; do not assert an unhandled IRQ. */
	writel(1, b + 0x5c);
	writel(0, b + 0x00); /* Y400, bypass the mesh/interpolator. */
	writel(1, b + 0x04);
	writel(r->pitch, b + 0x08);
	writel(r->rows, b + 0x0c);
	writel(0, b + 0x10);
	writel(1, b + 0x14);
	writel(gem->dma_addr >> 6, b + 0x20);
	writel(0, b + 0x24);
	writel(r->pitch, b + 0x28);
	writel(r->rows, b + 0x2c);
	writel(0, b + 0x30);
	writel(r->pitch - 1, b + 0x34); /* Rotate the entire padded grid. */
	writel(0, b + 0x38);
	writel(r->dma[next] >> 6, b + 0x40);
	writel(0, b + 0x44);
	writel(r->angle == 90 ? 3 : 2, b + 0x48);
	writel(0, b + 0x60);
	writel(0, b + 0x6c);
	writel(1, b + 0x74);
	writel(0, b + 0x8c);
	writel(0, b + 0x90);
	writel(1, b + 0x54);
	ret = readl_poll_timeout(b + 0x58, status, status & 1, 20, 50000);
	if (ret) {
		r->failed = true;
		dev_err(r->dev, "rotation timed out: DMA memory and clocks retained until reboot\n");
		return ret;
	}
	writel(1, b + 0x5c);
	dma_rmb();
	drm_gem_object_put(&gem->base);
	r->front = next;
	r->ready = true;
	return 0;
}

bool soph_rot_mem(struct soph_rot *r, struct sclr_mem *mem)
{
	dma_addr_t base = r->dma[r->front];
	unsigned int row = r->rows;

	if (!r->ready)
		return false;
	memset(mem, 0, sizeof(*mem));
	if (r->angle == 90) {
		mem->addr0 = base + 2 * row;
		mem->addr1 = base + row;
		mem->addr2 = base;
		mem->start_x = row - r->height;
	} else {
		base += (r->pitch - 3 * r->width) * row;
		mem->addr0 = base;
		mem->addr1 = base + row;
		mem->addr2 = base + 2 * row;
	}
	mem->pitch_y = mem->pitch_c = 3 * row;
	mem->width = r->height;
	mem->height = r->width;
	return true;
}

void soph_rot_stage(struct soph_rot *r)
{
	if (r)
		reinit_completion(&r->latched);
}

void soph_rot_latched(struct soph_rot *r)
{
	if (r)
		complete_all(&r->latched);
}
