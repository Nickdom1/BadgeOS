/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define GFP_KERNEL 0
#define DRM_FORMAT_XRGB8888 0
#define DRM_PLANE_TYPE_PRIMARY 0
#define DRM_MODE_ENCODER_DSI 0
#define DRM_MODE_CONNECTOR_HDMIA 0
#define DRM_BRIDGE_ATTACH_NO_CONNECTOR 1
struct mutex {
	int dummy;
};
static void mutex_init(struct mutex *m) { (void)m; }
struct drm_connector {
	int dummy;
};
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(n) ((void *)(intptr_t)(n))
typedef unsigned int u32;
struct drm_device {
	void *dev_private;
	struct {
		const void *funcs;
		unsigned int min_width, min_height, max_width, max_height;
	} mode_config;
};
struct drm_plane {
	int dummy;
};
struct drm_crtc {
	int dummy;
};
struct drm_encoder {
	unsigned int possible_crtcs;
};
struct sophgo_dsi {
	struct drm_device *drm;
	void *bridge;
	struct {
		void *dev;
	} host;
};
struct sophgo_kms {
	struct mutex release_lock;
	struct drm_connector connector;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct sophgo_dsi *dsi;
};
static int sophgo_dsi_driver, sophgo_mode_config_funcs, sophgo_plane_funcs, sophgo_plane_helpers;
static int sophgo_crtc_funcs, sophgo_crtc_helpers, sophgo_encoder_funcs;
static int sophgo_connector_funcs, sophgo_connector_helpers;
static int fail_at, stage, refs, configs, attached, registered, cleaned, unplugged;
static struct drm_device drm;
static struct sophgo_kms kms;
static int result(void) { return ++stage == fail_at ? -ENOMEM : 0; }
static struct drm_device *drm_dev_alloc(const void *driver, void *dev)
{
	(void)driver;
	(void)dev;
	if (result())
		return ERR_PTR(-ENOMEM);
	refs++;
	return &drm;
}
static void *drmm_kzalloc(struct drm_device *d, size_t n, int flags)
{
	(void)flags;
	assert(d == &drm && n == sizeof(kms));
	return result() ? NULL : &kms;
}
static int drmm_mode_config_init(struct drm_device *d)
{
	assert(d == &drm);
	int ret = result();
	if (!ret)
		configs++;
	return ret;
}
static void drm_mode_config_cleanup(struct drm_device *d)
{
	assert(d == &drm && configs-- == 1);
	attached = 0;
	cleaned++;
}
static void drm_dev_put(struct drm_device *d)
{
	assert(d == &drm && refs-- > 0);
	if (!refs && configs)
		drm_mode_config_cleanup(d);
}
static int drm_universal_plane_init(struct drm_device *d, struct drm_plane *p, int mask,
				    const void *funcs, const u32 *formats, size_t n,
				    const void *mods, int type, const char *name)
{
	(void)d;
	(void)p;
	(void)mask;
	(void)funcs;
	(void)formats;
	(void)n;
	(void)mods;
	(void)type;
	(void)name;
	return result();
}
static void drm_plane_helper_add(struct drm_plane *p, const void *h)
{
	(void)p;
	(void)h;
}
static int drm_crtc_init_with_planes(struct drm_device *d, struct drm_crtc *c, struct drm_plane *p,
				     void *cursor, const void *f, const char *name)
{
	(void)d;
	(void)c;
	(void)p;
	(void)cursor;
	(void)f;
	(void)name;
	return result();
}
static void drm_crtc_helper_add(struct drm_crtc *c, const void *h)
{
	(void)c;
	(void)h;
}
static int drm_encoder_init(struct drm_device *d, struct drm_encoder *e, const void *f, int type,
			    const char *name)
{
	(void)d;
	(void)e;
	(void)f;
	(void)type;
	(void)name;
	return result();
}
static int drm_crtc_mask(struct drm_crtc *c)
{
	assert(c == &kms.crtc);
	return 1;
}
static int drm_bridge_attach(struct drm_encoder *e, void *b, void *prev, int flags)
{
	(void)b;
	assert(e == &kms.encoder && !prev && flags == DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	int ret = result();
	if (!ret)
		attached++;
	return ret;
}
static void drm_mode_config_reset(struct drm_device *d) { assert(d == &drm && attached == 1); }
static int drm_dev_register(struct drm_device *d, int flags)
{
	assert(d == &drm && !flags && attached == 1);
	int ret = result();
	if (!ret)
		registered++;
	else
		refs++; /* file opened the primary minor before registration failed */
	return ret;
}
static void sophgo_kms_destroy(struct sophgo_dsi *dsi)
{
	unplugged++;
	attached = 0;
	kms.dsi = NULL;
	drm_dev_put(dsi->drm);
	dsi->drm = NULL;
}
static int drm_connector_init(struct drm_device *d, struct drm_connector *c, const void *f,
			      int type)
{
	assert(d == &drm && c == &kms.connector && f == &sophgo_connector_funcs);
	(void)type;
	return result();
}
static void drm_connector_helper_add(struct drm_connector *c, const void *h)
{
	assert(c == &kms.connector && h == &sophgo_connector_helpers);
}
static int drm_connector_attach_encoder(struct drm_connector *c, struct drm_encoder *e)
{
	assert(c == &kms.connector && e == &kms.encoder);
	return result();
}
static int sophgo_pipeline_attach(struct sophgo_dsi *dsi, struct drm_encoder *encoder)
{
	return drm_bridge_attach(encoder, dsi->bridge, NULL, DRM_BRIDGE_ATTACH_NO_CONNECTOR);
}
#include "kms.inc"
int main(void)
{
	for (fail_at = 1; fail_at <= 11; fail_at++) {
		struct sophgo_dsi dsi = {0};
		stage = cleaned = unplugged = 0;
		int ret = sophgo_kms_create(&dsi);
		if (fail_at == 10) {
			assert(ret == -ENOMEM && !dsi.drm && refs == 1 && configs == 1 &&
			       !attached && unplugged == 1 && !cleaned);
			drm_dev_put(&drm); /* retained file closes after failed probe */
		}
		if (fail_at <= 10) {
			assert(ret == -ENOMEM && !dsi.drm && !refs && !configs && !attached &&
			       !registered);
			assert(cleaned == (fail_at > 3));
		} else {
			assert(!ret && dsi.drm == &drm && refs == 1 && configs == 1 &&
			       attached == 1 && registered == 1);
			drm_dev_put(dsi.drm);
			registered = 0;
		}
	}
	puts("PASS: production KMS creation: allocation, mode config, plane, CRTC, encoder, "
	     "downstream attach and registration failure unwind; connector owned by DRM");
}
