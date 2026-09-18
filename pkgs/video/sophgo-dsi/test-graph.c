/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define __iomem
typedef unsigned int u32;
static unsigned int readl(const void *p) { return *(const unsigned int *)p; }
static void writel(unsigned int v, void *p) { *(unsigned int *)p = v; }
#include "sophgo-dphy.h"
struct property {
	const char *name;
	u32 values[5];
	int count;
};
struct device_node {
	struct property props[8];
	struct device_node *remote, *parent;
	int refs;
	bool compatible, available, reset, desk, rc_reset;
};
struct device {
	struct device_node *of_node;
};
static struct device_node host, ep, remote, bridge, supply;
static bool endpoint_present, missing_supply;
static struct device_node *get(struct device_node *np)
{
	if (np)
		np->refs++;
	return np;
}
static void of_node_put(struct device_node *np)
{
	if (np)
		assert(np->refs-- > 0);
}
static struct property *prop(struct device_node *np, const char *name)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(np->props); i++)
		if (np->props[i].name && !strcmp(np->props[i].name, name))
			return &np->props[i];
	return NULL;
}
static int of_property_count_u32_elems(struct device_node *np, const char *name)
{
	struct property *p = prop(np, name);
	return p ? p->count : -EINVAL;
}
static int of_property_read_u32_array(struct device_node *np, const char *name, u32 *out, int n)
{
	struct property *p = prop(np, name);
	if (!p || p->count < n)
		return -EINVAL;
	memcpy(out, p->values, n * sizeof(*out));
	return 0;
}
static int of_property_read_u32(struct device_node *np, const char *name, u32 *out)
{
	return of_property_read_u32_array(np, name, out, 1);
}
static struct device_node *of_graph_get_endpoint_by_regs(struct device_node *np, int port, int id)
{
	assert(np == &host && !port && id == -1);
	return endpoint_present ? get(&ep) : NULL;
}
static struct device_node *of_graph_get_remote_endpoint(struct device_node *np)
{
	return get(np->remote);
}
static struct device_node *of_graph_get_port_parent(struct device_node *np)
{
	return get(np->parent);
}
static bool of_device_is_compatible(struct device_node *np, const char *s)
{
	assert(!strcmp(s, "lontium,lt8912b"));
	return np->compatible;
}
static bool of_device_is_available(struct device_node *np) { return np->available; }
static bool of_property_present(struct device_node *np, const char *s)
{
	return !strcmp(s, "reset-gpios")	? np->reset
	       : !strcmp(s, "lontium,rc-reset") ? np->rc_reset
						: np->desk;
}
static struct device_node *of_parse_phandle(struct device_node *np, const char *s, int index)
{
	assert(np == &bridge && strstr(s, "-supply") && !index);
	return missing_supply ? NULL : get(&supply);
}
#include "sophgo-graph.h"
static void init(void)
{
	host = (struct device_node){.props = {{"sophgo,pad-roles", {1, 2, 0, 3, 4}, 5},
					      {"sophgo,pn-swap-mask", {0}, 1},
					      {"sophgo,clock-phase", {1}, 1}}};
	ep = (struct device_node){.props = {{"data-lanes", {0, 1, 2, 3}, 4}}, .remote = &remote};
	remote = (struct device_node){
	    .props = {{"data-lanes", {0, 1, 2, 3}, 4}}, .remote = &ep, .parent = &bridge};
	bridge = (struct device_node){.compatible = true, .available = true, .reset = true};
	endpoint_present = true;
	missing_supply = false;
}
static void check(int expected)
{
	struct device dev = {&host};
	struct device_node *out = NULL;
	struct sophgo_dphy_lanes config = {0};
	assert(sophgo_graph_get(&dev, &out, &config) == expected);
	if (!expected) {
		assert(out == &bridge && config.map == 0x04043021 && config.enable == 31 &&
		       !config.pn);
		of_node_put(out);
	} else
		assert(!out);
	assert(!ep.refs && !remote.refs && !bridge.refs && !supply.refs);
}
int main(void)
{
	init();
	check(0);
	endpoint_present = false;
	check(-ENODEV);
	init();
	ep.remote = NULL;
	check(-EINVAL);
	init();
	remote.remote = NULL;
	check(-EINVAL);
	init();
	remote.parent = NULL;
	check(-EINVAL);
	init();
	bridge.compatible = false;
	check(-EINVAL);
	init();
	bridge.available = false;
	check(-EINVAL);
	init();
	bridge.reset = false;
	check(-EINVAL);
	bridge.rc_reset = true;
	check(0);
	init();
	missing_supply = true;
	check(-EINVAL);
	init();
	host.desk = true;
	check(-EOPNOTSUPP);
	for (int i = 0; i < 3; i++) {
		init();
		host.props[i].name = NULL;
		check(-EINVAL);
		init();
		host.props[i].count++;
		check(-EINVAL);
		init();
		host.props[i].count--;
		check(-EINVAL);
	}
	init();
	host.props[0].values[4] = 3;
	check(-EINVAL);
	init();
	host.props[1].values[0] = 32;
	check(-EINVAL);
	init();
	host.props[2].values[0] = 2;
	check(-EINVAL);
	for (int side = 0; side < 2; side++) {
		for (int n = 0; n <= 5; n++) {
			if (n == 4)
				continue;
			init();
			(side ? remote.props : ep.props)[0].count = n;
			check(-EINVAL);
		}
		init();
		(side ? remote.props : ep.props)[0].values[0] = 1;
		check(-EINVAL);
		init();
		(side ? remote.props : ep.props)[0].name = NULL;
		check(-EINVAL);
	}
	puts("PASS: production graph parser: reciprocal endpoints, exact logical/physical lane "
	     "properties, missing reset/supply, desk refusal, balanced OF references");
}
