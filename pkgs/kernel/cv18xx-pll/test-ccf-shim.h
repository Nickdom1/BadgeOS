/* SPDX-License-Identifier: GPL-2.0 */
/* Surrounding CCF services for a two-parent, leaf clock. The no-op decisions,
 * rate commit (including discarded callback errors) and uncached query are
 * extracted verbatim from the pinned kernel. Not a full in-kernel CCF test.
 */
#define CLK_IS_CRITICAL BIT(5)
#define CLK_GET_RATE_NOCACHE BIT(0)
#define CLK_SET_RATE_UNGATE BIT(1)
#define CLK_OPS_PARENT_ENABLE BIT(2)
#define CLK_RECALC_NEW_RATES BIT(3)
#define CLK_SET_PARENT_GATE BIT(4)
#define PRE_RATE_CHANGE 1
#define POST_RATE_CHANGE 2
#define ABORT_RATE_CHANGE 3
#define NOTIFY_STOP_MASK 0x8000
struct hlist_node {
	int unused;
};
struct clk_core {
	const struct clk_ops *ops;
	struct clk_hw *hw;
	struct clk_core *parent, *new_parent, *new_child;
	const char *name;
	unsigned long flags, rate, req_rate, new_rate;
	unsigned int num_parents, prepare_count, notifier_count, new_parent_index;
};
struct clk {
	struct clk_core *core;
};
static struct clk_core osc_core = {.rate = 25000000};
static struct clk_core synth_core = {.rate = 900000000};
static unsigned int ccf_locked;
static void clk_prepare_lock(void) { assert(!ccf_locked++); }
static void clk_prepare_unlock(void) { assert(ccf_locked-- == 1); }
#define lockdep_assert_held(lock) assert(ccf_locked)
#define hlist_for_each_entry(child, head, node) for ((child) = NULL; (child);)
#define hlist_for_each_entry_safe(child, tmp, head, node)                                          \
	for ((child) = NULL, (tmp) = NULL; (child) && (tmp);)
#define pr_debug(...)                                                                              \
	do {                                                                                       \
	} while (0)
#define trace_clk_set_parent(...)                                                                  \
	do {                                                                                       \
	} while (0)
#define trace_clk_set_parent_complete(...)                                                         \
	do {                                                                                       \
	} while (0)
#define trace_clk_set_rate(...)                                                                    \
	do {                                                                                       \
	} while (0)
#define trace_clk_set_rate_complete(...)                                                           \
	do {                                                                                       \
	} while (0)
static int clk_pm_runtime_get(struct clk_core *core)
{
	(void)core;
	return 0;
}
static void clk_pm_runtime_put(struct clk_core *core) { (void)core; }
static bool clk_core_rate_is_protected(struct clk_core *core)
{
	(void)core;
	return false;
}
/* Tests do not exercise optional flags, notifiers or cross-tree reparenting. */
#define clk_core_prepare(core) assert(0)
#define clk_core_enable_lock(core) assert(0)
#define clk_core_disable_lock(core) assert(0)
#define clk_core_unprepare(core) assert(0)
#define clk_core_prepare_enable(core)                                                              \
	do {                                                                                       \
		(void)(core);                                                                      \
		assert(0);                                                                         \
	} while (0)
#define clk_core_disable_unprepare(core)                                                           \
	do {                                                                                       \
		(void)(core);                                                                      \
		assert(0);                                                                         \
	} while (0)
#define __clk_set_parent_before(core, parent)                                                      \
	((void)(core), (void)(parent), (struct clk_core *)NULL)
#define __clk_set_parent_after(core, parent, old)                                                  \
	do {                                                                                       \
		(void)(old);                                                                       \
		assert(0);                                                                         \
	} while (0)
#define __clk_notify(core, event, old, rate)                                                       \
	do {                                                                                       \
		(void)(old);                                                                       \
		assert(0);                                                                         \
	} while (0)
static struct clk_core *clk_propagate_rate_change(struct clk_core *core, unsigned long event)
{
	(void)core;
	(void)event;
	return NULL;
}
static int __clk_speculate_rates(struct clk_core *core, unsigned long rate)
{
	(void)core;
	(void)rate;
	return 0;
}
static void __clk_recalc_accuracies(struct clk_core *core) { (void)core; }
static int clk_fetch_parent_index(struct clk_core *core, struct clk_core *parent)
{
	(void)core;
	return parent == &synth_core ? 1 : parent == &osc_core ? 0 : -EINVAL;
}
static int __clk_set_parent(struct clk_core *core, struct clk_core *parent, int index)
{
	int ret = core->ops->set_parent(core->hw, index);
	if (!ret)
		core->parent = parent;
	return ret;
}
static unsigned long clk_core_req_round_rate_nolock(struct clk_core *core, unsigned long rate)
{
	struct clk_rate_request req = {.rate = rate, .best_parent_rate = core->parent->rate};
	return core->ops->determine_rate(core->hw, &req) ? 0 : req.rate;
}
static struct clk_core *clk_calc_new_rates(struct clk_core *core, unsigned long rate)
{
	core->new_rate = clk_core_req_round_rate_nolock(core, rate);
	return core->new_rate ? core : NULL;
}
#include "ccf.inc"
