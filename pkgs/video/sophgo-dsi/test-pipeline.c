/* SPDX-License-Identifier: GPL-2.0-only */
static void assert_trace(const char *label);
#include "test-pipeline-shim.h"
#include <setjmp.h>
#include <stdarg.h>
#include <sys/types.h>
#define MIPI_DSI_MODE_VIDEO (1UL << 0)
#define MIPI_DSI_MODE_VIDEO_BURST (1UL << 1)
#define MIPI_DSI_MODE_LPM (1UL << 11)
#define MIPI_DSI_MODE_VIDEO_NO_HFP (1UL << 5)
#define MIPI_DSI_MODE_NO_EOT_PACKET (1UL << 9)
#define MIPI_DSI_FMT_RGB888 0
#include <drm/bridge/lt8912b.h>
static unsigned int disp[0xc4 / 4], mac_regs[3];
static bool stop_timeout, start_timeout, configure_error;
static unsigned int readl(const void *addr)
{
	assert(bulk_on);
	mmio++;
	/* Full PD write identifies completed LP11 preparation; DATA_OV stays untouched. */
	if (addr == mac_regs && configure_error && phy[0x64 / 4] == 0)
		return 1;
	return *(const unsigned int *)addr;
}
static void writel(unsigned int value, void *addr)
{
	assert(bulk_on);
	mmio++;
	if (addr == mac_regs) {
		if (*mac_regs & 4) {
			if (!stop_timeout)
				*mac_regs &= ~4U;
		} else {
			/* Ordering contract from vendor config -> enable, not silicon proof. */
			assert(disp[0] & 0x80);
			if (!start_timeout)
				*mac_regs = value;
		}
	} else
		*(unsigned int *)addr = value;
	if (addr == disp && !(value & 0x80))
		idle_done = true;
}
#include "sophgo-disp.h"
#include "sophgo-dphy.h"
#include "sophgo-vip.h"
#include "sophgo-mac.h"
#include "sophgo-link.h"
struct drm_bridge;
struct drm_atomic_commit;
void drm_atomic_bridge_chain_disable(struct drm_bridge *, struct drm_atomic_commit *);
void drm_atomic_bridge_chain_post_disable(struct drm_bridge *, struct drm_atomic_commit *);
struct drm_bridge *pipeline_chain_bridge(void);
#include "sophgo-pipeline.h"
void pipeline_bridge_setup(struct lt8912b_pipeline *, int);
void pipeline_bridge_pre(void);
void pipeline_bridge_enable(void);
void pipeline_bridge_disable(void);
void pipeline_bridge_post(void);
unsigned int pipeline_bridge_writes(void);
void pipeline_bridge_stopped(void);
void pipeline_chain_start(void);
void pipeline_chain_stop(void);
static struct sophgo_clocks clocks;
static struct sophgo_vip vip;
static struct sophgo_mac mac;
struct mutex {
	bool held;
};
static void mutex_lock(struct mutex *m)
{
	assert(!m->held);
	m->held = true;
}
static void mutex_unlock(struct mutex *m)
{
	assert(m->held);
	m->held = false;
}
struct sophgo_dsi {
	struct mutex commit_lock, host_lock;
	bool manual_selected, manual_request, terminal;
	void *drm, *peripheral;
	struct drm_bridge *host_bridge, *bridge;
	struct sophgo_pipeline pipeline;
	struct {
		void *dev;
	} host;
};
#define dev_err(dev, ...) ((void)(dev))
static jmp_buf quarantine;
static unsigned int quarantines;
static void sophgo_terminal_quarantine(struct sophgo_dsi *dsi)
{
	assert(sophgo_pipeline_owned(&dsi->pipeline) && !dsi->commit_lock.held);
	dsi->terminal = true;
	quarantines++;
	longjmp(quarantine, 1); /* kernel never returns: keep callback/devres alive */
}
#include "pipeline-shutdown.inc"
static struct sophgo_dsi owner;
struct device_attribute;
static ssize_t sysfs_emit(char *buf, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int ret = vsnprintf(buf, 4096, fmt, args);
	va_end(args);
	return ret;
}
static void *dev_get_drvdata(struct device *dev)
{
	(void)dev;
	return &owner;
}
static bool sysfs_streq(const char *a, const char *b)
{
	return !strncmp(a, b, strlen(b)) && (a[strlen(b)] == 0 || a[strlen(b)] == '\n');
}
#define DRM_MODE_FLAG_PHSYNC 1
#define DRM_MODE_FLAG_PVSYNC 2
struct drm_display_mode {
	unsigned int clock, hdisplay, hsync_start, hsync_end, htotal;
	unsigned int vdisplay, vsync_start, vsync_end, vtotal, flags;
};
static void drm_bridge_chain_mode_set(struct drm_bridge *b, const struct drm_display_mode *m,
				      const struct drm_display_mode *adjusted)
{
	assert(b && m == adjusted && m->clock == 74250 && m->hdisplay == 1280 &&
	       m->hsync_start == 1390 && m->hsync_end == 1430 && m->htotal == 1650 &&
	       m->vdisplay == 720 && m->vsync_start == 725 && m->vsync_end == 730 &&
	       m->vtotal == 750 && m->flags == 3);
}
void drm_atomic_bridge_chain_pre_enable(struct drm_bridge *, struct drm_atomic_commit *);
void drm_atomic_bridge_chain_enable(struct drm_bridge *, struct drm_atomic_commit *);
void pipeline_chain_setup(void);
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#include "manual.inc"
#define pipeline owner.pipeline
struct drm_bridge *pipeline_bridge_get(void);
void pipeline_bridge_fail_disable(bool);
void pipeline_bridge_rc_board(void);
void pipeline_bridge_fail_receiver_stop(bool);

