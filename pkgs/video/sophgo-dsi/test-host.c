/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define EPROBE_DEFER 517
#define DL_FLAG_AUTOREMOVE_CONSUMER 1
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
struct device {
	int dummy;
};
struct device_node {
	int dummy;
};
struct drm_device {
	int dummy;
};
struct drm_bridge {
	int dummy;
};
struct device_link {
	int dummy;
};
struct i2c_client {
	struct device dev;
};
struct mutex {
	bool locked;
};
struct mipi_dsi_host {
	struct device *dev;
};
struct mipi_dsi_device {
	char *name;
	unsigned int lanes, format, channel;
	unsigned long mode_flags;
};
struct sophgo_dsi {
	struct mipi_dsi_host host;
	struct mutex host_lock;
	struct mipi_dsi_device *peripheral;
	struct drm_device *drm;
	struct drm_bridge *bridge;
	struct device_node *bridge_node;
};
static struct drm_bridge bridge;
static struct drm_device drm;
static struct i2c_client client;
static struct device_link link;
static bool bridge_present, client_present, link_ok;
static int bridge_refs, client_refs, links, kms_error, creates, step;
static void mutex_lock(struct mutex *m)
{
	assert(!m->locked);
	m->locked = true;
}
static void mutex_unlock(struct mutex *m)
{
	assert(m->locked);
	m->locked = false;
}
static int sophgo_mac_negotiate(bool lt, unsigned int lanes, unsigned int format,
				unsigned int channel, unsigned long flags, unsigned long *out)
{
	(void)format;
	if (!lt || channel || !lanes)
		return -EINVAL;
	*out = flags | 2;
	return 0;
}
static struct drm_bridge *of_drm_find_and_get_bridge(struct device_node *np)
{
	(void)np;
	if (!bridge_present)
		return NULL;
	bridge_refs++;
	return &bridge;
}
static void drm_bridge_put(struct drm_bridge *b) { assert(b == &bridge && bridge_refs-- == 1); }
static struct i2c_client *of_find_i2c_device_by_node(struct device_node *np)
{
	(void)np;
	if (!client_present)
		return NULL;
	client_refs++;
	return &client;
}
static void put_device(struct device *d) { assert(d == &client.dev && client_refs-- == 1); }
static struct device_link *device_link_add(struct device *c, struct device *h, int flags)
{
	(void)h;
	assert(c == &client.dev && flags == DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link_ok)
		return NULL;
	links++;
	return &link;
}
static void device_link_del(struct device_link *l) { assert(l == &link && links-- == 1); }
static int sophgo_kms_create(struct sophgo_dsi *dsi)
{
	creates++;
	if (kms_error)
		return kms_error;
	dsi->drm = &drm;
	return 0;
}
/* Detailed KMS operation/file lifetimes execute separately in test-lifetime. */
static void sophgo_kms_destroy(struct sophgo_dsi *dsi)
{
	if (dsi->drm) {
		step += 4;
		dsi->drm = NULL;
	}
}
#include "host.inc"
int main(void)
{
	struct sophgo_dsi dsi = {0};
	struct mipi_dsi_device p = {"lt8912", 4, 0, 0, 1}, other = p;
	assert(sophgo_dsi_attach(&dsi.host, &p) == -EPROBE_DEFER);
	bridge_present = true;
	assert(sophgo_dsi_attach(&dsi.host, &p) == -EPROBE_DEFER);
	client_present = true;
	assert(sophgo_dsi_attach(&dsi.host, &p) == -ENOMEM);
	link_ok = true;
	/* Includes downstream deferral, allocation/connector/registration errors.
	 * KMS internals are kernel-object checked, not emulated by this fake.
	 */
	int errors[] = {-EPROBE_DEFER, -ENOMEM, -EINVAL, -EIO};
	for (unsigned int i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
		kms_error = errors[i];
		assert(sophgo_dsi_attach(&dsi.host, &p) == kms_error);
		assert(!dsi.peripheral && !dsi.bridge && !dsi.drm);
		assert(!bridge_refs && !client_refs && !links && p.mode_flags == 1);
	}
	kms_error = 0;
	for (int i = 0; i < 3; i++) {
		step = 0;
		assert(!sophgo_dsi_attach(&dsi.host, &p));
		int before = creates;
		assert(sophgo_dsi_attach(&dsi.host, &p) == -EBUSY);
		assert(sophgo_dsi_attach(&dsi.host, &other) == -EBUSY);
		assert(sophgo_dsi_detach(&dsi.host, &other) == -EINVAL);
		assert(creates == before && p.mode_flags == 3 && links == 1);
		assert(!sophgo_dsi_detach(&dsi.host, &p));
		assert(step == 4 && !dsi.drm && !dsi.bridge && !dsi.peripheral);
		assert(!bridge_refs && !client_refs);
		device_link_del(&link); /* driver core's AUTOREMOVE_CONSUMER */
		assert(sophgo_dsi_detach(&dsi.host, &p) == -EINVAL);
		sophgo_kms_destroy(&dsi);
		assert(step == 4);
	}
	p.name = "other";
	assert(sophgo_dsi_attach(&dsi.host, &p) == -EINVAL);
	p.name = "lt8912";
	p.lanes = 3;
	assert(sophgo_dsi_attach(&dsi.host, &p) == -EINVAL);
	p.lanes = 4;
	p.channel = 1;
	assert(sophgo_dsi_attach(&dsi.host, &p) == -EINVAL);
	puts("PASS: production host arrival/defer, attach failures, duplicate attach, "
	     "detach/unplug/reference cleanup and reattach");
}
