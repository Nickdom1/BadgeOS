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
static int step, fail_at, bulk_on, mmio, idle_error, start_error;
static int round_bad, achieved_bad;
static bool idle_done;
static struct clk hw[10];
static int bt_on;
static unsigned int phy[0xcc / 4], top[0x74 / 4];
static unsigned int offsets[256], words[256], writes;
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
	int ret = fallible();
	if (!ret)
		bulk_on = 1;
	return ret;
}
static void clk_bulk_disable_unprepare(int n, struct clk_bulk_data *b)
{
	(void)b;
	assert(n == 8 && bulk_on && !bt_on);
	if (mmio)
		assert((phy[0x64 / 4] & 0x1f1f) == 0x1f1f);
	bulk_on = 0;
}
static int clk_prepare_enable(struct clk *c)
{
	assert(c == &hw[9] && bulk_on && !bt_on);
	int ret = fallible();
	if (!ret)
		bt_on = 1;
	return ret;
}
static void clk_disable_unprepare(struct clk *c)
{
	assert(c == &hw[9] && bulk_on && bt_on);
	bt_on = 0;
}
#include "sophgo-clocks.h"

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
static unsigned int readl(const void *addr)
{
	/* The production callback must have completed the divider readback. */
	assert(bulk_on && updates && reads && !update_error && !read_error);
	mmio++;
	phy_access++;
	return *(const unsigned int *)addr;
}
static void writel(unsigned int value, void *addr)
{
	assert(bulk_on && updates && reads && !update_error && !read_error);
	mmio++;
	phy_access++;
	assert(writes < ARRAY_SIZE(offsets));
	offsets[writes] = (uintptr_t)addr - (uintptr_t)phy;
	words[writes++] = value;
	*(unsigned int *)addr = value;
}
#include "sophgo-dphy.h"
#include "sophgo-vip.h"

#include "sophgo-disp.h"
#include "sophgo-link.h"