static struct sophgo_dphy_lanes lanes;
static const struct sophgo_mac_mode mode = {
    74250,
    1280,
    1390,
    1430,
    1650,
    720,
    725,
    730,
    750,
    4,
    MIPI_DSI_FMT_RGB888,
    0,
    SOPHGO_MAC_BRIDGE_FLAGS | MIPI_DSI_MODE_VIDEO_BURST,
};
static char order[100];
static unsigned int order_n;
void pipeline_host_pre(void)
{
	order[order_n++] = 'P';
	int ret = sophgo_pipeline_prepare(&pipeline, &mode, &lanes, 0x60, false, false, true,
					  &hw[8], 20000000, 900000000);
	lt8912b_pipeline_error(&pipeline.status, "host prepare", ret);
	if (!ret) {
		assert(phy[0x44 / 4] == 0xa0a0a0a0 && phy[0x64 / 4] == 0);
		assert((disp[0] & 0x80) && mac_regs[2] == 0x800f00);
	}
}
void pipeline_host_enable(void)
{
	order[order_n++] = 'E';
	int ret = sophgo_pipeline_enable(&pipeline);
	lt8912b_pipeline_error(&pipeline.status, "host enable", ret);
	if (!ret)
		assert((mac_regs[0] & 4) && (disp[0] & 0x80) && !(phy[0x44 / 4] & 0x1f1f1f1f));
}
void pipeline_host_disable(void)
{
	order[order_n++] = 'D';
	sophgo_pipeline_disable(&pipeline);
}
void pipeline_host_post(void)
{
	order[order_n++] = 'U';
	sophgo_pipeline_unprepare(&pipeline);
}
void pipeline_boundary(char c) { order[order_n++] = c; }
static const char *trace_stack[32];
static unsigned int trace_depth, trace_calls;
static void assert_trace(const char *label)
{
	assert(trace_depth && !strcmp(trace_stack[trace_depth - 1], label));
}
static void trace_event(void *ctx, const char *label, bool returned)
{
	struct sophgo_trace *trace = &pipeline.link.trace;
	char status[4096];
	(void)ctx;
	if (returned)
		trace->returned = label;
	else
		trace->entered = label;
	/* Actual manual calls hold commit_lock here. This must stay readable. */
	assert(pinstripe_progress_show(NULL, NULL, status) > 0);
	assert(strstr(status, "enabled=1"));
	if (returned) {
		assert(trace_depth && !strcmp(trace_stack[--trace_depth], label));
	} else {
		assert(trace_depth < ARRAY_SIZE(trace_stack));
		trace_stack[trace_depth++] = label;
		trace_calls++;
	}
}
static unsigned int start_failures;
static void start_failure(struct sophgo_pipeline *p, unsigned int cfg, int error) {
	assert(bulk_on && clocks.enabled && p->link.video_owned);
	assert(p->status.prepared && p->display_owned && (cfg & 0x80));
	assert(error == -ETIMEDOUT);
	assert(p->mac->start_initial == 0 && p->mac->start_final == 0);
	assert(p->mac->start_poll_us == 20000);
	start_failures++;
}
static char snapshot_labels[8][24];
static unsigned int snapshots;
static void snapshot(struct sophgo_pipeline *p, const char *label)
{
	/* Every snapshot point is inside the clock-owned pipeline. */
	assert(bulk_on && clocks.enabled && p->link.clocks_owned && p->display_owned);
	assert(snapshots < 8 && strlen(label) < 24);
	strcpy(snapshot_labels[snapshots++], label);
}
static void setup(int bridge_fault)
{
	const unsigned int roles[] = {1, 2, 0, 3, 4};
	memset(&owner, 0, sizeof(owner));
	owner.drm = owner.peripheral = &owner;
	owner.manual_selected = true;
	pipeline_chain_setup();
	owner.host_bridge = pipeline_chain_bridge();
	owner.bridge = pipeline_bridge_get();
	memset(&clocks, 0, sizeof(clocks));
	memset(hw, 0, sizeof(hw));
	memset(phy, 0xa0, sizeof(phy));
	memset(top, 0xa0, sizeof(top));
	memset(disp, 0xa0, sizeof(disp));
	memset(mac_regs, 0, sizeof(mac_regs));
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
	assert(!sophgo_mac_init(&mac, mac_regs, sizeof(mac_regs)));
	assert(!sophgo_dphy_calc_lanes(roles, 0, 1, 0, &lanes));
	pipeline.mac = &mac;
	pipeline.start_failure = start_failure;
	start_failures = 0;
	pipeline.snapshot = snapshot;
	snapshots = 0;
	pipeline.disp = disp;
	pipeline.link =
	    (struct sophgo_link){.clocks = &clocks, .vip = &vip, .phy = phy, .top = top};
	pipeline.link.trace.emit = trace_event;
	trace_depth = trace_calls = 0;
	step = fail_at = bulk_on = bt_on = mmio = order_n = reads = updates = 0;
	idle_done = stop_timeout = start_timeout = configure_error = false;
	pipeline_bridge_setup(&pipeline.status, bridge_fault);
}
static void run(void)
{
	pipeline_chain_start();
	sophgo_pipeline_complete(&pipeline, pipeline_chain_bridge());
}
static void stopped(void)
{
	assert(!trace_depth && trace_calls);
	assert(!bulk_on && !bt_on && !pipeline.link.phy_owned && !pipeline.link.video_owned);
	assert(!pipeline.display_owned);
	for (unsigned int i = 0; i < 8; i++)
		assert(!hw[i].held);
	assert(map.regs[0] == 4 && map.regs[6] == 0xa5a5a5ef);
	assert(hw[3].rate == 25000000 && hw[5].rate == 148500000);
	pipeline_bridge_stopped();
}
int main(void)
{
	setup(0);
	run();
	assert(!pipeline.status.error && pipeline.status.started && (disp[0] & 0x80));
	assert(snapshots == 2 && !strcmp(snapshot_labels[0], "prepared"));
	assert(!strcmp(snapshot_labels[1], "started"));
	assert(mac_regs[2] == 0x800f00);
	assert((top[0x70 / 4] & ~15U) == (0xa0a0a0a0 & ~15U));
	/* vendor scaler-top/DISP init reached the hardware before the
	 * MAC request, in golden values, reading the working bank. Initialization includes the
	 * reg_shrd_sel raw-register select (full SHD write 0x200) between the CFG
	 * writes and reg_done/force_up, so force_up leaves SHD = 0x2ff (bit 9 kept,
	 * sentinel cleared).
	 */
	assert(top[0x00 / 4] == 0x80000009 && top[0x04 / 4] == 0x00ff1200);
	assert(top[0x10 / 4] == 0x000002ff && top[0x08 / 4] == 0xa0a0a0a0);
	assert((disp[0] & 0x4f0ff) == 0x420e0); /* golden DISP_CFG with TGEN on */
	assert((disp[0xc0 / 4] & 1) == 1);
	int stages = step;
	unsigned int bridge_writes = pipeline_bridge_writes();
	pipeline_chain_stop();
	assert(order_n == 8 && !memcmp(order, "PpEedDuU", 8));
	stopped();
	pipeline_chain_stop();
	stopped();
	assert(!sophgo_pipeline_retry(&pipeline));
	run();
	assert(pipeline.status.started);
	pipeline_chain_stop();
	stopped();
	for (int i = 1; i <= stages; i++) {
		setup(0);
		fail_at = i;
		run();
		assert(pipeline.status.error && !pipeline.status.started &&
		       !pipeline_bridge_writes());
		fail_at = 0;
		pipeline_chain_stop();
		stopped();
	}
	for (int i = -7; i <= (int)bridge_writes; i++) {
		if (!i)
			continue;
		setup(i);
		run();
		assert(pipeline.status.error && !pipeline.status.started);
		if (i > 0)
			assert(pipeline_bridge_writes() == (unsigned int)i);
		pipeline_chain_stop();
		stopped();
	}
	/* Escape parent/rate restore, then shared divider update/readback. */
	for (int i = 1; i <= 4; i++) {
		setup(0);
		run();
		fail_at = step + i;
		pipeline_chain_stop();
		assert(pipeline.status.error && bulk_on);
		fail_at = 0;
		pipeline_chain_stop();
		stopped();
	}

	setup(0);
	configure_error = true;
	run();
	assert(pipeline.status.error == -EBUSY && !pipeline_bridge_writes());
	assert(bulk_on && pipeline.link.phy_owned && pipeline.link.video_owned);
	configure_error = false;
	pipeline_chain_stop();
	stopped();

	setup(0);
	hw[4].rate = 0;
	run();
	assert(pipeline.status.error == -ETIMEDOUT && !pipeline_bridge_writes());
	pipeline_chain_stop();
	stopped();
	setup(0);
	start_timeout = true;
	run();
	assert(pipeline.status.error == -ETIMEDOUT && !pipeline.status.started);
	assert(start_failures == 1 && !strcmp(pipeline.status.stage, "MAC start"));
	assert(!(disp[0] & 0x80) && !pipeline.display_owned);
	assert(snapshots == 2 && !strcmp(snapshot_labels[0], "prepared"));
	assert(!strcmp(snapshot_labels[1], "start-failed"));
	pipeline_chain_stop();
	stopped();
	setup(0);
	run();
	stop_timeout = true;
	pipeline_chain_stop();
	assert(bulk_on && pipeline.link.phy_owned);
	assert((disp[0] & 0x80) && pipeline.display_owned);
	stop_timeout = false;
	pipeline_chain_stop();
	stopped();
	setup(0);
	run();
	owner.host_bridge = pipeline_chain_bridge();
	owner.bridge = pipeline_bridge_get();
	sophgo_pipeline_shutdown(&owner);
	stopped();
	sophgo_pipeline_shutdown(&owner);
	stopped();
	setup(0);
	run();
	pipeline_bridge_fail_disable(true);
	pipeline_chain_stop();
	assert(bulk_on && pipeline.status.supplies_owned);
	assert(sophgo_pipeline_retry(&pipeline) == -EBUSY);
	pipeline_bridge_fail_disable(false);
	pipeline_chain_stop();
	stopped();
	assert(!sophgo_pipeline_retry(&pipeline));

	char status[4096];
	/* Actual sysfs command body executes the production pipeline/chain. */
	setup(0);
	owner.manual_selected = false;
	assert(pinstripe_store(NULL, NULL, "start", 5) == -EOPNOTSUPP);
	owner.manual_selected = true;
	assert(pinstripe_store(NULL, NULL, "720p", 4) == -EINVAL);
	assert(!mmio && !step);
	assert(pinstripe_store(NULL, NULL, "start\n", 6) == 6);
	assert(pipeline.status.started && owner.manual_request);
	/* The manual command adds the locked-state snapshot after both started. */
	assert(snapshots == 3 && !strcmp(snapshot_labels[2], "complete"));
	assert(pinstripe_show(NULL, NULL, status) > 0);
	assert(strstr(status, "started=1 owned=1") && strstr(status, "error=0"));
	assert(pinstripe_store(NULL, NULL, "start", 5) == -EBUSY);
	stop_timeout = true;
	assert(pinstripe_store(NULL, NULL, "stop", 4) == -ETIMEDOUT);
	assert(pinstripe_store(NULL, NULL, "start", 5) == -EBUSY);
	stop_timeout = false;
	assert(pinstripe_store(NULL, NULL, "stop", 4) == 4);
	assert(!owner.manual_request && !sophgo_pipeline_owned(&pipeline));
	assert(pinstripe_store(NULL, NULL, "start", 5) == 5);
	assert(pinstripe_store(NULL, NULL, "stop", 4) == 4);
	stopped();
	setup(1);
	assert(pinstripe_store(NULL, NULL, "start", 5) == -EIO);
	assert(!pipeline.status.started);
	assert(snapshots == 1 && !strcmp(snapshot_labels[0], "prepared"));
	assert(pinstripe_store(NULL, NULL, "stop", 4) == 4);
	stopped();

	/* Repeated permanent MAC, rail and local-restore faults never return
	 * to encoder cleanup, handshake unbind or host/I2C devres release. */
	for (volatile int fault = 0; fault < 4; fault++) {
		setup(0);
		if (fault == 3)
			pipeline_bridge_rc_board();
		run();
		if (fault == 0)
			stop_timeout = true;
		if (fault == 1)
			pipeline_bridge_fail_disable(true);
		if (fault == 2)
			update_error = -EIO;
		if (fault == 3)
			pipeline_bridge_fail_receiver_stop(true);
		for (volatile int attempt = 0; attempt < 3; attempt++) {
			if (!setjmp(quarantine)) {
				sophgo_pipeline_shutdown(&owner);
				assert(!"terminal teardown returned with live ownership");
			}
			assert(owner.terminal && bulk_on && sophgo_pipeline_owned(&pipeline));
			assert(pinstripe_show(NULL, NULL, status) > 0);
			assert(strstr(status, "terminal=1") && strstr(status, "owned=1"));
		}
		stop_timeout = false;
		pipeline_bridge_fail_disable(false);
		update_error = 0;
		pipeline_bridge_fail_receiver_stop(false);
		pipeline_chain_stop();
		stopped();
	}
	assert(quarantines == 12);

	setup(0);
	for (int event = 0; event < 2; event++)
		for (int fb = 0; fb < 2; fb++)
			for (int allowed = 0; allowed < 2; allowed++)
				assert((sophgo_pipeline_validate(&mode, &lanes, 0x60, event, fb,
								 allowed) == 0) ==
				       (!event && !fb && allowed));
	for (unsigned int i = 0; i < 13; i++) {
		struct sophgo_mac_mode bad = mode;
		/* All twelve scalar timing/link words precede flags. */
		if (i < 12)
			((unsigned int *)&bad)[i]++;
		else
			bad.flags ^= MIPI_DSI_MODE_VIDEO_BURST;
		assert(sophgo_pipeline_validate(&bad, &lanes, 0x60, false, false, true));
	}
	lanes.pn ^= 1;
	assert(sophgo_pipeline_validate(&mode, &lanes, 0x60, false, false, true));
	assert(!mmio && !step);
	puts("PASS: composed production host/bridge pipeline through Linux 7.2.2 "
	     "chain helpers; actual manual start/stop and twelve terminal fault containments, "
	     "golden order, every prepare/I2C/rail failure, "
	     "lock/start/stop timeouts, partial unwind, repeated stop, invalid "
	     "inputs and activation holds");
}
