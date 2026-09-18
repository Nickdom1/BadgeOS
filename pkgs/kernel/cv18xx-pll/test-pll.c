/* SPDX-License-Identifier: GPL-2.0 */
#include "clk-cv18xx-common.c"
#include "clk-cv18xx-pll.c"
/* Extracted verbatim from patched clk-cv1800.c by test.nix. */
#include "clk-cv1800.h"
#include "clk-cv18xx-dsi.h"
#include "limits.inc"

static u32 registers[64];
static spinlock_t lock;
static struct cv1800_clk_pll pll = {
    .common = {.base = registers, .lock = &lock},
    .pll_reg = 8,
    .pll_pwd = CV1800_CLK_BIT(16, 0),
    .pll_status = CV1800_CLK_BIT(12, 18),
    .pll_limit = pll_limits,
    .pll_syn = &clk_disppll_synthesizer,
};

/* Independent wide-integer rational oracle, not the driver's division split. */
static unsigned long oracle(unsigned long parent, u32 val, u32 factor, bool full)
{
	__uint128_t n = (__uint128_t)parent * PLL_GET_DIV_SEL(val) * (1UL << 26);
	u64 d = (u64)factor * PLL_GET_PRE_DIV_SEL(val) * PLL_GET_POST_DIV_SEL(val);

	if (!full)
		d *= 2;
	return (n + d / 2) / d;
}

static void rate_vector(unsigned long parent, unsigned long wanted, bool full)
{
	struct clk_hw *hw = &pll.common.hw;
	struct clk_rate_request req = {.rate = wanted, .best_parent_rate = parent};
	u32 sentinel = 0xa8000040 | (full ? BIT(0) : 0);
	unsigned long actual;

	registers[0] = sentinel;
	assert(!fpll_set_parent(hw, 1));
	assert(registers[0] == (sentinel | BIT(3)));
	assert(fpll_get_parent(hw) == 1);
	writes = 0;
	assert(!fpll_determine_rate(hw, &req));
	assert(!writes);
	assert(req.rate > 0 && req.rate <= wanted);
	registers[2] = 0xf8000000;
	registers[3] = BIT(18);
	assert(!fpll_set_rate(hw, req.rate, parent));
	actual = fpll_recalc_rate(hw, parent);
	assert(actual == req.rate);
	assert(actual == oracle(parent, registers[2], registers[1], full));
	assert((registers[2] & ~_PLL_ALL_FIELD_MASK) == 0xf8000000);
	assert(registers[0] == (sentinel | BIT(3)));
	assert(!fpll_set_parent(hw, 0));
	assert(registers[0] == sentinel);
	assert(!fpll_get_parent(hw));
	printf("PASS: parent=%lu request=%lu actual=%lu full=%d\n", parent, wanted, actual, full);
}

#include "test-ccf-shim.h"

