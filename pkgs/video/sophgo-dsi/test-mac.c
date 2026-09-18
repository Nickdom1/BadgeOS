/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define __iomem
/* Linux 7.2.2 include/drm/drm_mipi_dsi.h; object gates check real definitions. */
#define MIPI_DSI_FMT_RGB888 0
#define MIPI_DSI_MODE_VIDEO (1UL << 0)
#define MIPI_DSI_MODE_VIDEO_BURST (1UL << 1)
#define MIPI_DSI_MODE_VIDEO_NO_HFP (1UL << 5)
#define MIPI_DSI_MODE_NO_EOT_PACKET (1UL << 9)
#define MIPI_DSI_MODE_LPM (1UL << 11)

/* Deterministic virtual time at the production iopoll boundary. */
#define read_poll_timeout(op, val, cond, delay, timeout, sleep, args...)                           \
	({                                                                                         \
		int result = -ETIMEDOUT;                                                           \
		assert((delay) == 1000 && (timeout) == 20000 && !(sleep));                         \
		for (unsigned int tick = 0; tick <= (timeout) / (delay); tick++) {                 \
			(val) = op(args);                                                          \
			if (cond) {                                                                \
				result = 0;                                                        \
				break;                                                             \
			}                                                                          \
		}                                                                                  \
		result;                                                                            \
	})

static unsigned int regs[3], reads, writes, delay_reads, pending_mode;
static bool stuck, pending;
static struct {
	unsigned int reg, val;
} trace[64];
static unsigned int readl(const void *addr)
{
	unsigned int off = (const unsigned int *)addr - regs;
	assert(off < 3);
	reads++;
	if (!off && pending && !stuck) {
		if (delay_reads)
			delay_reads--;
		else {
			regs[0] = (regs[0] & ~7U) | pending_mode;
			pending = false;
		}
	}
	return regs[off];
}
static void writel(unsigned int val, void *addr)
{
	unsigned int off = (unsigned int *)addr - regs;
	assert(off < 3 && writes < 64);
	trace[writes].reg = off * 4;
	trace[writes++].val = val;
	if (!off) {
		/* Source-derived RWS transaction model; not hardware acceptance. */
		assert((val & ~7U) == (regs[0] & ~7U));
		pending_mode = (regs[0] ^ val) & 7;
		pending = true;
	} else {
		assert(!(regs[0] & 0xf)); /* configuration only after completed stop */
		regs[off] = val;
	}
}
#include "sophgo-disp.h"
#include "sophgo-mac.h"

