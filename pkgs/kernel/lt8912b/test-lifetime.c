/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EPROBE_DEFER 517
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define GPIOD_OUT_HIGH 1
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define dev_err(d, ...) ((void)(d))
#define dev_err_probe(d, e, ...) ((void)(d), (void)(e))
struct kref { unsigned int refs; };
struct list_head { bool present; };
struct drm_bridge;
struct drm_bridge_funcs { void (*destroy)(struct drm_bridge *); };
struct drm_bridge {
	struct kref refcount;
	struct list_head list;
	const struct drm_bridge_funcs *funcs;
	struct drm_bridge *next_bridge;
	void *container;
	bool freed;
};
struct device_node { unsigned int refs; };
struct device { struct device_node *of_node; };
struct gpio_desc { int unused; };
struct lt8912 {
	struct drm_bridge bridge;
	struct device *dev;
	struct device_node *host_node;
	struct gpio_desc *gp_reset;
	int data_lanes;
	bool rc_reset;
};
static int bridge_lock;
static const struct drm_bridge_funcs bridge_funcs;
static void mutex_lock(int *lock) { assert(!*lock); *lock = 1; }
static void mutex_unlock(int *lock) { assert(*lock); *lock = 0; }
static void list_del(struct list_head *list) { list->present = false; }
static void kfree(void *p)
{
	struct drm_bridge *bridge = p;
	assert(!bridge->freed);
	bridge->freed = true;
}
static void kref_get(struct kref *ref) { assert(ref->refs); ref->refs++; }
static void kref_put(struct kref *ref, void (*release)(struct kref *))
{
	assert(ref->refs);
	if (!--ref->refs)
		release(ref);
}
void drm_bridge_put(struct drm_bridge *bridge);
#include "lifetime-core.inc"

static struct device_node root, host_node, port_node;
static struct drm_bridge connector;
enum failure { NO_PORT, NO_CONNECTOR, BAD_COMPATIBLE, REGULATOR_DEFER, AFTER_PARSE };
static enum failure fail_at;
static bool of_property_read_bool(struct device_node *node, const char *property)
{ (void)node; (void)property; return false; }
static struct gpio_desc *devm_gpiod_get(struct device *dev, const char *name, int flags)
{ (void)dev; (void)name; (void)flags; return NULL; }
static struct gpio_desc *devm_gpiod_get_optional(struct device *dev, const char *name, int flags)
{ return devm_gpiod_get(dev, name, flags); }
static int drm_of_get_data_lanes_count_ep(struct device_node *node, int port, int ep,
					int min, int max)
{ (void)node; (void)port; (void)ep; (void)min; (void)max; return 4; }
static struct device_node *of_graph_get_remote_node(struct device_node *node, int port, int ep)
{
	(void)node; (void)ep;
	if (port && fail_at == NO_PORT)
		return NULL;
	struct device_node *result = port ? &port_node : &host_node;
	result->refs++;
	return result;
}
static void of_node_put(struct device_node *node)
{ if (node) { assert(node->refs); node->refs--; } }
static struct drm_bridge *of_drm_find_and_get_bridge(struct device_node *node)
{
	assert(node == &port_node);
	return fail_at == NO_CONNECTOR ? NULL : drm_bridge_get(&connector);
}
static bool of_device_is_compatible(struct device_node *node, const char *name)
{ assert(node == &port_node && !strcmp(name, "hdmi-connector")); return fail_at != BAD_COMPATIBLE; }
static int lt8912_get_regulators(struct lt8912 *lt)
{ (void)lt; return fail_at == REGULATOR_DEFER ? -EPROBE_DEFER : 0; }
#include "lifetime-driver.inc"

static void init_bridge(struct drm_bridge *bridge, unsigned int references)
{
	*bridge = (struct drm_bridge){
		.refcount.refs = references, .list.present = true,
		.funcs = &bridge_funcs, .container = bridge,
	};
}
int main(int argc, char **argv)
{
	assert(argc == 2);
	for (fail_at = NO_PORT; fail_at <= AFTER_PARSE; fail_at++) {
		/* Allocation and registration each hold a reference (checked in extractor). */
		init_bridge(&connector, 2);
		for (int attempt = 0; attempt < 8; attempt++) {
			struct device dev = { .of_node = &root };
			struct lt8912 lt = { .dev = &dev };
			init_bridge(&lt.bridge, 1);
			int ret = lt8912_parse_dt(&lt);
			if (fail_at == AFTER_PARSE) {
				assert(!ret);
				/* Probe's I2C/DSI error and remove both run this DT cleanup. */
				lt8912_put_dt(&lt);
			} else {
				assert(ret < 0);
			}
			/* Managed allocation is released after probe failure / remove. */
			drm_bridge_put(&lt.bridge);
			assert(lt.bridge.freed && !host_node.refs && !port_node.refs);
			if (connector.refcount.refs != 2 || connector.freed || !connector.list.present) {
				fprintf(stderr, "FAIL: %s path=%d attempt=%d connector references=%u (expected 2)\n",
					argv[1], fail_at, attempt + 1, connector.refcount.refs);
				return 1;
			}
		}
		/* Only its own unregister and allocation cleanup may free the connector. */
		drm_bridge_put(&connector);
		assert(!connector.freed);
		drm_bridge_put(&connector);
		assert(connector.freed && !connector.list.present);
	}
	printf("PASS: %s: 40 production LT8912B cleanup cycles preserve connector ownership; pinned DRM core frees next_bridge once\n", argv[1]);
}