static void ccf_readiness(void)
{
	struct clk_core core = {
	    .ops = &cv1800_clk_fpll_ops,
	    .hw = &pll.common.hw,
	    .parent = &osc_core,
	    .num_parents = 2,
	    .flags = TEST_DISPPLL_FLAGS,
	};
	struct clk consumer = {.core = &core};

	for (unsigned int firmware = 0; firmware < 2; firmware++) {
		for (unsigned int dsi = 0; dsi < 2; dsi++) {
			/* Other G2 synthesizers, reference and reserved bits survive. */
			u32 other = 0xa5000035 | (dsi ? BIT(1) : 0);
			registers[0] = other | (firmware ? BIT(3) : 0);
			registers[3] = BIT(18);
			registers[4] = 0;
			core.parent = firmware ? &synth_core : &osc_core;
			if (firmware) {
				assert(!fpll_set_rate(&pll.common.hw, 594000000, synth_core.rate));
				core.rate = fpll_recalc_rate(&pll.common.hw, synth_core.rate);
			}
			writes = 0;
			clk_prepare_lock();
			assert(!clk_core_set_parent_nolock(&core, &synth_core));
			assert(registers[0] == (other | BIT(3)));
			assert(!clk_core_set_rate_nolock(&core, 594000000));
			clk_prepare_unlock();
			if (firmware)
				assert(!writes);
			unsigned long rate = clk_get_rate(&consumer);
			assert(rate && rate <= 594000000);

			writes = 0;
			clk_prepare_lock();
			assert(!clk_core_set_parent_nolock(&core, &synth_core));
			assert(!clk_core_set_rate_nolock(&core, rate));
			clk_prepare_unlock();
			assert(!writes && clk_get_rate(&consumer) == rate);

			/* Lock lost with a cached good rate: both CCF setters still
			 * succeed without calling the provider. Query must fail.
			 */
			registers[3] = 0;
			clk_prepare_lock();
			assert(!clk_core_set_parent_nolock(&core, &synth_core));
			assert(!clk_core_set_rate_nolock(&core, rate));
			clk_prepare_unlock();
			assert(!writes && !clk_get_rate(&consumer));

			/* Real clk_change_rate discards provider -ETIMEDOUT. */
			unsigned int before = warnings;
			clk_prepare_lock();
			assert(!clk_core_set_rate_nolock(&core, 297000000));
			clk_prepare_unlock();
			assert(writes && warnings == before + 1);
			assert(!clk_get_rate(&consumer));
			registers[3] = BIT(18);
			assert(clk_get_rate(&consumer));
			registers[3] = BIT(2); /* update-busy is NOT lock */
			assert(!clk_get_rate(&consumer));
			registers[3] = BIT(18) | BIT(2); /* stale lock during update */
			assert(!clk_get_rate(&consumer));
			assert(fpll_set_rate(&pll.common.hw, 297000000, synth_core.rate) ==
			       -ETIMEDOUT);
			registers[3] = BIT(18);
			registers[4] = BIT(0);
			assert(!clk_get_rate(&consumer));
			registers[4] = 0;
			assert(registers[0] == (other | BIT(3)));

			/* Deselect only DISPPLL; direct repeated initialization is
			 * idempotent as well as CCF's no-op path.
			 */
			assert(!fpll_set_parent(&pll.common.hw, 0));
			assert(registers[0] == other);
			writes = 0;
			assert(!fpll_set_parent(&pll.common.hw, 0));
			assert(!writes);
		}
	}
	/* Readiness also applies to the integer oscillator parent. */
	core.parent = &osc_core;
	registers[3] = BIT(18);
	assert(!fpll_set_rate(&pll.common.hw, 600000000, osc_core.rate));
	assert(clk_get_rate(&consumer) == 600000000);
	registers[3] = 0;
	assert(!clk_get_rate(&consumer));
	/* Other shared synthesizers retain arithmetic while unlocked. */
	struct cv1800_clk_pll_synthesizer other_syn = clk_disppll_synthesizer;
	other_syn.report_lock = false;
	other_syn.en.shift = 2;
	pll.pll_syn = &other_syn;
	registers[3] = 0;
	registers[0] = BIT(2);
	writes = 0;
	assert(fpll_recalc_rate(&pll.common.hw, 900000000));
	assert(!fpll_set_parent(&pll.common.hw, 1));
	assert(!writes && registers[0] == BIT(2));
	pll.pll_syn = &clk_disppll_synthesizer;
	puts("PASS: pinned CCF no-op parent/rate, ignored set_rate timeout, uncached "
	     "lock "
	     "readiness/recovery, firmware/cold mode, DSI/shared-bit preservation");
}

static void dsi_source(void)
{
	/* Exercise the production callbacks and provider-locked RMW, with
	 * firmware gate on/off and all combinations of the other G2 users.
	 */
	u32 mmio[0x844 / 4] = {0};
	struct cv1800_clk_common source = {.base = mmio, .lock = &lock};
	struct clk_hw *hw = &source.hw;
	for (u32 bits = 0; bits < 64; bits++) {
		u32 seed = 0xa5000000 | bits;
		mmio[0x840 / 4] = seed;
		assert(!!cv1800_dsi_src_ops.is_enabled(hw) == !!(bits & BIT(1)));
		assert(!cv1800_dsi_src_ops.enable(hw));
		assert(mmio[0x840 / 4] == (seed | BIT(1)));
		assert(!cv1800_dsi_src_ops.enable(hw));
		assert(mmio[0x840 / 4] == (seed | BIT(1)));
		writes = 0;
		assert(cv1800_dsi_src_ops.recalc_rate(hw, 900000000) ==
		       (bits & BIT(0) ? 900000000 : 450000000));
		assert(cv1800_dsi_src_ops.recalc_rate(hw, 800000000) ==
		       (bits & BIT(0) ? 800000000 : 400000000));
		assert(!writes);
		cv1800_dsi_src_ops.disable(hw);
		assert(mmio[0x840 / 4] == (seed & ~BIT(1)));
		assert(!cv1800_dsi_src_ops.is_enabled(hw));
	}
	assert(!cv1800_dsi_src_ops.set_rate && !cv1800_dsi_src_ops.set_parent);
	puts("PASS: separate DSI source gate, cold/firmware enable, half/full MIPIMPLL rate, "
	     "read-only rate and preserved G2 users on unwind");
}

