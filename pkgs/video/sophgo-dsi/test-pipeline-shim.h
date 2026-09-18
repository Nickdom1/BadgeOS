/* SPDX-License-Identifier: GPL-2.0-only */
/* Execute the production syscon acquisition, divider callback and reset leaves.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define __iomem
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
struct device_node {
	int unused;
};
struct device {
	struct device_node *of_node;
};
struct resource {
	unsigned int size;
};
struct regmap {
	unsigned int regs[7];
};
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
struct clk {
	unsigned long rate;
	struct clk *parent;
	int held;
};
struct clk_bulk_data {
	const char *id;
	struct clk *clk;
};
static int step, fail_at, bulk_on, mmio;
static int round_bad, achieved_bad;
static bool idle_done;
static struct clk hw[10];
static int bt_on;
static unsigned int phy[0xcc / 4], top[0x74 / 4];

static int fallible(void) { return ++step == fail_at ? -EIO : 0; }
static unsigned long clk_get_rate(struct clk *c) { return c->rate; }
static struct clk *clk_get_parent(struct clk *c) { return c->parent; }
static bool clk_is_match(struct clk *a, struct clk *b) { return a == b; }
static int clk_rate_exclusive_get(struct clk *c)
{
	int ret = fallible();
	if (!ret)
		c->held++;
	return ret;
}
static void clk_rate_exclusive_put(struct clk *c)
{
	assert(c->held == 1);
	c->held--;
}
static int clk_set_parent(struct clk *c, struct clk *p)
{
	assert(c->held && c == &hw[3] && idle_done);
	int ret = fallible();
	if (!ret)
		c->parent = p;
	return ret;
}
static long clk_round_rate(struct clk *c, unsigned long r)
{
	/* Only ESC is programmable on this MIPI path. In the observed tree,
	 * DISPPLL=1188 MHz would need /16 for 74.25 MHz, outside the 4-bit
	 * one-based local divider. No display-divider request is allowed. */
	assert(c == &hw[3]);
	int ret = fallible();
	return ret ? ret : (long)(round_bad ? r / 2 : r);
}
static int clk_set_rate(struct clk *c, unsigned long r)
{
	assert(c->held && c == &hw[3] && idle_done);
	int ret = fallible();
	if (!ret)
		c->rate = achieved_bad ? r / 2 : r;
	return ret;
}
static int devm_clk_bulk_get(struct device *d, int n, struct clk_bulk_data *b)
{
	(void)d;
	for (int i = 0; i < n; i++)
		b[i].clk = &hw[i];
	return fallible();
}
static int devm_add_action_or_reset(struct device *d, void (*f)(void *), void *p)
{
	(void)d;
	(void)f;
	(void)p;
	return fallible();
}
static int clk_bulk_prepare_enable(int n, struct clk_bulk_data *b)
{
	(void)b;
	assert(n == 8 && !bulk_on);
	assert_trace("CCF bulk prepare/enable");
	int ret = fallible();
	if (!ret)
		bulk_on = 1;
	return ret;
}
static void clk_bulk_disable_unprepare(int n, struct clk_bulk_data *b)
{
	(void)b;
	assert(n == 8 && bulk_on && !bt_on);
	assert_trace("CCF bulk disable");

	bulk_on = 0;
}
static int clk_prepare_enable(struct clk *c)
{
	assert(c == &hw[9] && bulk_on && !bt_on);
	assert_trace("CCF clk_bt_vip prepare/enable");
	int ret = fallible();
	if (!ret)
		bt_on = 1;
	return ret;
}
static void clk_disable_unprepare(struct clk *c)
{
	assert(c == &hw[9] && bulk_on && bt_on);
	assert_trace("CCF clk_bt_vip disable");
	bt_on = 0;
}
#include "sophgo-clocks.h"

static struct device_node node;
static struct regmap map;
static unsigned int size = 0x1c, node_puts, lookups, updates, reads;
static unsigned int last_reg, last_mask, last_value;
static int address_error, lookup_error, update_error, read_error;
static bool present = true, compatible = true, syscon = true;
static struct device_node *of_parse_phandle(struct device_node *np, const char *name, int index)
{
	assert(np == &node && !strcmp(name, "sophgo,vip-sys") && index == 0);
	return present ? &node : NULL;
}
static bool of_device_is_compatible(struct device_node *np, const char *name)
{
	assert(np == &node);
	if (!strcmp(name, "sophgo,sg2000-vip-sys"))
		return compatible;
	assert(!strcmp(name, "syscon"));
	return syscon;
}
static void of_node_put(struct device_node *np)
{
	assert(np == &node);
	node_puts++;
}
static int of_address_to_resource(struct device_node *np, int index, struct resource *res)
{
	assert(np == &node && index == 0);
	res->size = size;
	return address_error;
}
static unsigned int resource_size(struct resource *res) { return res->size; }
static struct regmap *syscon_node_to_regmap(struct device_node *np)
{
	assert(np == &node);
	lookups++;
	return lookup_error ? ERR_PTR(lookup_error) : &map;
}
static int regmap_update_bits(struct regmap *m, unsigned int reg, unsigned int mask,
			      unsigned int value)
{
	assert(m == &map && reg <= 0x18 && !(reg % 4));
	updates++;
	last_reg = reg;
	last_mask = mask;
	last_value = value;
	assert(bulk_on && reg != 0);
	int err = fallible();
	if (err)
		return err;
	if (update_error)
		return update_error;
	m->regs[reg / 4] = (m->regs[reg / 4] & ~mask) | (value & mask);
	return 0;
}
static int regmap_read(struct regmap *m, unsigned int reg, unsigned int *value)
{
	assert(m == &map && reg <= 0x18);
	assert(bulk_on);
	int err = fallible();
	if (err)
		return err;
	reads++;
	*value = m->regs[reg / 4];
	return read_error;
}
