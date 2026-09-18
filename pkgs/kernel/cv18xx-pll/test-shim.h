/* SPDX-License-Identifier: GPL-2.0 */
/* Userspace MMIO/CCF shim; the algorithm and callbacks come from Linux. */
#ifndef TEST_SHIM_H
#define TEST_SHIM_H
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef unsigned int spinlock_t;
#define __iomem
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) ((~0UL >> (63 - (h))) & (~0UL << (l)))
#define FIELD_GET(m, v) (((v) & (m)) >> __builtin_ctzl(m))
#define FIELD_PREP(m, v) (((v) << __builtin_ctzl(m)) & (m))
#define U32_MAX UINT32_MAX
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define do_div(n, d) ((n) /= (d))
#define DIV64_U64_ROUND_CLOSEST(n, d) (((n) + (d) / 2) / (d))
static inline u64 div64_u64_rem(u64 n, u64 d, u64 *r)
{
	*r = n % d;
	return n / d;
}
struct clk_hw {
	int unused;
};
struct clk_rate_request {
	unsigned long rate, best_parent_rate;
};
struct clk_ops {
	void (*disable)(struct clk_hw *);
	int (*enable)(struct clk_hw *);
	int (*is_enabled)(struct clk_hw *);
	unsigned long (*recalc_rate)(struct clk_hw *, unsigned long);
	int (*determine_rate)(struct clk_hw *, struct clk_rate_request *);
	int (*set_rate)(struct clk_hw *, unsigned long, unsigned long);
	int (*set_rate_and_parent)(struct clk_hw *, unsigned long, unsigned long, u8);
	int (*set_parent)(struct clk_hw *, u8);
	u8 (*get_parent)(struct clk_hw *);
};
static unsigned int writes, locked, warnings;
#define WARN_ON(condition)                                                                         \
	do {                                                                                       \
		if (condition)                                                                     \
			warnings++;                                                                \
	} while (0)
#define spin_lock_irqsave(l, f)                                                                    \
	do {                                                                                       \
		(void)(l);                                                                         \
		(f) = 0;                                                                           \
		assert(!locked++);                                                                 \
	} while (0)
#define spin_unlock_irqrestore(l, f)                                                               \
	do {                                                                                       \
		(void)(l);                                                                         \
		(void)(f);                                                                         \
		assert(locked-- == 1);                                                             \
	} while (0)
/* Optional ordered trace with edge and W1T hypothesis models.
 * Neither model establishes physical silicon semantics; see latch-contract.md.
 */
static bool trace_mmio, latch_w1t;
static struct {
	const void *addr;
	u32 value;
	bool write;
	unsigned int lock_depth;
} mmio_trace[128];
static unsigned int mmio_count, latch_events;
static u32 *latch_ctrl, *latch_set, latched_factor;
static inline void record_mmio(const void *p, u32 v, bool write)
{
	if (!trace_mmio)
		return;
	assert(mmio_count < sizeof(mmio_trace) / sizeof(mmio_trace[0]));
	mmio_trace[mmio_count].addr = p;
	mmio_trace[mmio_count].value = v;
	mmio_trace[mmio_count].write = write;
	mmio_trace[mmio_count++].lock_depth = locked;
}
static inline u32 readl(const void *p)
{
	u32 v = *(const u32 *)p;
	record_mmio(p, v, false);
	return v;
}
static inline void writel(u32 v, void *p)
{
	assert(locked == 1);
	writes++;
	record_mmio(p, v, true);
	if (trace_mmio && p == latch_ctrl) {
		bool update = latch_w1t ? (v & BIT(0)) : ((*(u32 *)p ^ v) & BIT(0));
		if (update) {
			latched_factor = *latch_set;
			latch_events++;
		}
		/* W1T countermodel: keep the seeded readback, including high. */
		if (latch_w1t)
			v = (v & ~BIT(0)) | (*(u32 *)p & BIT(0));
	}
	*(u32 *)p = v;
}
#define readl_relaxed_poll_timeout(addr, v, cond, delay, timeout)                                  \
	((void)(delay), (void)(timeout), (v) = readl(addr), (cond) ? 0 : -ETIMEDOUT)
#endif
