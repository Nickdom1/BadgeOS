/* SPDX-License-Identifier: GPL-2.0-only */
/* Execute the production syscon acquisition, divider callback and reset leaves. */
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
struct sophgo_clocks {
	bool enabled;
};
static struct device_node node;
static struct regmap map;
static unsigned int size = 0x1c, node_puts, lookups, updates, reads, phy_access;
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
	if (update_error)
		return update_error;
	m->regs[reg / 4] = (m->regs[reg / 4] & ~mask) | (value & mask);
	return 0;
}
static int regmap_read(struct regmap *m, unsigned int reg, unsigned int *value)
{
	assert(m == &map && reg <= 0x18);
	reads++;
	*value = m->regs[reg / 4];
	return read_error;
}
static unsigned int readl(const void *addr)
{
	/* The production callback must have completed the divider readback. */
	assert(updates && reads && !update_error && !read_error);
	phy_access++;
	return *(const unsigned int *)addr;
}
static void writel(unsigned int value, void *addr)
{
	assert(updates && reads && !update_error && !read_error);
	phy_access++;
	*(unsigned int *)addr = value;
}
#include "sophgo-dphy.h"
#include "sophgo-vip.h"

static void test_get(void)
{
	struct device dev = {&node};
	struct sophgo_vip vip = {0};
	struct sophgo_clocks clocks = {0};
	present = false;
	assert(sophgo_vip_get(&dev, &vip, &clocks) == -ENODEV && !node_puts);
	present = true;
	compatible = false;
	assert(sophgo_vip_get(&dev, &vip, &clocks) == -EINVAL && node_puts == 1);
	compatible = true;
	syscon = false;
	assert(sophgo_vip_get(&dev, &vip, &clocks) == -EINVAL && node_puts == 2);
	syscon = true;
	size = 0x18;
	assert(sophgo_vip_get(&dev, &vip, &clocks) == -EINVAL && node_puts == 3);
	size = 0x1c;
	address_error = -EINVAL;
	assert(sophgo_vip_get(&dev, &vip, &clocks) == -EINVAL && node_puts == 4);
	assert(!lookups && !vip.map && !updates && !phy_access);
	address_error = 0;
	const int errors[] = {-517, -ENOMEM, -ENODEV};
	for (unsigned int i = 0; i < 3; i++) {
		lookup_error = errors[i];
		assert(sophgo_vip_get(&dev, &vip, &clocks) == errors[i]);
		assert(!vip.map && !vip.clocks);
	}
	lookup_error = 0;
	assert(!sophgo_vip_get(&dev, &vip, &clocks));
	assert(vip.map == &map && vip.clocks == &clocks && node_puts == 8);
	assert(!updates && !reads && !clocks.enabled);
}

static void test_divider(void)
{
	unsigned int phy[0xa4 / 4] = {0};
	struct sophgo_dphy_pll pll;
	struct sophgo_clocks clocks = {0};
	struct sophgo_vip vip = {&map, &clocks}, absent = {0};
	assert(!sophgo_dphy_calc_pll(74250, 4, 24, 25000, &pll));
	assert(sophgo_dphy_set_pll(phy, &pll, sophgo_vip_div, NULL) == -ENODEV);
	assert(sophgo_dphy_set_pll(phy, &pll, sophgo_vip_div, &absent) == -ENODEV);
	assert(sophgo_dphy_set_pll(phy, &pll, sophgo_vip_div, &vip) == -EACCES);
	assert(!updates && !reads && !phy_access);
	clocks.enabled = true;
	assert(sophgo_vip_div(&vip, 0x20) == -EINVAL && !updates);
	update_error = -EIO;
	assert(sophgo_dphy_set_pll(phy, &pll, sophgo_vip_div, &vip) == -EIO);
	assert(!reads && !phy_access);
	update_error = 0;
	read_error = -ETIMEDOUT;
	assert(sophgo_dphy_set_pll(phy, &pll, sophgo_vip_div, &vip) == -ETIMEDOUT);
	assert(!phy_access);
	read_error = 0;
	map.regs[6] = 0xa5a5a5ef;
	assert(!sophgo_dphy_set_pll(phy, &pll, sophgo_vip_div, &vip));
	assert(last_reg == 0x18 && last_mask == 0x10 && last_value == 0x10);
	assert(map.regs[6] == 0xa5a5a5ff && phy_access == 9);
	assert(!sophgo_vip_div(&vip, 0) && map.regs[6] == 0xa5a5a5ef);
}

static void test_resets(void)
{
	struct sophgo_clocks clocks = {true};
	struct sophgo_vip vip = {&map, &clocks};
	const unsigned int masks[] = {SOPHGO_VIP_DISP_RESET, SOPHGO_VIP_MAC_RESET,
				      SOPHGO_VIP_MAC_APB_RESET, SOPHGO_VIP_PHY_APB_RESET,
				      SOPHGO_VIP_PHY_RESET};
	const unsigned int expected[] = {0x200, 0x800, 0x100000, 0x800000, 0x4000000};
	unsigned int before = updates;
	assert(sophgo_vip_reset(&vip, 0, true) == -EINVAL);
	for (unsigned int bit = 0; bit < 32; bit++) {
		if (!(0x04900a00U & (1U << bit)))
			assert(sophgo_vip_reset(&vip, 1U << bit, true) == -EINVAL);
	}
	assert(updates == before);
	for (unsigned int i = 0; i < 5; i++) {
		assert(masks[i] == expected[i]);
		map.regs[0] = 0xb26ff5ff; /* all unrelated bits are retained */
		assert(!sophgo_vip_reset(&vip, masks[i], true));
		assert(last_reg == 0 && last_mask == expected[i] && last_value == expected[i]);
		assert(map.regs[0] == (0xb26ff5ff | expected[i]));
		/* Simulate a capture client's masked update between our operations. */
		map.regs[0] ^= 4;
		assert(!sophgo_vip_reset(&vip, masks[i], false));
		assert(map.regs[0] == (0xb26ff5fb & ~expected[i]));
	}
	update_error = -EIO;
	before = reads;
	assert(sophgo_vip_reset(&vip, 0x200, true) == -EIO && reads == before);
	update_error = 0;
	read_error = -ETIMEDOUT;
	assert(sophgo_vip_reset(&vip, 0x200, false) == -ETIMEDOUT);
	read_error = 0;
	clocks.enabled = false;
	before = updates;
	assert(sophgo_vip_reset(&vip, 0x200, true) == -EACCES && updates == before);
}
int main(void)
{
	test_get();
	test_divider();
	test_resets();
	printf("PASS: shared VIP DT acquisition, masks, reset isolation, divider-before-PHY and "
	       "errors\n");
	return 0;
}
