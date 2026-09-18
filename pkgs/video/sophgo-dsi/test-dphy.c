/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define __iomem
static unsigned int regs[0xa4 / 4], offsets[64], values[64], writes, reads;
static unsigned int readl(const void *addr)
{
	reads++;
	return *(const unsigned int *)addr;
}
static void writel(unsigned int value, void *addr)
{
	assert(writes < 64);
	offsets[writes] = (const unsigned char *)addr - (const unsigned char *)regs;
	values[writes++] = value;
	*(unsigned int *)addr = value;
}
#include "sophgo-dphy.h"

static void test_lanes(void)
{
	const unsigned int golden[] = {1, 2, 0, 3, 4};
	const unsigned int two[] = {1, 5, 0, 2, 5};
	const unsigned int invalid[][5] = {
	    {1, 2, 3, 4, 5}, /* no clock */
	    {0, 5, 5, 5, 5}, /* no data */
	    {0, 1, 1, 5, 5}, /* duplicate */
	    {0, 2, 5, 5, 5}, /* hole */
	    {0, 1, 6, 5, 5}, {0, 1, UINT_MAX, 5, 5}, {0, 0, 1, 2, 3},
	};
	struct sophgo_dphy_lanes p, sentinel = {11, 22, 33, 44};

	assert(!sophgo_dphy_calc_lanes(golden, 0, 1, 0, &p));
	assert(p.map == 0x04043021 && !p.pn && p.enable == 31 && p.count == 4);
	assert(!sophgo_dphy_calc_lanes(two, 9, 0, 1, &p));
	assert(p.map == 0x00052051 && p.pn == 9 && p.enable == 0x27 && p.count == 2);
	for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		p = sentinel;
		assert(sophgo_dphy_calc_lanes(invalid[i], 0, 1, 0, &p) == -1);
		assert(!memcmp(&p, &sentinel, sizeof(p)));
	}
	assert(sophgo_dphy_calc_lanes(two, 2, 0, 0, &p) == -1); /* unused pad */
	assert(sophgo_dphy_calc_lanes(golden, 32, 0, 0, &p) == -1);
	assert(sophgo_dphy_calc_lanes(golden, 0, 2, 0, &p) == -1);
	assert(sophgo_dphy_calc_lanes(golden, 0, 0, 2, &p) == -1);
	/* Move the clock through every physical pad, all PN bits and both phases. */
	for (unsigned int pad = 0; pad < 5; pad++) {
		unsigned int roles[] = {0, 1, 2, 3, 4};
		roles[0] = pad;
		roles[pad] = 0;
		for (unsigned int phase = 0; phase <= 1; phase++) {
			assert(!sophgo_dphy_calc_lanes(roles, 31, phase, 1, &p));
			assert((p.map & 0x1f000000) == (phase << (24 + pad)));
			assert(p.pn == 31 && p.enable == 63);
		}
	}
}

static void test_pll(void)
{
	struct sophgo_dphy_pll p, sentinel = {1, 2, 3, 4, 5};
	/* _cal_pll_reg source arithmetic: 4455000 * gain4 / 10 = 1782000;
	 * divider24, loop8, floor((900000*8 << 26)/1782000) = 0x10295fad.
	 */
	for (unsigned int lanes = 1; lanes <= 4; lanes *= 2) {
		assert(!sophgo_dphy_calc_pll(74250, lanes, 24, 25000, &p));
		assert(p.txpll == (lanes == 4 ? 0x218 : lanes == 2 ? 0x118 : 0x18));
		assert(p.loop == 0x100000 && p.set == 0x10295fad && p.vip_div == 0x10);
		assert(p.preamble == (lanes == 1));
	}
	assert(!sophgo_dphy_calc_pll(10000, 2, 18, 25000, &p));
	assert(p.txpll == 0x748 && p.vip_div == 0); /* div144 -> half72 */
	assert(!sophgo_dphy_calc_pll(125000, 2, 24, 25000, &p));
	assert(!p.preamble);
	assert(!sophgo_dphy_calc_pll(125001, 2, 24, 25000, &p));
	assert(p.preamble);
	/* Additional source-derived formats, three lanes, loop branches and floors. */
	const unsigned int vectors[][7] = {
	    {74250, 3, 24, 0x220, 2, 0x183e0f83, 0x10}, {74250, 4, 16, 0x320, 2, 0x183e0f83, 0x10},
	    {74250, 4, 18, 0x212, 1, 0x158c7f91, 0x10}, {20000, 4, 30, 0x778, 2, 0x18000000, 0x10},
	    {140000, 1, 24, 0x18, 3, 0x19b6db6d, 0x10}, {230000, 1, 24, 0x18, 3, 0x0fa6f4de, 0x10},
	    {10001, 2, 18, 0x748, 1, 0x13ff7cf0, 0},
	};
	for (unsigned int i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
		const unsigned int *v = vectors[i];
		assert(!sophgo_dphy_calc_pll(v[0], v[1], v[2], 25000, &p));
		assert(p.txpll == v[3] && p.loop == (v[4] << 20));
		assert(p.set == v[5] && p.vip_div == v[6]);
	}
	assert(!sophgo_dphy_calc_pll(UINT_MAX / 300, 4, 30, 25000, &p));
	p = sentinel;
	assert(sophgo_dphy_calc_pll(UINT_MAX / 300 + 1, 4, 30, 25000, &p) == -1);
	assert(!memcmp(&p, &sentinel, sizeof(p)));
	const unsigned int invalid[][4] = {
	    {0, 4, 24, 25000},	    {1, 4, 24, 25000},
	    {74250, 0, 24, 25000},  {74250, 5, 24, 25000},
	    {74250, 4, 0, 25000},   {74250, 4, 32, 25000},
	    {74250, 4, 24, 24000},  {UINT_MAX, 4, 24, 25000},
	    {200000, 1, 24, 25000}, /* loop4 doesn't fit mask */
	    {5000, 4, 24, 25000},   /* div_sel cannot fit 0x7ff */
	};
	for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		p = sentinel;
		assert(sophgo_dphy_calc_pll(invalid[i][0], invalid[i][1], invalid[i][2],
					    invalid[i][3], &p) == -1);
		assert(!memcmp(&p, &sentinel, sizeof(p)));
	}
}