static void synthesizer_latch(void)
{
	struct clk_hw *hw = &pll.common.hw;

	latch_ctrl = &registers[5];
	latch_set = &registers[1];
	/* All G2 low bits include both cold and firmware-selected parents,
	 * both reference rates, DSI, and unrelated synthesizer enables.
	 */
	for (u32 bits = 0; bits < 64; bits++) {
		for (u32 ctrl = 0; ctrl < 4; ctrl++) {
			u32 seed = 0xa5000000 | bits;
			u32 control = 0x5a000080 | (ctrl & 1) | ((ctrl & 2) ? BIT(6) : 0);
			registers[0] = seed;
			registers[3] = BIT(18);
			registers[4] = 0xa5000001; /* powered down */
			registers[5] = control;
			registers[6] = 0x12345678; /* unrelated synthesizer control */
			registers[7] = 0x87654321; /* unrelated synthesizer setting */
			mmio_count = latch_events = 0;
			trace_mmio = true;
			assert(!fpll_set_parent(hw, 1));
			assert(registers[0] == (seed | BIT(3)));
			for (unsigned int update = 0; update < 2; update++) {
				unsigned int start = mmio_count, count = 0;
				const void *expected[] = {latch_set, latch_ctrl, &registers[2]};
				/* A different setting must latch on each edge, even with
				 * a pre-existing lock indication and either initial SYN_UP.
				 */
				assert(
				    !fpll_set_rate(hw, update ? 297000000 : 594000000, 900000000));
				for (unsigned int i = start; i < mmio_count; i++) {
					if (!mmio_trace[i].write)
						continue;
					assert(count < 3 &&
					       mmio_trace[i].addr == expected[count++]);
					assert(mmio_trace[i].lock_depth == 1);
				}
				assert(count == 3);
				assert(latch_events == update + 1 && latched_factor == *latch_set);
				assert(registers[5] == (control ^ (update ? 0 : BIT(0))));
				assert(registers[4] == 0xa5000001); /* rate never powers up */
			}
			assert(!pll_enable(hw));
			assert(registers[4] == 0xa5000000);
			pll_disable(hw);
			assert(registers[4] == 0xa5000001);
			assert(!fpll_set_parent(hw, 0));
			assert(registers[0] == (seed & ~BIT(3)));
			assert(registers[5] == control);
			assert(registers[6] == 0x12345678 && registers[7] == 0x87654321);
			/* Oscillator fallback must not touch the fractional latch. */
			assert(!fpll_set_rate(hw, 600000000, 25000000));
			assert(latch_events == 2 && registers[5] == control);
			trace_mmio = false;
		}
	}
	puts(
	    "PASS: ordered source/fraction/toggle/CSR/power, both SYN_UP edges and fix_div states, "
	    "repeated latch, all G2 states, integer fallback and unwind preservation");
}

/* Exercise the actual provider against the competing access interpretation.
 * A green return with stale lock can coexist with zero modeled updates.
 */
static void synthesizer_w1t_countermodel(void)
{
	latch_w1t = trace_mmio = true;
	for (u32 readback = 0; readback < 2; readback++) {
		registers[0] = BIT(3);
		registers[3] = BIT(18); /* stale lock, no update busy */
		registers[5] = 0x5a0000c0 | readback;
		mmio_count = latch_events = 0;
		latched_factor = 0xdeadbeef;
		for (unsigned int update = 0; update < 2; update++) {
			assert(!fpll_set_rate(&pll.common.hw, update ? 297000000 : 594000000,
					      900000000));
			assert(latch_events == (readback ? 0 : update + 1));
			assert(latched_factor == (readback ? 0xdeadbeef : *latch_set));
			assert(registers[5] == (0x5a0000c0 | readback));
		}
	}
	latch_w1t = trace_mmio = false;
	puts("PASS: W1T countermodel: XOR repeats at readback 0, misses at readback 1 despite "
	     "lock; silicon unresolved");
}

