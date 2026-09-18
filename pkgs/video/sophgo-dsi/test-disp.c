/* SPDX-License-Identifier: GPL-2.0-only */
/* Execute the production HAL with a RAM-backed MMIO trace; no hardware. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define __iomem
static unsigned int regs[0xc4 / 4];
static unsigned int offsets[64], values[64], writes;
static unsigned int readl(const void *addr) { return *(const unsigned int *)addr; }
static void writel(unsigned int value, void *addr)
{
	assert(writes < 64);
	offsets[writes] = (unsigned char *)addr - (unsigned char *)regs;
	values[writes++] = value;
	*(unsigned int *)addr = value;
}
#include "sophgo-disp.h"
_Static_assert(sizeof(regs) == SOPHGO_DISP_SIZE, "test window covers every register used");

static void test_axes(void)
{
	struct sophgo_disp_axis a, unchanged = {123, 456, 789};

	/* SDK 6f8962c dsi_lt9611.h: 40/220/1280/110, 5/20/720/5.
	 * _fill_disp_timing at osdrv aa542c41: inclusive counters.
	 */
	assert(!sophgo_disp_calc_axis(1280, 1390, 1430, 1650, &a));
	assert(a.total == 1649 && a.sync == 0x00280001 && a.active == 0x06040105);
	assert(!sophgo_disp_calc_axis(720, 725, 730, 750, &a));
	assert(a.total == 749 && a.sync == 0x00050001 && a.active == 0x02e9001a);
	/* One pixel, one sync, no back porch, one front porch. */
	assert(!sophgo_disp_calc_axis(1, 2, 3, 3, &a));
	assert(a.total == 2 && a.sync == 0x00010001 && a.active == 0x00020002);
	/* Largest inclusive active end, and largest sync width. */
	assert(!sophgo_disp_calc_axis(16382, 16383, 16384, 16384, &a));
	assert(a.total == 16383 && a.active == 0x3fff0002);
	assert(!sophgo_disp_calc_axis(1, 2, 16384, 16384, &a));
	assert(a.sync == 0x3ffe0001 && a.active == 0x3fff3fff);
	const unsigned int invalid[][4] = {
	    {0, 2, 3, 4},	 {1, 1, 2, 3}, /* empty active / zero front porch */
	    {2, 1, 2, 3},	 {1, 2, 2, 3},	      {1, 3, 2, 4},	   {1, 2, 4, 3},
	    {1, 2, 3, 16385},	 {1, 2, 3, 0},	      {UINT_MAX, 2, 3, 4}, {1, UINT_MAX, 3, 4},
	    {1, 2, UINT_MAX, 4}, {1, 2, 3, UINT_MAX},
	};
	for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		a = unchanged;
		assert(sophgo_disp_calc_axis(invalid[i][0], invalid[i][1], invalid[i][2],
					     invalid[i][3], &a) == -1);
		assert(!memcmp(&a, &unchanged, sizeof(a)));
	}
}

/* the vendor init writes, in sclr_ctrl_init order, with the exact
 * golden values (dbg-locked: TOP_CFG0 0x80000008, TOP_CFG1 0x00ff1200,
 * DISP_CFG fmt_sel 2 / bit 18, DISP_CACHE bit 0).
 */
