/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define EPROBE_DEFER 517
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define MIPI_DSI_FMT_RGB888 1
#define MIPI_DSI_MODE_VIDEO 1
#define MIPI_DSI_MODE_LPM 2
#define MIPI_DSI_MODE_VIDEO_NO_HFP 4
#define MIPI_DSI_MODE_NO_EOT_PACKET 8
#define dev_err(d, ...) ((void)(d))
#define dev_err_probe(d, e, ...) ((void)(d), (e))
struct device {
	int unused;
};
struct mipi_dsi_host {
	int unused;
};
struct mipi_dsi_device {
	unsigned int lanes, format, mode_flags;
	bool attached;
};
struct mipi_dsi_device_info {
	const char *type;
	int channel;
	void *node;
};
struct drm_bridge {
	int unused;
};
struct lt8912 {
	struct device *dev;
	void *host_node;
	struct mipi_dsi_device *dsi;
	unsigned int data_lanes;
	struct drm_bridge bridge;
};
struct i2c_client {
	struct lt8912 *lt;
};
static struct mipi_dsi_host host;
static struct mipi_dsi_device peripheral;
static int fail_at, stage, detaches, step;
static bool registered, host_drained, dt_alive, bridge_alive;
static void (*action)(void *);
static void *action_data;
static struct mipi_dsi_host *of_find_mipi_dsi_host_by_node(void *node)
{
	(void)node;
	return ++stage == fail_at ? NULL : &host;
}
static struct mipi_dsi_device *
devm_mipi_dsi_device_register_full(struct device *dev, struct mipi_dsi_host *h,
				   const struct mipi_dsi_device_info *info)
{
	(void)dev;
	assert(h == &host && !info->channel);
	if (++stage == fail_at)
		return ERR_PTR(-ENOMEM);
	registered = true;
	return &peripheral;
}
static int mipi_dsi_attach(struct mipi_dsi_device *dsi)
{
	assert(dsi == &peripheral && registered && !dsi->attached);
	if (++stage == fail_at)
		return -EIO;
	dsi->attached = true;
	return 0;
}
static void mipi_dsi_detach(struct mipi_dsi_device *dsi)
{
	assert(dsi->attached && registered && bridge_alive && dt_alive);
	/* Production host drains operations and removes its encoder chain. */
	dsi->attached = false;
	host_drained = true;
	detaches++;
}
static int devm_add_action_or_reset(struct device *dev, void (*fn)(void *), void *data)
{
	(void)dev;
	if (++stage == fail_at) {
		fn(data);
		return -ENOMEM;
	}
	action = fn;
	action_data = data;
	return 0;
}
static void devm_release_action(struct device *dev, void (*fn)(void *), void *data)
{
	(void)dev;
	assert(step++ == 0 && action == fn && action_data == data);
	action = NULL;
	fn(data);
}
static struct lt8912 *i2c_get_clientdata(struct i2c_client *client) { return client->lt; }
static void lt8912_bridge_detach(struct drm_bridge *bridge)
{
	(void)bridge;
	assert(step++ == 1 && host_drained && bridge_alive && dt_alive);
}
static void drm_bridge_remove(struct drm_bridge *bridge)
{
	(void)bridge;
	assert(step++ == 2 && host_drained && dt_alive);
	bridge_alive = false;
}
static void lt8912_put_dt(struct lt8912 *lt)
{
	(void)lt;
	assert(step++ == 3 && host_drained && !bridge_alive);
	dt_alive = false;
}
#include "remove.inc"
int main(void)
{
	for (fail_at = 1; fail_at <= 5; fail_at++) {
		struct device dev;
		struct lt8912 lt = {.dev = &dev, .data_lanes = 4};
		struct i2c_client client = {&lt};
		stage = detaches = step = 0;
		registered = host_drained = false;
		bridge_alive = dt_alive = true;
		int ret = lt8912_attach_dsi(&lt);
		if (fail_at < 5) {
			assert(ret < 0 && !peripheral.attached && !action);
			assert(detaches == (fail_at == 4));
		} else {
			assert(!ret && action && peripheral.attached);
			lt8912_remove(&client);
			assert(step == 4 && detaches == 1 && !action);
		}
		/* Subsequent devres cleanup must not repeat mipi_dsi_detach. */
		assert(!action && !peripheral.attached);
		registered = false;
	}
	puts("PASS: production LT8912B attach/action faults; remove drains host before bridge/DT "
	     "release; no double devres detach");
}