int main(void)
{
	dsi_source();
	const unsigned long rates[] = {6000,	  74250000,  148500000,	   297000000,
				       594000000, 891000000, 30000000000UL};
	struct clk_hw *hw = &pll.common.hw;
	struct clk_rate_request req = {.rate = 0, .best_parent_rate = 900000000};
	u32 factor, val = 0;
	unsigned long actual, wanted;

	assert(TEST_DISPPLL_STATUS_SHIFT == pll.pll_status.shift);
	assert(clk_disppll_synthesizer.en.reg == 0x840);
	assert(clk_disppll_synthesizer.en.shift == 3);
	assert(clk_disppll_synthesizer.clk_half.reg == 0x840);
	assert(clk_disppll_synthesizer.clk_half.shift == 0);
	/* Relocate the real descriptor's MMIO into our small fake register file. */
	clk_disppll_synthesizer.en.reg = 0;
	clk_disppll_synthesizer.clk_half.reg = 0;
	clk_disppll_synthesizer.set = 4;
	assert(clk_disppll_synthesizer.ctrl == 0x860);
	clk_disppll_synthesizer.ctrl = 20;

	for (unsigned int full = 0; full < 2; full++)
		for (unsigned int i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
			rate_vector(900000000, rates[i], full);

	/* Boundary search, including an unachievable request and both endpoints. */
	for (unsigned int full = 0; full < 2; full++) {
		val = PLL_SET_PRE_DIV_SEL(0, 1);
		val = PLL_SET_POST_DIV_SEL(val, 1);
		val = PLL_SET_DIV_SEL(val, 6);
		wanted = oracle(900000000, val, U32_MAX, full);
		assert(!fpll_find_synthesizer(900000000, wanted - 1, 1, 6, 1, full, &factor));
		actual = fpll_find_synthesizer(900000000, wanted, 1, 6, 1, full, &factor);
		assert(actual == wanted && oracle(900000000, val, factor, full) == actual);
		wanted = oracle(900000000, val, PLL_SYN_FACTOR_MINIMUM, full);
		assert(fpll_find_synthesizer(900000000, wanted + 1, 1, 6, 1, full, &factor) ==
		       wanted);
		assert(factor == PLL_SYN_FACTOR_MINIMUM);
	}
	registers[0] = BIT(3);
	writes = 0;
	assert(fpll_determine_rate(hw, &req) == -EINVAL);
	assert(fpll_set_rate(hw, 0, 900000000) == -EINVAL);
	assert(fpll_set_rate(hw, 1, 900000000) == -EINVAL);
	assert(fpll_set_rate(hw, 74250000, 0) == -EINVAL);
	assert(fpll_set_parent(hw, 2) == -EINVAL);
	assert(!writes);
	registers[3] = 0;
	assert(fpll_set_rate(hw, 74250000, 900000000) == -ETIMEDOUT);
	assert(registers[0] == BIT(3)); /* DSI source gate remains off */
	registers[0] = 0;
	writes = 0;
	assert(fpll_set_rate(hw, 0, 25000000) == -EINVAL);
	assert(!writes);
	assert(fpll_set_rate(hw, 600000000, 25000000) == -ETIMEDOUT);
	assert(warnings == 2);
	registers[3] = BIT(18);
	assert(!fpll_set_rate(hw, 600000000, 25000000));
	assert(fpll_recalc_rate(hw, 25000000) == 600000000);
	registers[2] = 0;
	assert(!fpll_recalc_rate(hw, 25000000));
	registers[0] = BIT(3);
	assert(!fpll_recalc_rate(hw, 900000000));
	ccf_readiness();
	synthesizer_latch();
	synthesizer_w1t_countermodel();
	puts("PASS: boundary factors, invalid rates/parents, locked RMW, timeout "
	     "propagation, "
	     "integer fallback");
	return 0;
}