static void test_init(void)
{
	for (unsigned int i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
		regs[i] = 0xc000c000;
	regs[0] = 0x12345678;
	regs[0x10 / 4] = 0xc000c0c0;
	writes = 0;
	sophgo_sc_top_init(regs);
	/* Plain CFG0/CFG1 writes, then the reg_shrd_sel raw-register select
	 * (full SHD write, bit 9), then reg_done (CFG0 bit 0) and
	 * force_up (SHD low byte) as read-modify-writes preserving the other
	 * bits. The full SHD write clears the sentinel before force_up.
	 */
	assert(writes == 5);
	assert(offsets[0] == 0x00 && values[0] == 0x80000008);
	assert(offsets[1] == 0x04 && values[1] == 0x00ff1200);
	assert(offsets[2] == 0x10 && values[2] == 0x00000200); /* reg_shrd_sel: read the raw bank */
	assert(offsets[3] == 0x00 && values[3] == 0x80000009);
	assert(offsets[4] == 0x10 && values[4] == 0x000002ff); /* force_up, bit 9 preserved */
	assert(regs[0x04 / 4] == 0x00ff1200 && regs[0x10 / 4] == 0x000002ff);
	assert(regs[0x08 / 4] == 0xc000c000 && regs[0x0c / 4] == 0xc000c000);
	assert(regs[0x70 / 4] == 0xc000c000); /* the VO mux is not part of init */

	/* DISP: mask 0xf01f (vendor 0xf09f without the TGEN bit) sets format
	 * RGB planar, DRAM source and no external sync; a stale force-update
	 * command (bit 16) is never replayed; cache mode is one bit.
	 */
	regs[0] = 0xa5a5f09f;
	regs[0xc0 / 4] = 0xc000c000;
	writes = 0;
	sophgo_disp_init(regs);
	assert(writes == 2);
	assert(offsets[0] == 0x00 && values[0] == 0xa5a42080);
	assert(offsets[1] == 0xc0 && values[1] == 0xc000c001);
	assert(regs[0] == 0xa5a42080 && regs[0xc0 / 4] == 0xc000c001);
	/* Idempotent, and TGEN (bit 7, set by the sentinel) is left alone in
	 * either state: the init mask deliberately excludes it.
	 */
	writes = 0;
	sophgo_disp_init(regs);
	assert(writes == 2 && regs[0] == 0xa5a42080 && regs[0xc0 / 4] == 0xc000c001);
	regs[0] &= ~0x80U;
	sophgo_disp_init(regs);
	assert(regs[0] == 0xa5a42000);
}

static void test_registers(void)
{
	struct sophgo_disp_timing t = {.polarity = 0x60};
	const unsigned int expected_offsets[] = {
	    0, 0, 0, 0xc0, 0, 0, 4, 8, 12, 16, 20, 24, 28, 0x94, 0x98, 0x9c, 0x94, 0, 0, 0,
	};
	assert(!sophgo_disp_calc_axis(1280, 1390, 1430, 1650, &t.h));
	assert(!sophgo_disp_calc_axis(720, 725, 730, 750, &t.v));
	/* Sentinel reserved bits and a previously running timing generator. */
	for (unsigned int i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
		regs[i] = 0xc000c000;
	regs[0] = 0xa5a40080;
	regs[0x94 / 4] = 0x00010028; /* bank + unrelated bit 3 + window bg */
	writes = 0;
	sophgo_disp_enable(regs, &t, 1);
	assert(writes == sizeof(expected_offsets) / sizeof(expected_offsets[0]));
	for (unsigned int i = 0; i < writes; i++)
		assert(offsets[i] == expected_offsets[i]);
	assert(values[0] == 0xa5a40080); /* select the WORKING read bank */
	assert(values[1] == 0xa5a60080); /* mask updates */
	assert(values[2] == 0xa5a62080); /* vendor init: fmt_sel = 2, source DRAM */
	assert(values[3] == 0xc000c001); /* cache mode */
	assert(values[4] == 0xa5a62000); /* stop tgen before timing */
	assert(values[5] == 0xa5a62060);
	assert(regs[4 / 4] == (0x067102ed | 0xc000c000));
	assert(regs[8 / 4] == (0x00050001 | 0xc000c000));
	assert(regs[12 / 4] == (0x02e9001a | 0xc000c000));
	assert(regs[16 / 4] == regs[12 / 4]);
	assert(regs[20 / 4] == (0x00280001 | 0xc000c000));
	assert(regs[24 / 4] == (0x06040105 | 0xc000c000));
	assert(regs[28 / 4] == regs[24 / 4]);
	assert(regs[0x94 / 4] == 0x0701000a);
	assert(regs[0x98 / 4] == 0x03ff03ff);
	assert(regs[0x9c / 4] == 0xc000c3ff);
	assert(values[17] == 0xa5a620e0); /* tgen after timing/pattern */
	assert(values[18] == 0xa5a420e0); /* unmask */
	assert(values[19] == 0xa5a520e0); /* display-only force update */
	writes = 0;
	sophgo_disp_disable(regs);
	assert(writes == 7 && offsets[2] == 0 && values[2] == 0xa5a62060);
	assert(values[6] == 0xa5a52060);
	assert(regs[0x94 / 4] == 0x07010008);
	/* Configuring timing/pattern alone must never restart tgen. */
	t.polarity = 0;
	writes = 0;
	sophgo_disp_set_timing(regs, &t);
	sophgo_disp_pattern(regs, 1);
	assert(regs[0] == 0xa5a42000);
	sophgo_disp_pattern(regs, 0);
	assert(regs[0x94 / 4] == 0x07010008);
}

int main(void)
{
	/* Only parallel-VO mux low nibble may change; no global TOP update. */
	regs[0x70 / 4] = 0xdeadbeef;
	sophgo_disp_mux_dsi(regs);
	assert(writes == 1 && offsets[0] == 0x70 && values[0] == 0xdeadbee0);
	test_axes();
	test_init();
	test_registers();
	puts("PASS: production display timing vectors, bounds, masks, vendor scaler-top/DISP "
	     "init values and MMIO ordering (offline)");
	return 0;
}
