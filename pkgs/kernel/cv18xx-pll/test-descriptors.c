/* SPDX-License-Identifier: GPL-2.0 */
#include "clk-cv1800.h"
#include "clk-cv18xx-common.c"
#include "clk-cv18xx-pll.c"
#include <string.h>
/* Stub CCF registration only; use actual PLL macros and register descriptors.
 */
#undef CV1800_CLK_COMMON
#define CLK_IS_CRITICAL BIT(1)
#define CLK_IGNORE_UNUSED BIT(2)
#define CLK_GET_RATE_NOCACHE BIT(6)
#define CV1800_CLK_COMMON(name, parents, ops, flags) {.features = (flags)}
#include "descriptors.inc"

static u32 registers[0xa00 / 4];
static spinlock_t lock;
/* Independent offsets/bits from SG2000 TRM v1.0 G2/G6 tables. */
static const struct {
	struct cv1800_clk_pll *pll;
	unsigned int csr, bank;
	int bit, source;
	unsigned int ctrl;
	int pwd;
	const char *parent, *expected_parent;
} cases[] = {
    {&clk_mipimpll, 0x808, 0x800, 0, 0, 0, 0, clk_mipimpll_parent, "osc_parents"},
    {&clk_a0pll, 0x80c, 0x800, 1, 2, 0x850, 4, clk_a0pll_parent, "clk_bypass_mipimpll_parents"},
    {&clk_disppll, 0x810, 0x800, 2, 3, 0x860, 8, clk_disppll_parent, "clk_bypass_mipimpll_parents"},
    {&clk_cam0pll, 0x814, 0x800, 3, 4, 0x870, 12, clk_cam0pll_parent,
     "clk_bypass_mipimpll_parents"},
    {&clk_cam1pll, 0x818, 0x800, 4, 5, 0x880, 16, clk_cam1pll_parent,
     "clk_bypass_mipimpll_parents"},
    {&clk_mpll, 0x908, 0x900, 0, 2, 0x960, 0, clk_mpll_parent, "clk_bypass_fpll_parents"},
    {&clk_tpll, 0x90c, 0x900, 1, 3, 0x970, 4, clk_tpll_parent, "clk_bypass_fpll_parents"},
    {&clk_fpll, 0x910, 0x900, 2, 0, 0, 8, clk_fpll_parent, "osc_parents"},
};

int main(void)
{
	for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const typeof(cases[0]) *c = &cases[i];
		struct cv1800_clk_pll *p = c->pll;
		struct cv1800_clk_pll_synthesizer *s = p->pll_syn;
		struct clk_hw *hw = &p->common.hw;
		assert(p->pll_reg == c->csr);
		assert(p->pll_status.reg == c->bank + 4);
		assert(p->pll_status.shift == c->bit + 16);
		assert(p->pll_pwd.reg == c->bank && p->pll_pwd.shift == c->pwd);
		assert(!strcmp(c->parent, c->expected_parent));
		p->common.base = registers;
		p->common.lock = &lock;
		if (s) {
			assert(s->en.reg == c->bank + 0x40 && s->en.shift == c->source);
			assert(s->clk_half.reg == s->en.reg && s->clk_half.shift == 0);
			assert(s->ctrl == c->ctrl && s->set == c->ctrl + 4);
		}
		/* Integer fallback and fractional mode; both reference selections.
		 * Neighbour busy/lock bits must not affect this PLL's result.
		 */
		for (unsigned int frac = 0; frac <= !!s; frac++)
			for (unsigned int full = 0; full < 2; full++)
				for (unsigned int state = 0; state < 4; state++) {
					memset(registers, 0, sizeof(registers));
					u32 source = 0xa500003e & ~BIT(c->source);
					source |= full | (frac ? BIT(c->source) : 0);
					registers[(c->bank + 0x40) / 4] = source;
					registers[c->csr / 4] = 0xf8000080;
					u32 status = 0xffffffff & ~(BIT(c->bit) | BIT(c->bit + 16));
					if (state & 1)
						status |= BIT(c->bit);
					if (state & 2)
						status |= BIT(c->bit + 16);
					registers[(c->bank + 4) / 4] = status;
					unsigned int before = warnings;
					int ret = s ? fpll_set_rate(hw, 600000000,
								    frac ? 1500000000 : 25000000)
						    : ipll_set_rate(hw, 600000000, 25000000);
					assert(ret == (state == 2 ? 0 : -ETIMEDOUT));
					assert(warnings == before + (state != 2));
					assert((registers[c->csr / 4] & ~_PLL_ALL_FIELD_MASK) ==
					       0xf8000080);
					assert(registers[(c->bank + 4) / 4] == status);
					assert(registers[(c->bank + 0x40) / 4] == source);
					if (s) {
						assert(!fpll_set_parent(hw, 1));
						assert(registers[s->en.reg / 4] ==
						       (source | BIT(c->source)));
						assert(!fpll_set_parent(hw, 0));
						assert(registers[s->en.reg / 4] ==
						       (source & ~BIT(c->source)));
					}
				}
		printf("PASS: PLL CSR %#x descriptor, lock/update isolation, "
		       "shared preservation\n",
		       c->csr);
	}
	return 0;
}
