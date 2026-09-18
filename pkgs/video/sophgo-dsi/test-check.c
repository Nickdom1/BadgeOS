/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#define __iomem
static unsigned int readl(const void *p)
{
	(void)p;
	assert(0);
	return 0;
}
static void writel(unsigned int v, void *p)
{
	(void)v;
	(void)p;
	assert(0);
}
#include "sophgo-disp.h"
#define DRM_MODE_FLAG_PHSYNC 1
#define DRM_MODE_FLAG_NHSYNC 2
#define DRM_MODE_FLAG_PVSYNC 4
#define DRM_MODE_FLAG_NVSYNC 8
struct drm_display_mode {
	int clock, hdisplay, hsync_start, hsync_end, htotal;
	int vdisplay, vsync_start, vsync_end, vtotal;
	unsigned int flags, vscan;
};
struct drm_crtc_state {
	bool active;
	void *event;
	struct drm_display_mode adjusted_mode;
};
struct drm_plane_state {
	void *crtc;
};
struct drm_crtc {
	int unused;
};
struct drm_plane {
	int unused;
};
struct drm_atomic_commit {
	struct drm_crtc_state crtc;
	struct drm_plane_state plane;
};
static struct drm_crtc_state *drm_atomic_get_new_crtc_state(struct drm_atomic_commit *s,
							    struct drm_crtc *c)
{
	(void)c;
	return &s->crtc;
}
static struct drm_plane_state *drm_atomic_get_new_plane_state(struct drm_atomic_commit *s,
							      struct drm_plane *p)
{
	(void)p;
	return &s->plane;
}
#include "check.inc"
int main(void)
{
	struct drm_atomic_commit s = {
	    .crtc = {.adjusted_mode = {74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, 5, 0}}};
	assert(!sophgo_crtc_check(NULL, &s));
	s.crtc.event = &s;
	assert(sophgo_crtc_check(NULL, &s) == -EOPNOTSUPP);
	s.crtc.active = true;
	assert(sophgo_crtc_check(NULL, &s) == -EOPNOTSUPP);
	s.crtc.event = NULL;
	assert(sophgo_crtc_check(NULL, &s) == -EOPNOTSUPP);
	s.crtc.adjusted_mode.flags = 15;
	assert(sophgo_crtc_check(NULL, &s) == -EINVAL);
	assert(!sophgo_plane_check(NULL, &s));
	s.plane.crtc = &s;
	assert(sophgo_plane_check(NULL, &s) == -EOPNOTSUPP);
	puts("PASS: actual atomic checks reject active modes, events (including inactive), invalid "
	     "polarity and scanout before MMIO");
}
