/* SPDX-License-Identifier: GPL-2.0-only */
/* Execute the production lifecycle with fault injection at CCF/devres edges.
 * Partial bulk prepare/enable rollback is CCF's contract, not emulated here.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
struct clk;
struct device {
	int unused;
};
struct clk_bulk_data {
	const char *id;
	void *clk;
};
static unsigned long pll_rate = 594000000;
static unsigned int rate_reads;
static unsigned long clk_get_rate(void *clk)
{
	assert(clk && !strcmp(((struct clk_bulk_data *)clk)->id, "disppll"));
	rate_reads++;
	return pll_rate;
}
static int get_error, action_error, enable_error;
static unsigned int gets, enables, disables, actions;
static unsigned int cfg_refs;
static void (*cleanup)(void *);
static void *cleanup_data;

static int devm_clk_bulk_get(struct device *dev, int n, struct clk_bulk_data *bulk)
{
	static const char *const names[] = {
	    "sc_top", "clk_disp", "clk_dsi", "dsi_esc", "disppll", "disp_src", "dsi_src", "cfg_reg",
	};
	(void)dev;
	assert(n == 7 || n == 8);
	for (int i = 0; i < n; i++) {
		assert(!strcmp(bulk[i].id, names[i]));
		bulk[i].clk = &bulk[i];
	}
	gets++;
	return get_error;
}

static int devm_add_action_or_reset(struct device *dev, void (*fn)(void *), void *data)
{
	(void)dev;
	actions++;
	if (action_error) {
		fn(data);
		return action_error;
	}
	cleanup = fn;
	cleanup_data = data;
	return 0;
}

static int clk_bulk_prepare_enable(int n, struct clk_bulk_data *bulk)
{
	assert((n == 7 || n == 8) && bulk);
	enables++;
	if (!enable_error)
		for (int i = 0; i < n; i++)
			if (!strcmp(bulk[i].id, "cfg_reg"))
				cfg_refs++;
	return enable_error;
}

static void clk_bulk_disable_unprepare(int n, struct clk_bulk_data *bulk)
{
	assert((n == 7 || n == 8) && bulk);
	for (int i = 0; i < n; i++)
		if (!strcmp(bulk[i].id, "cfg_reg")) {
			assert(cfg_refs);
			cfg_refs--;
		}
	disables++;
}

/* The optional clk_bt_vip gate: only ever enabled while the bulk (with its
 * register clock) is on, and released before the bulk.
 */
static struct clk_bulk_data bt_clock = {.id = "clk_bt_vip"};
static unsigned int bt_enables, bt_disables;
static int bt_error;
static int clk_prepare_enable(struct clk *clk)
{
	assert((void *)clk == &bt_clock && cfg_refs == 1);
	bt_enables++;
	return bt_error;
}
static void clk_disable_unprepare(struct clk *clk)
{
	assert((void *)clk == &bt_clock && cfg_refs == 1);
	bt_disables++;
}
#include "sophgo-clocks.h"

static void test_bt(void)
{
	struct device dev = {0};
	struct sophgo_clocks clocks = {0};

	assert(!bt_enables && !bt_disables); /* NULL bt: never touched above */
	assert(!sophgo_clocks_get(&dev, &clocks));
	clocks.bt = (struct clk *)&bt_clock;
	bt_error = -EIO;
	assert(sophgo_clocks_enable(&clocks) == -EIO);
	assert(!clocks.enabled && !clocks.bt_enabled && bt_enables == 1 && !bt_disables && !cfg_refs);
	bt_error = 0;
	assert(!sophgo_clocks_enable(&clocks) && clocks.enabled && clocks.bt_enabled);
	assert(!sophgo_clocks_enable(&clocks) && bt_enables == 2 && !bt_disables);
	sophgo_clocks_disable(&clocks);
	assert(!clocks.enabled && !clocks.bt_enabled && bt_disables == 1 && !cfg_refs);
	cleanup(cleanup_data);
	assert(bt_disables == 1);
	/* A readiness timeout on first enable releases the gate with the bulk. */
	pll_rate = 0;
	assert(sophgo_clocks_enable(&clocks) == -ETIMEDOUT);
	assert(!clocks.enabled && !clocks.bt_enabled && bt_enables == 3 && bt_disables == 2);
	pll_rate = 594000000;
	assert(!sophgo_clocks_enable(&clocks) && clocks.bt_enabled);
	cleanup(cleanup_data); /* devres release while enabled */
	assert(!clocks.bt_enabled && bt_disables == 3 && !cfg_refs);
}