static int vip_div(void *ctx, unsigned int value)
{
	assert(ctx == regs && writes == 5); /* after lanes, before PLL */
	assert(value == 0x10);
	return 0;
}
static int vip_fail(void *ctx, unsigned int value)
{
	(void)ctx;
	(void)value;
	return -17;
}

static void test_mmio(void)
{
	const unsigned int golden[] = {1, 2, 0, 3, 4};
	const unsigned int expected[] = {0,    0x9c, 0x9c, 0xa0, 0,   0x6c,
					 0x6c, 0x90, 0x8c, 0x8c, 0x14};
	struct sophgo_dphy_lanes lanes;
	struct sophgo_dphy_pll pll;
	unsigned int hs;

	assert(!sophgo_dphy_calc_lanes(golden, 0, 1, 0, &lanes));
	assert(!sophgo_dphy_calc_pll(74250, 4, 24, 25000, &pll));
	assert(!sophgo_dphy_calc_hs(6, 32, 5, &hs));
	assert(hs == 0x05200600);
	for (unsigned int i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
		regs[i] = 0xffffffff;
	writes = reads = 0;
	assert(sophgo_dphy_set_pll(regs, &pll, NULL, regs) == -1);
	assert(sophgo_dphy_set_pll(regs, &pll, vip_fail, regs) == -17);
	assert(!writes && !reads);
	sophgo_dphy_set_lanes(regs, &lanes);
	assert(!sophgo_dphy_set_pll(regs, &pll, vip_div, regs));
	sophgo_dphy_set_hs(regs, hs);
	assert(writes == sizeof(expected) / sizeof(expected[0]));
	for (unsigned int i = 0; i < writes; i++)
		assert(offsets[i] == expected[i]);
	assert(values[0] == 0xffffffc0 && values[1] == 0xfff00000);
	assert(values[2] == 0xe4f43021 && values[3] == 0xffffffe0);
	assert(values[4] == 0xffffffdf);
	assert(values[5] == 0xffdfffff && values[6] == 0xffdffa18);
	assert(values[7] == pll.set);
	assert(values[8] == 0xfffffffe && values[9] == 0xffffffff);
	assert(values[10] == 0x052006ff);
	writes = reads = 0;
	sophgo_dphy_disable_lanes(regs);
	assert(writes == 2 && reads == 3); /* final readback */
	assert(!(regs[0] & 63) && !(regs[0x9c / 4] & 0xfffff));
	assert(!sophgo_dphy_calc_hs(255, 255, 255, &hs) && hs == 0xffffff00);
	assert(!sophgo_dphy_calc_hs(0, 0, 0, &hs) && hs == 0);
	assert(sophgo_dphy_calc_hs(256, 0, 0, &hs) == -1 && hs == 0);
	assert(sophgo_dphy_calc_hs(0, 256, 0, &hs) == -1 && hs == 0);
	assert(sophgo_dphy_calc_hs(0, 0, UINT_MAX, &hs) == -1 && hs == 0);
}

int main(void)
{
	test_lanes();
	test_pll();
	test_mmio();
	puts("PASS: production D-PHY lane/PLL/HS vectors, bounds, masks, owner failure and "
	     "ordering (offline)");
	return 0;
}