static int video(void *ctx, bool enable)
{
	(void)ctx;
	assert(bulk_on);
	if (enable) {
		assert((phy[0x44 / 4] & 0x1f1f1f1f) == 0);
		return start_error;
	}
	if (!idle_error)
		idle_done = true;
	return idle_error;
}
static struct sophgo_clocks clocks;
static struct sophgo_vip vip;
static struct sophgo_link link;
static void setup(void)
{
	memset(&clocks, 0, sizeof(clocks));
	memset(hw, 0, sizeof(hw));
	memset(phy, 0xa0, sizeof(phy));
	memset(top, 0xa0, sizeof(top));
	memset(&map, 0, sizeof(map));
	map.regs[0] = 4;
	map.regs[6] = 0xa5a5a5ef;
	for (unsigned int i = 0; i < 8; i++) {
		hw[i].rate = 25000000;
		hw[i].parent = &hw[8];
		clocks.bulk[i].clk = &hw[i];
	}
	hw[4].rate = 1188000000;
	hw[5].rate = 148500000;
	hw[5].parent = &hw[4];
	hw[6].rate = 900000000;
	hw[8].rate = 100000000;
	clocks.bt = &hw[9];
	vip = (struct sophgo_vip){.map = &map, .clocks = &clocks};
	link = (struct sophgo_link){
	    .clocks = &clocks, .vip = &vip, .phy = phy, .top = top, .video = video};
	step = fail_at = bulk_on = bt_on = mmio = idle_error = start_error = round_bad = achieved_bad = 0;
	reads = updates = phy_access = writes = 0;
	idle_done = false;
}
static int prepare(void) { return sophgo_link_prepare(&link, &hw[8], 20000000, 900000000); }
static void stopped(void)
{
	assert(!bulk_on && !bt_on && !link.phy_owned && !link.pll_held && !link.video_owned);
	for (unsigned int i = 0; i < 8; i++)
		assert(!hw[i].held);
	assert(hw[3].rate == 25000000 && hw[5].rate == 148500000);
	assert(hw[3].parent == &hw[8] && hw[5].parent == &hw[4]);
	assert(map.regs[0] == 4 && hw[4].rate == 1188000000);
}
int main(void)
{
	setup();
	/* A lane-only mask retained PD bit 13 while historical working Debian read zero.
	 * Vendor dphy_init(MIPI) writes the complete PD word for all used pads.
	 * Seed that observed residue so a lane-only 0x1f1f mask cannot pass.
	 */
	phy[0x64 / 4] = 0x2000;
	assert(!prepare());
	int stages = step;
	assert(offsets[0] == 0 && (words[0] & 0x3f) == 0);
	assert(offsets[1] == 0x9c && offsets[2] == 0x9c && offsets[3] == 0xa0);
	assert(offsets[4] == 0 && (words[4] & 0x3f) == 0x1f);
	assert(offsets[5] == 0x6c && offsets[6] == 0x6c && offsets[7] == 0x90);
	assert(offsets[8] == 0x8c && !(words[8] & 1));
	assert(offsets[9] == 0x8c && (words[9] & 1));
	assert(link.lp11_programmed && bulk_on);
	assert(phy[0x90 / 4] == 0x10295fad);
	assert((phy[0x6c / 4] & 0x3007ff) == 0x100218);
	assert((phy[0x9c / 4] & 0x1f077777) == 0x04043021);
	assert((phy[0x14 / 4] & 0xffffff00) == 0x05200600);
	/* Preparation leaves DATA_OV untouched. */
	assert(phy[0x44 / 4] == 0xa0a0a0a0);
	assert(phy[0x0c / 4] == 0x100 && phy[0x10 / 4] == 0x100);
	assert(phy[0x64 / 4] == 0 && map.regs[6] == 0xa5a5a5ff);
	/* The remaining masked analog writes preserve other fields. */
	const unsigned int analog[][2] = {
	    {0x4c, 0x001f001f}, {0x54, 0x1f1f},	 {0x50, 0x1f1f1f1f}, {0xc4, 0x1f1f},
	    {0xc8, 0x1f1f},	{0xc0, 0xfffff}, {0xb4, 1},	     {0x60, 0x10000},
	    {0x5c, 0x1f000000}, {0x88, 0xfffff}, {0x74, 0x3ff},
	};
	for (unsigned int i = 0; i < ARRAY_SIZE(analog); i++)
		assert(phy[analog[i][0] / 4] == (0xa0a0a0a0 & ~analog[i][1]));
	int before = mmio;
	assert(!prepare() && mmio == before && step == stages);
	hw[4].rate = 0;
	assert(prepare() == -ETIMEDOUT && mmio == before && bulk_on);
	assert(sophgo_link_video(&link) == -ETIMEDOUT && mmio == before);
	hw[4].rate = 1188000000;
	assert(sophgo_link_prepare(&link, &hw[8], 10000000, 900000000) == -EBUSY);
	assert(!sophgo_link_video(&link));
	before = mmio;
	assert(!sophgo_link_video(&link) && before == mmio);
	idle_error = -ETIMEDOUT;
	assert(sophgo_link_stop(&link) == -ETIMEDOUT);
	assert(bulk_on && link.phy_owned && link.video_owned && before == mmio);
	idle_error = 0;
	assert(!sophgo_link_stop(&link));
	stopped();
	before = step;
	assert(!sophgo_link_stop(&link) && step == before);
	assert(!prepare());
	assert(!sophgo_link_stop(&link));
	stopped();
	/* Every fallible CCF acquisition/configuration and VIP read/update edge.
	 * All failures precede PHY access; cleanup only restores acquired stages.
	 */
	for (int i = 1; i <= stages; i++) {
		setup();
		fail_at = i;
		assert(prepare() < 0);
		assert(!mmio);
		assert(!sophgo_link_stop(&link));
		stopped();
	}
	/* A restore failure retains protection and clocks; explicit stop finishes. */
	setup();
	assert(!prepare());
	fail_at = step + 1;
	assert(sophgo_link_stop(&link) == -EIO && bulk_on && link.pll_held);
	assert(!link.phy_owned);
	assert(!sophgo_link_stop(&link));
	stopped();
	for (int i = 1; i <= 4; i++) {
		setup();
		assert(!prepare());
		fail_at = step + i;
		assert(sophgo_link_stop(&link) == -EIO && bulk_on && !link.phy_owned);
		assert(!sophgo_link_stop(&link));
		stopped();
	}
	setup();
	vip.map = NULL;
	assert(prepare() == -ENODEV && !step && !mmio);
	setup();
	clocks.enabled = true;
	assert(prepare() == -EBUSY && !step && !mmio);
	setup();
	map.regs[0] |= SOPHGO_VIP_PHY_RESET;
	assert(prepare() == -EBUSY && !mmio && !bulk_on);
	assert(map.regs[0] == (4 | SOPHGO_VIP_PHY_RESET));
	setup();
	hw[4].rate = 0;
	assert(prepare() == -ETIMEDOUT && !mmio && !bulk_on);
	setup();
	/* MIPI does not require a particular unrelated DISPPLL frequency. */
	hw[4].rate = 594000000;
	assert(!prepare() && hw[4].rate == 594000000);
	assert(!sophgo_link_stop(&link) && hw[4].rate == 594000000);
	setup();
	/* The existing D-PHY factor is 900 MHz: half-rate must refuse before
	 * VIP/MAC/PHY access, without retuning the shared source. */
	hw[6].rate = 450000000;
	assert(prepare() == -ERANGE && !mmio && !bulk_on && !reads && !idle_done);
	assert(hw[6].rate == 450000000);
	assert(sophgo_link_prepare(&link, &hw[8], 20000000, 450000000) == -EINVAL);
	stopped();
	setup();
	assert(!prepare());
	before = mmio;
	hw[6].rate = 450000000;
	assert(sophgo_link_video(&link) == -ERANGE && mmio == before);
	hw[6].rate = 900000000;
	hw[4].rate = 594000000;
	assert(sophgo_link_video(&link) == -ETIMEDOUT && mmio == before);
	hw[4].rate = 1188000000;
	assert(!sophgo_link_stop(&link));
	stopped();
	setup();
	round_bad = 1;
	assert(prepare() == -ERANGE && !mmio && !bulk_on);
	stopped();
	setup();
	achieved_bad = 1;
	assert(prepare() == -ERANGE && !mmio && bulk_on);
	achieved_bad = 0;
	assert(!sophgo_link_stop(&link));
	stopped();
	setup();
	idle_error = -ETIMEDOUT;
	assert(prepare() == -ETIMEDOUT && !mmio && bulk_on);
	idle_error = 0;
	assert(!sophgo_link_stop(&link));
	stopped();
	setup();
	assert(!prepare());
	start_error = -EIO;
	assert(sophgo_link_video(&link) == -EIO && link.video_owned && bulk_on);
	before = mmio;
	assert(sophgo_link_video(&link) == -EBUSY && before == mmio);
	assert(!sophgo_link_stop(&link));
	stopped();
	setup();
	assert(sophgo_link_prepare(&link, &hw[8], 20000001, 900000000) == -EINVAL);
	assert(sophgo_link_prepare(&link, &hw[8], 0, 900000000) == -EINVAL);
	assert(sophgo_link_prepare(&link, &hw[8], 20000000, 123) == -EINVAL && !step);
	assert(!sophgo_link_rate_matches(1, 594000000));
	assert(sophgo_link_rate_matches(593999999, 594000000));
	puts("PASS: observed 1188/900 MHz tree, no display-divider retune, clock-to-PHY rates, LP-11 programming, all fallible stages, owned rollback, "
	     "repeated prepare/video/stop, cold/firmware failures");
	return 0;
}