int main(void)
{
	struct device dev = {0};
	struct sophgo_clocks clocks = {0};

	/* Missing provider, deferred provider, and allocation failure propagate. */
	const int errors[] = {-ENOENT, -517, -ENOMEM};
	for (unsigned int i = 0; i < ARRAY_SIZE(errors); i++) {
		get_error = errors[i];
		assert(sophgo_clocks_get(&dev, &clocks) == errors[i]);
		assert(!clocks.enabled && !actions && !enables && !disables);
	}
	get_error = 0;
	action_error = -ENOMEM;
	assert(sophgo_clocks_get(&dev, &clocks) == -ENOMEM);
	assert(!clocks.enabled && !enables && !disables);
	action_error = 0;
	assert(!sophgo_clocks_get(&dev, &clocks));
	assert(gets == 5 && actions == 2 && cleanup);

	/* Later probe failure: cleanup must not disable unenabled clocks. */
	cleanup(cleanup_data);
	assert(!disables);
	for (unsigned int i = 0; i < ARRAY_SIZE(errors); i++) {
		enable_error = errors[i];
		assert(sophgo_clocks_enable(&clocks) == errors[i]);
		assert(!clocks.enabled);
		sophgo_clocks_disable(&clocks);
		cleanup(cleanup_data);
		assert(!disables); /* no second unwind of CCF's partial failure */
	}
	enable_error = 0;
	assert(!sophgo_clocks_enable(&clocks) && clocks.enabled);
	assert(!sophgo_clocks_enable(&clocks) && enables == 4);
	/* Reproduce syscon takes then drops its own clock reference.
	 * Direct MAC access follows, so a pipeline reference must survive.
	 * Permit the old seven-clock acquisition above: the old production
	 * helper reaches and fails this behavioral assertion, not a name check.
	 */
	cfg_refs++;
	cfg_refs--;
	assert(cfg_refs == 1 && "MAC access after syscon needs a held register clock");
	sophgo_clocks_disable(&clocks);
	assert(!clocks.enabled && disables == 1 && !cfg_refs);
	cleanup(cleanup_data); /* remove after shutdown must not double-put */
	assert(disables == 1);
	assert(!sophgo_clocks_enable(&clocks));
	cleanup(cleanup_data); /* release while enabled */
	assert(!clocks.enabled && disables == 2 && enables == 5);
	/* Repeated enable must query hardware again and retain an existing
	 * reference until hardware quiesces; recovery can acquire it again.
	 */
	assert(!sophgo_clocks_enable(&clocks));
	unsigned int before = rate_reads;
	pll_rate = 0;
	assert(sophgo_clocks_enable(&clocks) == -ETIMEDOUT);
	assert(rate_reads > before && clocks.enabled && disables == 2 && cfg_refs == 1);
	/* Caller quiesces hardware before releasing the existing references. */
	sophgo_clocks_disable(&clocks);
	assert(!clocks.enabled && disables == 3);
	assert(sophgo_clocks_enable(&clocks) == -ETIMEDOUT);
	assert(!clocks.enabled && disables == 4);
	cleanup(cleanup_data);
	assert(disables == 4);
	pll_rate = 594000000;
	assert(!sophgo_clocks_enable(&clocks));
	cleanup(cleanup_data);
	assert(disables == 5 && !cfg_refs);
	test_bt();
	puts("PASS: uncached readiness on first/repeated enable, timeout unwind and "
	     "retry; clk_bt_vip held inside the bulk's ownership");
	puts("PASS: clock acquisition, deferred/error propagation, cleanup "
	     "registration failure, "
	     "enable failure/retry, balanced shutdown/devres");
	return 0;
}