static struct sophgo_mac_mode golden(void)
{
	return (struct sophgo_mac_mode){
	    .pixel_khz = 74250,
	    .hdisplay = 1280,
	    .hsync_start = 1390,
	    .hsync_end = 1430,
	    .htotal = 1650,
	    .vdisplay = 720,
	    .vsync_start = 725,
	    .vsync_end = 730,
	    .vtotal = 750,
	    .lanes = 4,
	    .format = MIPI_DSI_FMT_RGB888,
	    .flags = SOPHGO_MAC_BRIDGE_FLAGS | MIPI_DSI_MODE_VIDEO_BURST,
	};
}
static void reset(void)
{
	memset(regs, 0, sizeof(regs));
	reads = writes = delay_reads = 0;
	stuck = pending = false;
}
static void test_calculations(void)
{
	struct sophgo_mac_mode m = golden();
	struct sophgo_mac_config c, before;
	unsigned int word = 0xdeadbeef;
	unsigned long flags = 0xfeed;

	assert(!sophgo_mac_calc(&m, &c));
	/* EoT is always emitted now, so golden flags give 0x26000000. */
	assert(c.hs0 == 0x26000000 && c.hs1 == 0x00800f00);
	assert(c.h.total == 1649 && c.h.sync == 0x00280001 && c.h.active == 0x06040105);
	assert(c.v.total == 749 && c.v.sync == 0x00050001 && c.v.active == 0x02e9001a);
	m.flags &= ~MIPI_DSI_MODE_NO_EOT_PACKET; /* no longer changes HS_0 */
	assert(!sophgo_mac_calc(&m, &c) && c.hs0 == 0x26000000);
	for (unsigned int lanes = 1; lanes <= 4; lanes++) {
		m.lanes = lanes;
		int ret = sophgo_mac_calc(&m, &c);
		if (lanes == 3)
			assert(ret == -EINVAL);
		else {
			assert(!ret && c.hs1 == 0x00800f00);
			assert(c.hs0 == (0x24000000 | ((lanes >> 1) << 24)));
		}
	}
	assert(!sophgo_mac_packet(1, &word) && word == 3);
	assert(!sophgo_mac_packet(9, &word) && word == 27);
	assert(!sophgo_mac_packet(10, &word) && word == 0x0001001e);
	assert(!sophgo_mac_packet(20479, &word) && word == 0x07ffeffd);
	const unsigned int bad_widths[] = {0, 20480, 21845, 21846, UINT32_MAX};
	for (unsigned int i = 0; i < sizeof(bad_widths) / sizeof(bad_widths[0]); i++) {
		word = 0xdeadbeef;
		assert(sophgo_mac_packet(bad_widths[i], &word) == -ERANGE && word == 0xdeadbeef);
	}
	assert(sophgo_mac_packet(1, NULL) == -ERANGE);
	assert(!sophgo_mac_negotiate(true, 4, 0, 0, SOPHGO_MAC_BRIDGE_FLAGS, &flags));
	assert(flags == golden().flags);
	flags = 0xfeed;
	assert(sophgo_mac_negotiate(false, 4, 0, 0, SOPHGO_MAC_BRIDGE_FLAGS, &flags) ==
	       -EOPNOTSUPP);
	assert(flags == 0xfeed);
	assert(sophgo_mac_negotiate(true, 2, 0, 0, SOPHGO_MAC_BRIDGE_FLAGS, &flags) == -EOPNOTSUPP);
	assert(sophgo_mac_negotiate(true, 4, 1, 0, SOPHGO_MAC_BRIDGE_FLAGS, &flags) == -EINVAL);
	assert(sophgo_mac_negotiate(true, 4, 0, 1, SOPHGO_MAC_BRIDGE_FLAGS, &flags) == -EINVAL);
	assert(sophgo_mac_negotiate(true, 4, 0, 0, golden().flags, NULL) == -EINVAL);
	assert(!sophgo_mac_negotiate(false, 4, 0, 0, golden().flags, &flags));

	memset(&before, 0xa5, sizeof(before));
	c = before;
	m = golden();
	m.htotal = UINT32_MAX;
	assert(sophgo_mac_calc(&m, &c) == -EINVAL && !memcmp(&c, &before, sizeof(c)));
	assert(sophgo_mac_calc(NULL, &c) == -EINVAL);
	assert(sophgo_mac_calc(&m, NULL) == -EINVAL);
	m = golden();
	m.hdisplay = 16381;
	m.hsync_start = 16382;
	m.hsync_end = 16383;
	m.htotal = 16384;
	assert(!sophgo_mac_calc(&m, &c) && c.h.total == 16383);
	m.htotal++;
	assert(sophgo_mac_calc(&m, &c) == -EINVAL);
}
static void test_invalid_before_mmio(void)
{
	struct sophgo_mac mac;
	struct sophgo_mac_mode m;
	reset();
	assert(sophgo_mac_init(&mac, regs, 11) == -EINVAL);
	assert(sophgo_mac_init(&mac, NULL, 12) == -EINVAL);
	assert(sophgo_mac_init(NULL, regs, 12) == -EINVAL);
	assert(!sophgo_mac_init(&mac, regs, 12));
	assert(sophgo_mac_start(&mac) == -EINVAL);
	/* Each unsupported flag must fail, including sync-pulse, automatic
	 * vertical, HBP/HSA suppression, vsync flush, non-continuous clock. */
	for (unsigned int bit = 0; bit < sizeof(unsigned long) * 8; bit++) {
		if (golden().flags & (1UL << bit))
			continue;
		m = golden();
		m.flags |= 1UL << bit;
		assert(sophgo_mac_configure(&mac, &m) == -EOPNOTSUPP);
	}
	for (unsigned int i = 0; i < 17; i++) {
		m = golden();
		switch (i) {
		case 0:
			m.lanes = 0;
			break;
		case 1:
			m.lanes = 3;
			break;
		case 2:
			m.lanes = 5;
			break;
		case 3:
			m.format = 1;
			break;
		case 4:
			m.format = UINT32_MAX;
			break;
		case 5:
			m.channel = 1;
			break;
		case 6:
			m.pixel_khz = 0;
			break;
		case 7:
			m.hdisplay = UINT32_MAX;
			break;
		case 8:
			m.hdisplay = 0;
			break;
		case 9:
			m.hsync_start = m.hdisplay;
			break;
		case 10:
			m.hsync_end = m.hsync_start;
			break;
		case 11:
			m.htotal = m.hsync_end - 1;
			break;
		case 12:
			m.vdisplay = 0;
			break;
		case 13:
			m.vtotal = UINT32_MAX;
			break;
		case 14:
			m.vsync_end = m.vsync_start - 1;
			break;
		case 15:
			m.flags &= ~MIPI_DSI_MODE_VIDEO;
			break;
		case 16:
			m.flags &= ~MIPI_DSI_MODE_VIDEO_BURST;
			break;
		}
		assert(sophgo_mac_configure(&mac, &m) < 0);
	}
	assert(sophgo_mac_configure(&mac, NULL) == -EINVAL);
	assert(!reads && !writes && !mac.configured);
}
static void test_lifecycle(void)
{
	struct sophgo_mac mac;
	struct sophgo_mac_mode m = golden();
	reset();
	assert(!sophgo_mac_init(&mac, regs, sizeof(regs)));
	/* Dirty firmware state, including unrelated packet/skew/reserved bits. */
	regs[0] = 0xa5000034;
	regs[1] = 0xffffffff;
	regs[2] = 0xffffffff;
	delay_reads = 3;
	assert(!sophgo_mac_configure(&mac, &m));
	assert(mac.configured && regs[0] == 0xa5000030);
	assert(writes == 3 && trace[0].reg == 0 && trace[0].val == 0xa5000034);
	assert(trace[1].reg == 4 && trace[2].reg == 8);
	assert(regs[1] == 0x3effffff && regs[2] == 0xf8800f00); /* EoT bit 26 set */
	delay_reads = 2;
	assert(!sophgo_mac_start(&mac) && regs[0] == 0xa5000034);
	assert(mac.start_initial == 0xa5000030 && mac.start_final == 0xa5000034);
	assert(mac.start_poll_us == 20000);
	assert(writes == 4 && trace[3].reg == 0 && trace[3].val == 0xa5000034);
	assert(sophgo_mac_start(&mac) == -EBUSY && writes == 4);
	assert(mac.start_initial == 0xa5000034 && mac.start_final == 0xa5000034);
	assert(mac.start_poll_us == 0);
	assert(!sophgo_mac_quiesce(&mac) && regs[0] == 0xa5000030);
	assert(!sophgo_mac_quiesce(&mac) && writes == 5);
	assert(!sophgo_mac_start(&mac));
	stuck = true;
	unsigned int n = reads;
	assert(sophgo_mac_configure(&mac, &m) == -ETIMEDOUT);
	assert(!mac.configured && reads - n == 22 && writes == 7);
	assert(trace[6].reg == 0); /* no packet writes after failed stop */
	assert(sophgo_mac_start(&mac) == -EINVAL);
	stuck = false;
	assert(!sophgo_mac_quiesce(&mac));
	assert(!sophgo_mac_configure(&mac, &m));
	stuck = true;
	n = reads;
	assert(sophgo_mac_start(&mac) == -ETIMEDOUT && !mac.configured);
	assert(reads - n == 22 + 256); /* Immediate burst precedes polling. */
	assert(mac.start_burst_reads == 256 && !mac.start_burst_hits);
	assert(mac.start_initial == 0xa5000030 && mac.start_final == 0xa5000030);
	assert(mac.start_poll_us == 20000);
	stuck = false;
	assert(!sophgo_mac_quiesce(&mac)); /* late start completion is stopped */
	for (unsigned int mode = 1; mode < 16; mode++) {
		if (mode == 4)
			continue;
		regs[0] = mode;
		n = writes;
		assert(sophgo_mac_configure(&mac, &m) == -EBUSY);
		assert(sophgo_mac_quiesce(&mac) == -EBUSY && writes == n);
	}
	/* the supervised mac_no_eot override clears EoT at configure time,
	 * reproducing the no-EoT HS_0 (0x22000000) for an on-silicon A/B; the default
	 * keeps the golden EoT (0x26000000). */
	reset();
	assert(!sophgo_mac_init(&mac, regs, sizeof(regs)) && mac.eot);
	assert(!sophgo_mac_configure(&mac, &m) && (regs[1] & (1u << 26)));
	mac.eot = false;
	assert(!sophgo_mac_quiesce(&mac));
	assert(!sophgo_mac_configure(&mac, &m) && !(regs[1] & (1u << 26)));
	/* mac_fire_and_forget makes a HS poll timeout non-fatal (the vendor
	 * sclr_dsi_set_mode(HS) model): sophgo_mac_start returns 0 and keeps the MAC
	 * configured so the pipeline enables the downstream, even though the MAC has
	 * not acknowledged HS; the stop still runs afterwards. Default off (unset by
	 * sophgo_mac_init) so the fatal-abort cases above keep the fatal-timeout behaviour. */
	reset();
	assert(!sophgo_mac_init(&mac, regs, sizeof(regs)) && !mac.fire_and_forget);
	mac.fire_and_forget = true;
	assert(!sophgo_mac_configure(&mac, &m));
	stuck = true;
	assert(sophgo_mac_start(&mac) == 0 && mac.configured);
	assert((mac.start_final & 0xf) != SOPHGO_MAC_VIDEO && mac.start_poll_us == 20000);
	stuck = false;
	assert(!sophgo_mac_quiesce(&mac));
	assert(sophgo_mac_quiesce(NULL) == -EINVAL);
	assert(sophgo_mac_start(NULL) == -EINVAL);
	assert(sophgo_mac_configure(NULL, &m) == -EINVAL);
}
int main(void)
{
	test_calculations();
	test_invalid_before_mmio();
	test_lifecycle();
	puts("PASS: production MAC packet/timing bounds, bridge negotiation, stopped "
	     "configuration, RWS lifecycle and timeouts");
	return 0;
}
